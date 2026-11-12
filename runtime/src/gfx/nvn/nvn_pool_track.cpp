// SPDX-License-Identifier: GPL-3.0-or-later
// Write tracking of copy pools (nvn_pool_track.h).
#include "nvn_pool_track.h"

#include <algorithm>
#include <atomic>
#include <memory>

#include "nxrt/config.h"
#include "nxrt/write_watch.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr uint64_t kPage = 4096;
// A write fault and the protection change after it cost far more than copying a page (Proton: ~160 faults and ~115 protection changes a
// frame made the Steam Deck slower than copying everything). So a page turns hot on the second write found within kHotWindow presents:
// data written once and then left (a mesh loaded into reused memory) stays tracked, a ring the game refills (whatever its period up to
// the window) faults twice. A hot page is tried again after kHotCool presents; a page that was hot before turns hot again at its first
// write, for twice as long each time (up to kHotCool << kHotMaxShift), so a ring costs one fault per page per period.
constexpr uint32_t kHotWindow = 600;
constexpr uint32_t kHotCool = 1800;
constexpr uint32_t kHotMaxShift = 5;
uint32_t hot_presents(uint8_t level) { return kHotCool << std::min<uint32_t>(level ? level - 1u : 0u, kHotMaxShift); }

std::atomic<uint64_t> g_arms{0}, g_armed_pages{0}, g_rewrites{0}, g_unarmable{0}, g_land_kept{0}, g_land_differ{0};
std::atomic<int64_t> g_hot_now{0};

}  // namespace

struct PoolTrack {
    mem::ww::Region* region = nullptr;
    uint64_t npages = 0;
    bool stopped = false;
    std::unique_ptr<uint32_t[]> gen;      // 0: never armed
    std::unique_ptr<uint32_t[]> mark;     // present count of the last write found (hot pages: when they became hot)
    std::unique_ptr<uint8_t[]> struck;    // a write was found before (mark says when)
    std::unique_ptr<uint8_t[]> level;     // times it turned hot
    std::unique_ptr<uint8_t[]> hot;
};

bool pool_track_on() {
    static const bool on = config_get_bool("nvn", "pool_copy_track", true);
    return on;
}

PoolTrack* ptrack_create(const uint8_t* base, uint64_t size) {
    if (!pool_track_on() || !size) return nullptr;
    mem::ww::Region* r = mem::ww::region_create(reinterpret_cast<uint64_t>(base), size);
    if (!r) return nullptr;
    auto* t = new PoolTrack();
    t->region = r;
    t->npages = (size + kPage - 1) / kPage;
    t->gen.reset(new uint32_t[t->npages]());
    t->mark.reset(new uint32_t[t->npages]());
    t->struck.reset(new uint8_t[t->npages]());
    t->level.reset(new uint8_t[t->npages]());
    t->hot.reset(new uint8_t[t->npages]());
    return t;
}

void ptrack_destroy(PoolTrack* t) {
    if (!t) return;
    int64_t hot = 0;
    for (uint64_t p = 0; p < t->npages; ++p) hot += t->hot[p];
    g_hot_now.fetch_sub(hot, std::memory_order_relaxed);
    if (t->region) mem::ww::region_destroy(t->region);
    delete t;
}

void ptrack_stop(PoolTrack* t) {
    if (!t || t->stopped) return;
    t->stopped = true;
    if (t->region) mem::ww::region_destroy(t->region);
    t->region = nullptr;
}

bool ptrack_current(const PoolTrack* t, uint64_t page, uint32_t copied) {
    if (!copied || t->stopped || page >= t->npages || t->hot[page] || t->gen[page] != copied) return false;
    return mem::ww::page_valid(t->region, page);
}

void ptrack_before_copy(PoolTrack* t, uint64_t first, uint64_t count, uint32_t frame, uint32_t* gens) {
    if (t->stopped || first >= t->npages || count > t->npages - first) {
        for (uint64_t i = 0; i < count; ++i) gens[i] = 0;
        return;
    }
    // Pages to arm (not hot, and not armed-and-unwritten already) are armed in runs: one protection change per run.
    uint64_t run = 0;        // start (index into the range) of the current run, valid while in_run
    bool in_run = false;
    const auto arm_run = [&](uint64_t end) {
        if (!in_run) return;
        in_run = false;
        const uint64_t n = end - run;
        g_arms.fetch_add(1, std::memory_order_relaxed);
        if (mem::ww::arm(t->region, first + run, n)) {
            g_armed_pages.fetch_add(n, std::memory_order_relaxed);
            return;
        }
        // one page of the run cannot be armed (another view of it exists, a file read into it is in progress, the guest changed its
        // protection): page by page, and those that fail are copied at every use
        for (uint64_t i = run; i < end; ++i) {
            if (mem::ww::arm(t->region, first + i, 1)) {
                g_armed_pages.fetch_add(1, std::memory_order_relaxed);
            } else {
                gens[i] = 0;
                g_unarmable.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t p = first + i;
        if (t->hot[p]) {
            if (frame - t->mark[p] <= hot_presents(t->level[p])) {
                gens[i] = 0;
                arm_run(i);
                continue;
            }
            t->hot[p] = 0;
            g_hot_now.fetch_sub(1, std::memory_order_relaxed);
        } else if (t->gen[p] && mem::ww::page_valid(t->region, p)) {
            gens[i] = t->gen[p];           // armed and unwritten (another shadow copied it): this copy is of the same generation
            arm_run(i);
            continue;
        } else if (t->gen[p]) {            // copied before and written since
            g_rewrites.fetch_add(1, std::memory_order_relaxed);
            const bool again = t->level[p] != 0 || (t->struck[p] && frame - t->mark[p] <= kHotWindow);
            t->struck[p] = 1;
            t->mark[p] = frame;
            if (again) {
                if (t->level[p] < 255) ++t->level[p];
                t->hot[p] = 1;
                g_hot_now.fetch_add(1, std::memory_order_relaxed);
                gens[i] = 0;
                arm_run(i);
                continue;
            }
        }
        if (++t->gen[p] == 0) t->gen[p] = 1;
        gens[i] = t->gen[p];
        if (!in_run) {
            in_run = true;
            run = i;
        }
    }
    arm_run(count);
}

void ptrack_gpu_write(PoolTrack* t, uint64_t page, uint32_t* gen) {
    if (t->stopped || page >= t->npages || t->hot[page] || !t->gen[page] || !mem::ww::page_valid(t->region, page)) return;   // nobody is current
    const bool current = *gen == t->gen[page];
    if (++t->gen[page] == 0) t->gen[page] = 1;
    if (current) *gen = t->gen[page];
}

uint64_t ptrack_land_begin(PoolTrack* t, uint64_t first, uint64_t count) {
    if (!t || t->stopped || !mem::ww::page_valid(t->region, first)) return 0;
    return mem::ww::owner_write_begin(t->region, first, count);
}

bool ptrack_land_end(PoolTrack* t, uint64_t first, uint64_t count) {
    const bool ok = mem::ww::owner_write_end(t->region, first, count);
    if (ok) g_land_kept.fetch_add(count, std::memory_order_relaxed);
    else g_unarmable.fetch_add(count, std::memory_order_relaxed);
    return ok;
}

void ptrack_land_differs(PoolTrack* t, uint64_t first, uint64_t count) {
    mem::ww::invalidate(t->region, first, count);
    g_land_differ.fetch_add(count, std::memory_order_relaxed);
}

PoolTrackStats ptrack_stats() {
    PoolTrackStats s;
    s.arms = g_arms.load(std::memory_order_relaxed);
    s.armed_pages = g_armed_pages.load(std::memory_order_relaxed);
    s.rewrites = g_rewrites.load(std::memory_order_relaxed);
    s.hot_now = static_cast<uint64_t>(std::max<int64_t>(0, g_hot_now.load(std::memory_order_relaxed)));
    s.unarmable = g_unarmable.load(std::memory_order_relaxed);
    s.land_kept = g_land_kept.load(std::memory_order_relaxed);
    s.land_differ = g_land_differ.load(std::memory_order_relaxed);
    return s;
}

}  // namespace nxrt::nvn::be::gpu
