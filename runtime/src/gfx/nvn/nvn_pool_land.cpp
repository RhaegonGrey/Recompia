// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] pool_copy: GPU writes go back into guest memory ("landing"; the rules are at the top of nvn_pool_copy.cpp). The render thread
// records every range a command may write in pool memory with its submission's serial (land_record). Once the timeline reaches that
// serial, the range is copied from the shadow into guest memory, before the serial is published as complete: by whichever thread reads
// the timeline first (pcopy_land from read_completed), which is the nvn-pool-land thread here unless a game thread polls in the moment
// between the GPU finishing and that thread waking up.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"
#include "nxrt/config.h"
#include "nvn_gpu_internal.h"
#include "nvn_pool_copy_impl.h"
#include "nvn_pool_track.h"

namespace nxrt::nvn::be::gpu {
namespace {

using pcopy::kPage;
constexpr uint64_t kNone = ~0ull;         // g_land_min with no record
// [nvn] pool_copy_land_keep: a record of at most this many bytes has every page it kept compared after the copy; a larger one only the
// bytes of its first and last page that lie outside the record (see land_one).
constexpr uint64_t kCompareAll = 64 * 1024;

// GPU writes waiting for their serial, in serial order (recorded by the render thread as it records). Lock order:
// g_live_mu -> g_reg_mu -> g_landing_mu -> g_land_mu. g_land_mu guards only the queue, land_refs and g_land_min and is held for short sections
// (the render thread takes it per recorded write and in pcopy_busy); it is a leaf. g_landing_mu serialises landers and is held across a whole
// landing (pop, memcpy, before_os_write, page bits); before_os_write could call the compute-out guard handler, which copy pools never arm.
struct LandRec {
    PoolGpu* pool;
    PoolChunk* chunk;
    uint64_t pool_off, size, serial;
};
std::mutex g_land_mu;
std::mutex g_landing_mu;
std::deque<LandRec> g_land;
std::vector<LandRec> g_landing;                 // the batch a lander took out of g_land and is copying (under g_land_mu; one lander at a time)
std::atomic<uint64_t> g_land_min{kNone};        // serial of g_land.front() (kNone: empty), the lock-free test of pcopy_land
thread_local bool t_landing = false;            // a landing on this thread (a nested read_completed returns at once)

// the landing thread
std::mutex g_th_mu;
std::condition_variable g_th_cv;
std::thread g_th;
bool g_th_stop = false;

void land_min_update() { g_land_min.store(g_land.empty() ? kNone : g_land.front().serial, std::memory_order_release); }

void drop_all_records() {
    std::lock_guard<std::mutex> lk(g_land_mu);
    for (const LandRec& r : g_land)
        if (r.chunk->land_refs) --r.chunk->land_refs;
    g_land.clear();
    land_min_update();
}

// One record goes from its shadow into guest memory. Returns the parts that could not be written.
//
// [nvn] pool_copy_land_keep: the bytes come from the shadow, so a page that was current before (armed by the write tracker, unwritten)
// is current after, and no bind copies it back. Such runs of pages are made writable through the tracker (ptrack_land_begin), which
// keeps them out of the written record, and armed again after the copy. The tracker sees no CPU write made in between, so afterwards
// the CPU's bytes are compared with the shadow's where a game may write while this runs: the bytes of the record's first and last page
// outside the record, which are no part of the GPU's write and may hold other data; and for a small record (an event word, a report,
// a counter the game polls in memory and may answer at once) all of its kept pages. A page that differs counts as written. Inside a
// large record the game does not write before the serial is published, which happens after this returns; a write made there while the
// GPU's write was in flight was lost before too (the limits at the top of nvn_pool_copy.cpp).
// Without it every page of the record is written as an operating-system write: it counts as written, the next bind copies it into the
// shadow again, and a page that happens to twice turns hot.
struct KeptRun { PoolGpu* pool; uint64_t first, count; };
uint64_t land_one(PoolGpu* pool, const PoolChunk* c, uint64_t off, uint64_t size, std::vector<KeptRun>& kept) {
    uint8_t* const base = pool->base;
    const auto shadow = [c](uint64_t pool_off) { return static_cast<const uint8_t*>(c->shadow.mapped) + (pool_off - c->off); };
    const uint64_t p0 = off / kPage, p1 = (off + size - 1) / kPage;
    const size_t k0 = kept.size();
    if (PoolTrack* trk = pcopy::land_keep() ? pool->track : nullptr)
        for (uint64_t p = p0; p <= p1;) {
            const uint64_t n = ptrack_land_begin(trk, p, p1 - p + 1);
            if (n) kept.push_back(KeptRun{pool, p, n});
            p += n ? n : 1;
        }
    mem::ww::before_os_write(base + off, size);     // pages a write watch made read-only become writable (and count as written)
    const uint64_t bad = pcopy::guarded_copy(base + off, shadow(off), size, false);
    const uint64_t lo = c->off, hi = c->off + c->size;     // what the shadow holds
    const auto same = [&](uint64_t a, uint64_t b) {
        a = std::max(a, lo);
        b = std::min(b, hi);
        return a >= b || pcopy::guarded_same(base + a, shadow(a), b - a);
    };
    for (size_t k = k0; k < kept.size();) {
        KeptRun& r = kept[k];
        bool ok = ptrack_land_end(pool->track, r.first, r.count);
        if (ok && size <= kCompareAll && !same(r.first * kPage, (r.first + r.count) * kPage)) {
            ptrack_land_differs(pool->track, r.first, r.count);
            ok = false;
        }
        if (ok && size > kCompareAll) {      // more than one page (p0 < p1): the run loses an end page that differs
            if (r.first == p0 && !same(p0 * kPage, off)) {
                ptrack_land_differs(pool->track, p0, 1);
                ++r.first;
                --r.count;
            }
            if (r.count && r.first + r.count - 1 == p1 && !same(off + size, (p1 + 1) * kPage)) {
                ptrack_land_differs(pool->track, p1, 1);
                --r.count;
            }
            ok = r.count != 0;
        }
        if (ok) ++k;
        else kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(k));
    }
    return bad;
}

// [nvn] pool_land_slice_ms: the longest single wait for the serial of the oldest record. The wait ends the moment the timeline reaches
// the serial, so its length adds nothing to a landing; it only bounds how long the thread takes to see a stop or a lost device. The
// queue is in serial order and a record is only ever added behind the oldest, so the serial waited for is never too late a one. A wait
// that ran its whole length is followed by the next one at once: only a wait that came back early without the serial (an error) is
// followed by a 1 ms pause. 0: the earlier loop, 5 ms waits with a 1 ms pause after each one that did not reach the serial.
void land_loop() {
    log_set_thread_name("nvn-pool-land");
    const int64_t slice_ms = std::clamp<int64_t>(config_get_int("nvn", "pool_land_slice_ms", 50), 0, 1000);
    const bool pause_always = slice_ms == 0;
    const uint64_t slice_ns = static_cast<uint64_t>(pause_always ? 5 : slice_ms) * 1000 * 1000;
    if (!pause_always)
        NXLOG_I("nvn", "pool landing: waits for a serial up to %lld ms at a time, no pause after a wait that ran out ([nvn] pool_land_slice_ms)",
                static_cast<long long>(slice_ms));
    std::unique_lock<std::mutex> lk(g_th_mu);
    while (!g_th_stop) {
        const uint64_t s = g_land_min.load(std::memory_order_acquire);
        if (s == kNone) {
            g_th_cv.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }
        lk.unlock();
        // A word the game polls in memory with no API call (an event the GPU signals, a report) lands within a few ms of its serial.
        const auto t0 = std::chrono::steady_clock::now();
        if (timeline_wait(s, slice_ns)) {
            poll_completed();
        } else if (device().device_lost()) {   // nothing lands after a device loss: the records go
            drop_all_records();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else if (pause_always || std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // not a timeout (an error returns at once): no busy loop
        }
        lk.lock();
    }
}

}  // namespace

namespace pcopy {

void land_start() {
    std::lock_guard<std::mutex> lk(g_th_mu);
    if (g_th.joinable()) return;
    g_th_stop = false;
    g_th = std::thread(land_loop);
}

void land_stop() {
    {
        std::lock_guard<std::mutex> lk(g_th_mu);
        g_th_stop = true;
    }
    g_th_cv.notify_all();
    if (g_th.joinable()) g_th.join();
    {
        std::lock_guard<std::mutex> lk(g_th_mu);
        g_th_stop = false;            // a later back end (tests bring it up and down) starts its own
    }
    drop_all_records();               // the device is idle and the pools go next
}

bool land_record(PoolGpu* pg, PoolChunk* c, uint64_t off, uint64_t size, uint64_t serial) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(g_land_mu);
        // the same range bound again in this submission (a storage buffer of every draw): one record
        const size_t n = g_land.size();
        for (size_t i = n; i-- > 0 && i + 8 >= n;) {
            const LandRec& r = g_land[i];
            if (r.serial != serial) break;
            if (r.chunk == c && r.pool_off <= off && off + size <= r.pool_off + r.size) return false;
        }
        first = g_land.empty();
        g_land.push_back(LandRec{pg, c, off, size, serial});
        ++c->land_refs;
        if (first) land_min_update();
    }
    shared().records.fetch_add(1, std::memory_order_relaxed);
    if (first) {
        std::lock_guard<std::mutex> lk(g_th_mu);     // under the mutex the lander waits on: no wake-up lost between its test and its wait
        g_th_cv.notify_one();
    }
    return true;
}

// Records of a chunk that goes (its pool is destroyed) do not land; an idle drop or an eviction happens only with none left. A landing
// holds g_landing_mu from taking its records out of the queue until its last copy, so once this has had the mutex no lander reads the
// chunk's shadow: the caller may destroy it.
void land_forget(PoolChunk& c) {
    std::lock_guard<std::mutex> landing(g_landing_mu);
    std::lock_guard<std::mutex> lk(g_land_mu);
    g_land.erase(std::remove_if(g_land.begin(), g_land.end(), [&c](const LandRec& r) { return r.chunk == &c; }), g_land.end());
    land_min_update();
}

bool land_keep() {
    static const bool on = config_get_bool("nvn", "pool_copy_land_keep", true);
    return on;
}

LandingPause::LandingPause() { g_landing_mu.lock(); }
LandingPause::~LandingPause() { g_landing_mu.unlock(); }

size_t land_pending() {
    std::lock_guard<std::mutex> lk(g_land_mu);
    return g_land.size();
}

void land_busy(const PoolGpu* pg, uint64_t lo, uint64_t hi, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    std::lock_guard<std::mutex> lk(g_land_mu);
    const auto add = [&](const LandRec& r) {
        if (r.pool == pg && r.pool_off < hi && r.pool_off + r.size > lo) out.emplace_back(std::max(r.pool_off, lo), std::min(r.pool_off + r.size, hi));
    };
    for (const LandRec& r : g_land) add(r);
    for (const LandRec& r : g_landing) add(r);
}

}  // namespace pcopy

bool pcopy_busy(const PoolChunk& c) {
    std::lock_guard<std::mutex> lk(g_land_mu);
    return c.land_refs != 0;
}

uint64_t pcopy_land(uint64_t reached) {
    if (t_landing || g_land_min.load(std::memory_order_acquire) > reached) return reached;
    t_landing = true;
    const uint64_t t0 = ft::on() ? ft::now_ns() : 0;
    uint64_t bytes = 0, bad = 0;
    {
        std::lock_guard<std::mutex> landing(g_landing_mu);
        // The records leave the queue now, but g_land_min and land_refs stay as they were until the copy is done: a second caller that tests
        // g_land_min still waits on g_landing_mu, so no serial is published before its bytes landed, and pcopy_busy holds the chunks.
        // g_landing shows the batch to land_busy from the moment it leaves the queue until its bytes are in guest memory (#531).
        std::vector<LandRec>& batch = g_landing;
        std::vector<KeptRun> kept;
        {
            std::lock_guard<std::mutex> lk(g_land_mu);
            while (!g_land.empty() && g_land.front().serial <= reached) {
                batch.push_back(g_land.front());
                g_land.pop_front();
            }
        }
        // pass 1: every copy. Two records of a serial can share a page: its page_gpu must stay set until both have landed, or a touch between
        // them sees 0 and copies guest -> shadow over the page while the second record still lands stale bytes.
        for (const LandRec& r : batch) {
            bad += land_one(r.pool, r.chunk, r.pool_off, r.size, kept);
            bytes += r.size;
        }
        // pass 2: page_gpu back to 0 where no later write was recorded: after every copy, so a touch that sees 0 copies the landed bytes
        for (const LandRec& r : batch)
            for (uint64_t p = r.pool_off / kPage; p <= (r.pool_off + r.size - 1) / kPage; ++p) {
                r.pool->page_landed[static_cast<size_t>(p)].store(1, std::memory_order_relaxed);
                uint64_t e = r.serial;
                r.pool->page_gpu[static_cast<size_t>(p)].compare_exchange_strong(e, 0, std::memory_order_release, std::memory_order_relaxed);
            }
        for (const KeptRun& r : kept)      // no copy follows these
            for (uint64_t p = r.first; p < r.first + r.count; ++p) r.pool->page_landed[static_cast<size_t>(p)].store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_land_mu);
            for (const LandRec& r : batch)
                if (r.chunk->land_refs) --r.chunk->land_refs;
            batch.clear();
            land_min_update();
        }
    }
    t_landing = false;
    if (bad) pcopy::shared().unreadable.fetch_add(bad, std::memory_order_relaxed);
    if (bytes) {
        pcopy::shared().landed.fetch_add(bytes, std::memory_order_relaxed);
        if (t0) {
            static const uint16_t z = ft::zone_id("pool.copy.land");
            ft::event(z, t0, ft::now_ns(), static_cast<uint32_t>(std::min<uint64_t>(bytes >> 10, 0xffffffffu)));
        }
    }
    return reached;
}

}  // namespace nxrt::nvn::be::gpu
