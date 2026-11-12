// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] pool_copy = auto (default: Wine, or an integrated GPU with <=8 GiB of device-local memory) | on | off: memory pools as copies (shadows)
// instead of imports. Under Proton every vkQueueSubmit costs more the more userptr memory is imported (amdgpu revalidates every userptr BO of the
// submission's BO list): with whole pools imported (3.8 GiB) the game froze inside vkQueueSubmit, and the lazy pool's ~2 GiB of live imports
// still cost ~17.8 ms per batch submission on the Deck. Here a pool keeps the lazy pool's cell structure (nvn_gpu_pool.cpp: cells, ext,
// cell_ext, spans, idle drop), but each cell holds a host-visible Vulkan buffer of its own (MemoryUse::Shadow, handed out by
// nvn_pool_stock.cpp) instead of an import of the guest pages, so no submission carries a userptr BO.
//
// The rules that keep the shadow right:
//  * touch (resolve_in_pool, render thread): every resolve copies the pages of the range it binds from guest memory into the shadow, unless
//    a copy that serves the submission being recorded exists (page_copied), or the CPU has not written it since this shadow copied
//    it (the write tracker, nvn_pool_track.h; [nvn] pool_copy_track). A bind of unknown size touches to the end of its chunk. Without
//    the tracker every bind copied its pages again in every submission: ~49 MiB a frame on a Steam Deck, of which ~1.6 MiB had changed.
//    On a page with a GPU write still in flight (page_gpu above the completed serial) the shadow holds newer bytes inside the recorded
//    write ranges only: the rest of the page is copied around them (copy_around, #531).
//  * record (note_gpu_write, render thread): every range a command may write in pool memory is recorded with the submission's serial
//    (LandRec), and its pages' page_gpu set to that serial.
//  * land (nvn_pool_land.cpp; read_completed, any thread; and the nvn-pool-land thread): once the timeline reaches a record's serial, the
//    recorded range is copied from the shadow back into guest memory, before that serial is published as complete (complete_serial). So a
//    game thread that waits for a sync, polls one, or reads an event word the GPU wrote, finds the GPU's bytes in its memory, as with an import.
// Known limits: a CPU write to bytes of a range the GPU may write, made while that submission is in flight, is overwritten at
// landing (the whole recorded range lands, not only what a shader stored: landed_kb); a GPU write through a span and a read of the same bytes
// through another chunk (or the reverse) while that write is in flight sees the other buffer's bytes (span_writes counts such writes).
// Design after totk-pc-recomp runtime/vk_memory.c (shadow storage) and ultratears src/gfx/backend_vk.c (pool copies); the code is new.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/host_device.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"
#include "nvn_gpu_internal.h"
#include "nvn_pool_copy_impl.h"
#include "nvn_pool_track.h"

namespace nxrt::nvn::be::gpu {
namespace {

using pcopy::kPage;
using pcopy::kTag;
constexpr uint64_t kSpanGrain = 64 * 1024;  // a span's start and end are multiples of it

// Native Windows keeps ordinary pool imports forever. On an integrated GPU with a small device-local heap those imports share RAM with
// everything else and eventually make queue submissions fail with VK_ERROR_OUT_OF_DEVICE_MEMORY. Use the bounded shadow cache there too.
struct AutoDecision {
    gfx::Device* device = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    bool on = false;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint64_t local = 0;          // largest DEVICE_LOCAL heap in bytes; read only for an integrated GPU, else 0
};
constexpr uint64_t kAutoMaxLocal = 8ull << 30;
std::mutex g_auto_mu;
AutoDecision g_auto;

bool auto_copy() {
    if (host_is_wine()) return true;
    if (!active()) return false;     // an early stats call must not latch auto off
    gfx::Device& d = device();
    std::lock_guard<std::mutex> lk(g_auto_mu);
    if (g_auto.device == &d && g_auto.physical == d.physical()) return g_auto.on;
    bool on = false;
    uint64_t local = 0;
    const VkPhysicalDeviceType type = d.properties().deviceType;
    if (type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        VkPhysicalDeviceMemoryProperties mp = {};
        vkGetPhysicalDeviceMemoryProperties(d.physical(), &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local = std::max<uint64_t>(local, mp.memoryHeaps[i].size);
        on = local && local <= kAutoMaxLocal;
    }
    g_auto = AutoDecision{&d, d.physical(), on, type, local};
    return on;
}

struct Cfg {
    bool automatic = true;
    bool on = false;
    std::string mode;
    uint64_t cell = 16ull << 20;
    uint32_t idle = 600;
    uint64_t cap = 1536ull << 20;
    bool once_queued = true;
};

const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg r;
        r.mode = config_get_str("nvn", "pool_copy", "auto");
        r.automatic = r.mode == "auto";
        r.on = !r.automatic && (r.mode == "on" || r.mode == "1" || r.mode == "true");
        r.cell = static_cast<uint64_t>(std::clamp<int64_t>(config_get_int("nvn", "pool_copy_cell_mb", 16), 1, 1024)) << 20;
        r.idle = static_cast<uint32_t>(std::clamp<int64_t>(config_get_int("nvn", "pool_copy_idle", 600), 0, 1 << 30));
        r.cap = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "pool_copy_mb", 1536))) << 20;
        r.once_queued = config_get_bool("nvn", "pool_copy_once_queued", true);
        return r;
    }();
    return c;
}

// What auto saw, for the decision line of a bug report: host, Vulkan device type, largest device-local heap (integrated GPUs only), threshold.
std::string auto_facts() {
    if (host_is_wine()) return ", Wine: yes";
    char buf[256];
    AutoDecision a;
    {
        std::lock_guard<std::mutex> lk(g_auto_mu);
        a = g_auto;
    }
    const char* type = a.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : a.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "other";
    if (a.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        std::snprintf(buf, sizeof buf, ", Wine: no, GPU type: %s, largest device-local heap: %llu MiB, on up to %llu MiB", type,
                 static_cast<unsigned long long>(a.local >> 20), static_cast<unsigned long long>(kAutoMaxLocal >> 20));
    else
        std::snprintf(buf, sizeof buf, ", Wine: no, GPU type: %s (heap not read), on only for integrated GPUs with up to %llu MiB", type,
                 static_cast<unsigned long long>(kAutoMaxLocal >> 20));
    return buf;
}

void log_cfg(const Cfg& c, bool on) {
    static std::once_flag once;
    std::call_once(once, [&] {
        const std::string why = c.automatic ? auto_facts() : std::string();
        NXLOG_I(kTag, "pool_copy %s (%s%s)", on ? "on" : "off", c.mode.c_str(), why.c_str());
        if (on)
            NXLOG_I(kTag, "pool_copy: shadows in cells of %llu MiB, dropped after %u idle presents, cap %llu MiB; pool_lazy, pool_lazy_chunk_mb, "
                          "pool_lazy_idle, pool_lazy_cpu_untile and compute_out_lazy are ignored",
                    static_cast<unsigned long long>(c.cell >> 20), c.idle, static_cast<unsigned long long>(c.cap >> 20));
        if (on)
            NXLOG_I(kTag, "pool_copy: a copy of a page serves %s", c.once_queued ? "every submission the game had queued when it was made (pool_copy_once_queued)"
                                                                               : "the submission being recorded only (pool_copy_once_queued off)");
    });
}

// statistics
std::atomic<bool> g_any{false};
std::atomic<int64_t> g_live{0};
std::atomic<uint64_t> g_chunks{0}, g_allocs{0}, g_copied{0}, g_unmatched{0}, g_fallback{0}, g_span_writes{0}, g_evicted{0}, g_spans{0};
uint64_t g_sub_serial = 0, g_sub_bytes = 0;     // render thread: bytes copied in by the submission being recorded
// Why a page was copied into a shadow (the summary's split of "copied in"), in the order the touch tests them:
enum Why : int {
    kWhyFirst,       // the shadow had no copy of it yet
    kWhyAgain,       // the same, in a shadow made again after a drop (idle, or the cap)
    kWhyLanded,      // a GPU write landed in it since the last copy (the bytes came from this or another shadow)
    kWhyUntracked,   // its pool has no write tracker, or the copy is not the render thread's: every use copies
    kWhyHot,         // the tracker left it writable (hot, or it cannot be armed): every use copies
    kWhyWritten,     // the tracker saw a CPU write since the last copy
    kWhyAround,      // a GPU write into the page is in flight: the bytes around it (copy_around)
    kWhyCount
};
std::atomic<uint64_t> g_why[kWhyCount];
std::atomic<uint64_t> g_unsized{0};             // bytes copied for binds of unknown size (they copy to the end of their chunk)
std::atomic<int64_t> g_last_sub_kb{0};          // ... and by the last one (gauge)

// The last resolve of the render thread: the chunk a following gpu_wrote of the same range writes, and pool_copy_touch's chunk.
struct LastResolve {
    PoolGpu* pool = nullptr;
    PoolChunk* chunk = nullptr;
    uint64_t gen = 0;      // g_free_gen when set
};
thread_local LastResolve t_last;
// bumped by pcopy_free: a t_last set before it may point into a chunk another thread destroyed, and is ignored
std::atomic<uint64_t> g_free_gen{0};
inline bool last_valid() { return t_last.chunk && t_last.gen == g_free_gen.load(std::memory_order_acquire); }

// live shadows, for the cap's least-recently-used eviction (lock order: g_live_mu of nvn_gpu_pool.cpp -> g_reg_mu -> the landing's mutexes)
std::mutex g_reg_mu;
std::vector<PoolChunk*> g_reg;

struct CopyGauges {
    CopyGauges() {
        nxrt::gauge_register("nvn.pool.copy.live_mb", [] { return g_live.load(std::memory_order_relaxed) >> 20; });
        nxrt::gauge_register("nvn.pool.copy.copied_kb", [] { return g_last_sub_kb.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.pool.copy.landed_kb", [] { return static_cast<int64_t>(pcopy::shared().landed.load(std::memory_order_relaxed) >> 10); });
        nxrt::gauge_register("nvn.pool.copy.write_unmatched", [] { return static_cast<int64_t>(g_unmatched.load(std::memory_order_relaxed)); });
        nxrt::gauge_register("nvn.pool.copy.import_fallback", [] { return static_cast<int64_t>(g_fallback.load(std::memory_order_relaxed)); });
        nxrt::gauge_register("nvn.pool.copy.unreadable", [] { return static_cast<int64_t>(pcopy::shared().unreadable.load(std::memory_order_relaxed)); });
        nxrt::gauge_register("nvn.pool.copy.span_writes", [] { return static_cast<int64_t>(g_span_writes.load(std::memory_order_relaxed)); });
        nxrt::hitch_add_counter("pool_copy_copied_kb", []() -> uint64_t { return g_copied.load(std::memory_order_relaxed) >> 10; });
        nxrt::hitch_add_counter("pool_copy_landed_kb", []() -> uint64_t { return pcopy::shared().landed.load(std::memory_order_relaxed) >> 10; });
    }
    ~CopyGauges() {
        for (const char* n : {"nvn.pool.copy.live_mb", "nvn.pool.copy.copied_kb", "nvn.pool.copy.landed_kb", "nvn.pool.copy.write_unmatched",
                              "nvn.pool.copy.import_fallback", "nvn.pool.copy.unreadable", "nvn.pool.copy.span_writes"})
            nxrt::gauge_unregister(n);
    }
};
CopyGauges g_copy_gauges;

// memcpy that survives an access violation (a guest page decommitted under it). No C++ objects here (SEH).
bool seh_copy(void* d, const void* s, size_t n) {
    __try {
        std::memcpy(d, s, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

bool seh_same(const void* a, const void* b, size_t n) {      // as seh_copy; false on a fault
    __try {
        return std::memcmp(a, b, n) == 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void reg_remove(PoolChunk* c) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    g_reg.erase(std::remove(g_reg.begin(), g_reg.end(), c), g_reg.end());
}

// pcopy_evict_for is making room for a shadow of this size (0: not running): pcopy_free tells the stock, which keeps only a buffer of
// that size then. Render thread.
uint64_t g_evict_want = 0;

bool covers(const PoolChunk* c, uint64_t off) { return c && c->is_shadow && c->buffer && off >= c->off && off < c->off + c->size; }

// Submission `serial` writes pool bytes [off, end) through `writer`. Another shadow over those pages (a span beside the cell's chunk) does
// not get that write, so its copies stop serving at that submission: a later one copies the landed bytes from guest memory, exactly as
// when a copy served one submission only ([nvn] pool_copy_once_queued). Render thread.
void stop_serving(PoolGpu* pg, const PoolChunk* writer, uint64_t off, uint64_t end, uint64_t serial) {
    const auto clamp = [&](PoolChunk& x) {
        if (&x == writer || !x.is_shadow || !x.buffer) return;
        const uint64_t lo = std::max<uint64_t>(off, x.off), hi = std::min<uint64_t>(end, x.off + x.size);
        if (lo >= hi) return;
        for (uint64_t p = (lo - x.off) / kPage; p <= (hi - 1 - x.off) / kPage; ++p) {
            uint64_t& pc = x.page_copied[static_cast<size_t>(p)];
            if (pc > serial + 1) pc = serial + 1;
        }
    };
    for (size_t k = static_cast<size_t>(off / pg->stride); k < pg->cell_ext.size() && k * pg->stride < end; ++k)
        if (const int32_t e = pg->cell_ext[k]; e >= 0) clamp(pg->ext[static_cast<size_t>(e)]);
    for (PoolChunk& x : pg->spans) clamp(x);
}

}  // namespace

namespace pcopy {

Shared& shared() {
    static Shared s;
    return s;
}

bool guarded_same(const uint8_t* a, const uint8_t* b, uint64_t n) { return seh_same(a, b, static_cast<size_t>(n)); }

uint64_t guarded_copy(uint8_t* d, const uint8_t* s, uint64_t n, bool in) {
    if (seh_copy(d, s, static_cast<size_t>(n))) return 0;
    uint64_t bad = 0;
    const uint64_t a = reinterpret_cast<uint64_t>(in ? s : d);
    for (uint64_t o = 0; o < n;) {
        const uint64_t k = std::min<uint64_t>(n - o, kPage - ((a + o) & (kPage - 1)));
        if (!seh_copy(d + o, s + o, static_cast<size_t>(k))) {
            if (in) std::memset(d + o, 0, static_cast<size_t>(k));
            ++bad;
        }
        o += k;
    }
    return bad;
}

}  // namespace pcopy

bool pool_copy_on() {
    const Cfg& c = cfg();
    const bool on = c.automatic ? auto_copy() : c.on;
    if (!c.automatic || host_is_wine() || active()) log_cfg(c, on);   // native Windows: wait for a device before deciding or logging auto
    return on;
}
uint64_t pool_copy_cell() { return cfg().cell; }
uint32_t pool_copy_idle() { return cfg().idle; }
bool pool_copy_any() { return g_any.load(std::memory_order_relaxed); }

void pcopy_pool_init(PoolGpu* g) {
    const size_t pages = static_cast<size_t>((g->size + kPage - 1) / kPage);
    g->page_gpu.reset(new std::atomic<uint64_t>[pages]);
    for (size_t i = 0; i < pages; ++i) g->page_gpu[i].store(0, std::memory_order_relaxed);
    g->page_landed.reset(new std::atomic<uint8_t>[pages]);
    for (size_t i = 0; i < pages; ++i) g->page_landed[i].store(0, std::memory_order_relaxed);
    g->cell_had.assign(g->cell_ext.size(), 0);
    g->track = ptrack_create(g->base, g->size);
    static std::once_flag once;
    std::call_once(once, [&] {
        NXLOG_I(kTag, "pool_copy: %s", g->track ? "a page is copied again only after the CPU wrote it (write tracking)"
                                                : pool_track_on() ? "this pool cannot be write-tracked: every bind copies its pages"
                                                                  : "pool_copy_track off: every bind copies its pages");
        if (g->track)
            NXLOG_I(kTag, "pool_copy: a GPU write that lands in a page %s", pcopy::land_keep() ? "leaves it current: no bind copies it back (pool_copy_land_keep)"
                                                                                               : "counts as a CPU write (pool_copy_land_keep off)");
    });
    g_any.store(true, std::memory_order_relaxed);
    pcopy::land_start();
    pcopy::stock_start(cfg().cell, cfg().cap);
}

void pcopy_pool_destroy(PoolGpu* g) {
    ptrack_destroy(g->track);
    g->track = nullptr;
}

void pcopy_track_stop(PoolGpu* g) {
    pcopy::LandingPause pause;     // a landing asks the tracker about pages of this pool ([nvn] pool_copy_land_keep)
    ptrack_stop(g->track);
}

bool pcopy_alloc(PoolGpu* g, PoolChunk& c, uint64_t off, uint64_t size, const char* what, bool cells) {
    NXFT("pool.copy.alloc");
    const uint64_t cap = cfg().cap;
    if (cap && pcopy::stock_over(size, cap)) {
        pcopy_evict_for(size);
        static bool logged = false;
        if (pcopy::stock_over(size, cap) && !logged) {
            logged = true;
            NXLOG_I(kTag, "pool_copy: %llu MiB of shadows live, over the cap of %llu MiB, and none idle enough to drop: allocated anyway (logged once)",
                    static_cast<unsigned long long>(g_live.load() >> 20), static_cast<unsigned long long>(cap >> 20));
        }
    }
    gfx::Buffer b;
    if (!pcopy::stock_get(size, &b)) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        static bool warned = false;
        if (!warned) {
            warned = true;
            NXLOG_W(kTag, "pool_copy: a shadow of %llu KiB (pool %p %s at offset 0x%llx) could not be made: imported instead (logged once; counter "
                          "nvn.pool.copy.import_fallback)", static_cast<unsigned long long>(size >> 10), static_cast<void*>(g->base), what,
                    static_cast<unsigned long long>(off));
        }
        return false;
    }
    c.shadow = b;
    c.buffer = b.buffer;
    c.mem = VK_NULL_HANDLE;
    c.view = nullptr;
    c.is_shadow = true;
    c.tried = true;
    c.failed = false;
    c.off = off;
    c.size = size;
    c.owner = g;
    c.page_copied.assign(static_cast<size_t>((size + kPage - 1) / kPage), 0);   // nothing is copied until a bind touches it
    c.page_gen.assign(c.page_copied.size(), 0);
    c.land_refs = 0;
    c.cov_serial = c.cov_lo = c.cov_hi = 0;
    c.again = false;
    c.filled = 0;
    if (cells && g->stride)
        for (size_t k = static_cast<size_t>(off / g->stride); k < g->cell_had.size() && k * g->stride < off + size; ++k) {
            c.again = c.again || g->cell_had[k];
            g->cell_had[k] = 1;
        }
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        g_reg.push_back(&c);
    }
    g_live.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
    g_chunks.fetch_add(1, std::memory_order_relaxed);
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    NXLOG_D(kTag, "pool %p: shadow %s at offset 0x%llx, %llu KiB", static_cast<void*>(g->base), what, static_cast<unsigned long long>(off),
            static_cast<unsigned long long>(size >> 10));
    return true;
}

PoolChunk* pcopy_span(PoolGpu* g, uint64_t off, uint64_t size, bool* made) {
    for (PoolChunk& s : g->spans)
        if (s.is_shadow && s.buffer && off >= s.off && off + size <= s.off + s.size) return &s;
    // The span holds the range itself, rounded out to kSpanGrain (binds at nearby offsets of one object share it), not the whole cells it
    // touches: those cells have shadows of their own, and a span the size of two cells for every object on a cell boundary held more
    // memory than [nvn] pool_copy_mb allows (84 spans of 32 MiB in a 200 s Steam Deck run, mostly for ranges of a few KiB).
    const uint64_t s0 = off / kSpanGrain * kSpanGrain;
    const uint64_t s1 = std::min<uint64_t>((off + size + kSpanGrain - 1) / kSpanGrain * kSpanGrain, g->size);
    const uint64_t maxb = device().limits_ext().max_buffer_size;
    if (s1 <= s0 || (maxb && s1 - s0 > maxb)) return nullptr;
    PoolChunk* slot = nullptr;
    for (PoolChunk& s : g->spans)                 // a dropped span's empty slot (copy pools never erase spans)
        if (!s.buffer && !s.failed && !s.size) { slot = &s; break; }
    const bool appended = !slot;
    if (appended) { g->spans.emplace_back(); slot = &g->spans.back(); }
    if (!pcopy_alloc(g, *slot, s0, s1 - s0, "span", false)) {
        if (appended) g->spans.pop_back();
        return nullptr;
    }
    NXLOG_D(kTag, "pool %p: a range of %llu KiB at offset 0x%llx crosses a cell boundary: shadow span of %llu KiB at offset 0x%llx",
            static_cast<void*>(g->base), static_cast<unsigned long long>(size >> 10), static_cast<unsigned long long>(off),
            static_cast<unsigned long long>((s1 - s0) >> 10), static_cast<unsigned long long>(s0));
    g_spans.fetch_add(1, std::memory_order_relaxed);
    if (made) *made = true;
    return slot;
}

void pcopy_free(PoolChunk& c) {
    reg_remove(&c);
    g_free_gen.fetch_add(1, std::memory_order_acq_rel);
    pcopy::land_forget(c);     // no landing reads this chunk's shadow from here on
    if (t_last.chunk == &c) t_last = LastResolve{};
    g_live.fetch_sub(static_cast<int64_t>(c.size), std::memory_order_relaxed);
    g_chunks.fetch_sub(1, std::memory_order_relaxed);
    pcopy::stock_give(c.shadow, g_evict_want);
}

// Pages [p0, p1) of chunk c hold a GPU write in flight (touch's state 2). The shadow is newer than guest memory only inside the recorded
// write ranges; every other byte of those pages is the CPU's, and copying none of the page left the GPU reading stale bytes there. A
// constant buffer the CPU writes can share a 4 KiB page with an UpdateUniformBuffer target or a storage buffer, so a loop count read there
// could be an older word and run a compute loop for seconds (device lost on AMD, a frozen GPU on NVIDIA: #531, #533). Copies guest ->
// shadow around every unlanded record of the pool (pcopy::land_busy). A record added after that is a later command's (its GPU write
// replaces these bytes).
static uint64_t copy_around(PoolGpu* g, PoolChunk* c, uint64_t p0, uint64_t p1) {
    const uint64_t lo = c->off + p0 * kPage, hi = c->off + p1 * kPage;   // pool offsets
    std::vector<std::pair<uint64_t, uint64_t>> busy;
    pcopy::land_busy(g, lo, hi, busy);
    std::sort(busy.begin(), busy.end());
    uint8_t* dst = static_cast<uint8_t*>(c->shadow.mapped) - c->off;
    const uint8_t* src = g->base;
    uint64_t at = lo, bytes = 0, bad = 0;
    const auto copy_to = [&](uint64_t end) {
        if (end > at) {
            bad += pcopy::guarded_copy(dst + at, src + at, end - at, true);
            bytes += end - at;
        }
    };
    for (const auto& [a, b] : busy) {
        copy_to(a);
        at = std::max(at, b);
    }
    copy_to(hi);
    if (bad) pcopy::shared().unreadable.fetch_add(bad, std::memory_order_relaxed);
    return bytes;
}

void pcopy_touch(PoolGpu* g, PoolChunk* c, uint64_t off, uint64_t size) {
    const uint64_t cend = c->off + c->size;
    if (off < c->off || off >= cend) return;
    const uint64_t end = size ? std::min<uint64_t>(off + size, cend) : cend;   // unknown size: to the end of the chunk
    const uint64_t p0 = (off - c->off) / kPage, p1 = (end - c->off + kPage - 1) / kPage;   // chunk pages [p0, p1)
    const bool rt = on_render_thread();
    if (rt) t_last = LastResolve{g, c, g_free_gen.load(std::memory_order_acquire)};
    const uint64_t cur = rt ? recording_serial() : 0;     // 0 outside a recording: always copied
    if (cur && c->cov_serial == cur && p0 >= c->cov_lo && p1 <= c->cov_hi) return;   // all copied in this submission already
    // The last submission a copy made from here on serves (page_copied holds it + 1). The game's data for a submission is in its memory
    // when the game queues it, and it leaves those bytes alone until the GPU is done with them, so a copy made now holds what every
    // submission queued so far reads ([nvn] pool_copy_once_queued; read before the copy reads a byte). The render thread is usually
    // several submissions behind the game: a page that is copied at every use (hot, untracked) is then copied once per group of queued
    // submissions instead of once per submission. Off: the submission being recorded only.
    const uint64_t through = rt ? std::max(cur, cfg().once_queued ? queued_serial() : uint64_t{0}) : 0;
    const uint64_t done = completed_serial();
    const uint64_t pbase = c->off / kPage;
    // 0: a copy that serves this submission exists; 1: copy; 2: a GPU write is in flight there (the shadow is newer inside its range: copy
    // around it), unless nothing was ever copied into the page of this chunk (a new span over a page another chunk writes): then guest
    // memory is the better source.
    // ... or (0 too) the CPU has not written the page since this chunk copied it (the write tracker; render thread only).
    PoolTrack* const trk = rt ? g->track : nullptr;
    const auto need = [&](uint64_t p) -> int {
        const uint64_t pc = c->page_copied[static_cast<size_t>(p)];
        if (cur && pc > cur) return 0;
        if (g->page_gpu[static_cast<size_t>(pbase + p)].load(std::memory_order_acquire) > done && (!rt || pc != 0)) return 2;
        if (trk && ptrack_current(trk, pbase + p, c->page_gen[static_cast<size_t>(p)])) return 0;
        return 1;
    };
    const uint32_t frame = static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed));
    uint8_t* dst = static_cast<uint8_t*>(c->shadow.mapped);
    const uint8_t* src = g->base + c->off;
    const uint64_t t0 = ft::on() ? ft::now_ns() : 0;
    uint64_t bytes = 0, why[kWhyCount] = {};
    for (uint64_t p = p0; p < p1;) {
        const int n = need(p);
        if (n == 0) {
            ++p;
            continue;
        }
        uint64_t q = p + 1;
        while (q < p1 && need(q) == n) ++q;
        if (n == 2) {
            // serves the submission being recorded only, and the tracker does not count it as current: once the write has landed, the
            // first bind copies the whole page again
            const uint64_t got = copy_around(g, c, p, q);
            bytes += got;
            why[kWhyAround] += (got + kPage - 1) / kPage;
            if (trk) std::fill(c->page_gen.begin() + static_cast<std::ptrdiff_t>(p), c->page_gen.begin() + static_cast<std::ptrdiff_t>(q), 0u);
            if (rt)
                for (uint64_t i = p; i < q; ++i) c->page_copied[static_cast<size_t>(i)] = cur + 1;
            p = q;
            continue;
        }
        for (uint64_t i = p; i < q; ++i) {      // why, before the copy changes what that is read from
            std::atomic<uint8_t>& ld = g->page_landed[static_cast<size_t>(pbase + i)];
            const bool landed = ld.load(std::memory_order_relaxed) != 0;
            if (landed) ld.store(0, std::memory_order_relaxed);
            const bool first = c->page_copied[static_cast<size_t>(i)] == 0;
            if (first) ++c->filled;
            ++why[first ? (c->again ? kWhyAgain : kWhyFirst) : landed ? kWhyLanded : !trk ? kWhyUntracked
                        : !c->page_gen[static_cast<size_t>(i)] ? kWhyHot : kWhyWritten];
        }
        // A copy made while a GPU write into one of the pages is in flight (through another shadow: this one had no copy yet) misses
        // that write. It serves the submission being recorded only, and the tracker does not count it as current (the landing may
        // leave the page current, [nvn] pool_copy_land_keep): the first bind after the landing copies the pages again. A page_gpu of 0
        // read here means the landing is over, so the copy below reads the landed bytes.
        bool flight = false;
        for (uint64_t i = p; i < q && !flight; ++i) flight = g->page_gpu[static_cast<size_t>(pbase + i)].load(std::memory_order_acquire) != 0;
        // armed before the copy reads them: a write that lands during the copy faults, and the page is copied again at its next use
        if (trk) {
            ptrack_before_copy(trk, pbase + p, q - p, frame, &c->page_gen[static_cast<size_t>(p)]);
            if (flight) std::fill(c->page_gen.begin() + static_cast<std::ptrdiff_t>(p), c->page_gen.begin() + static_cast<std::ptrdiff_t>(q), 0u);
        }
        const uint64_t bad = pcopy::guarded_copy(dst + p * kPage, src + p * kPage, (q - p) * kPage, true);
        if (bad) pcopy::shared().unreadable.fetch_add(bad, std::memory_order_relaxed);
        if (rt)
            for (uint64_t i = p; i < q; ++i) c->page_copied[static_cast<size_t>(i)] = (flight ? cur : through) + 1;   // never 0: a copy exists
        bytes += (q - p) * kPage;
        p = q;
    }
    if (cur) {      // [p0, p1) is current for this submission: widen the fast path's interval
        if (c->cov_serial == cur && p0 <= c->cov_hi && p1 >= c->cov_lo) {
            c->cov_lo = std::min(c->cov_lo, p0);
            c->cov_hi = std::max(c->cov_hi, p1);
        } else {
            c->cov_serial = cur;
            c->cov_lo = p0;
            c->cov_hi = p1;
        }
    }
    if (!bytes) return;
    g_copied.fetch_add(bytes, std::memory_order_relaxed);
    for (int i = 0; i < kWhyCount; ++i)
        if (why[i]) g_why[i].fetch_add(why[i] * kPage, std::memory_order_relaxed);
    if (!size) g_unsized.fetch_add(bytes, std::memory_order_relaxed);
    if (rt) {
        if (cur != g_sub_serial) {
            g_last_sub_kb.store(static_cast<int64_t>(g_sub_bytes >> 10), std::memory_order_relaxed);
            g_sub_serial = cur;
            g_sub_bytes = 0;
        }
        g_sub_bytes += bytes;
    }
    if (t0) {
        static const uint16_t z = ft::zone_id("pool.copy.touch");
        ft::event(z, t0, ft::now_ns(), static_cast<uint32_t>(std::min<uint64_t>(bytes >> 10, 0xffffffffu)));
    }
}

void pool_copy_touch(uint64_t gpu_address, uint64_t size) {
    if (!pool_copy_on() || !size || !last_valid()) return;
    const uint64_t off = gpu_address - reinterpret_cast<uint64_t>(t_last.pool->base);
    if (!covers(t_last.chunk, off)) return;
    pcopy_touch(t_last.pool, t_last.chunk, off, size);
}

void pcopy_note_write(uint64_t gpu_address, uint64_t size, VkBuffer hint) {
    if (!size) return;
    const PoolHit hit = pool_lookup(gpu_address);
    if (!hit.pool || !hit.pool->gpu) return;
    PoolGpu* pg = hit.pool->gpu;
    if (!pg->copy || pg->failed || !pg->page_gpu) return;
    const uint64_t off = gpu_address - reinterpret_cast<uint64_t>(pg->base);
    if (off >= pg->size) return;
    PoolChunk* c = nullptr;
    bool span = false;
    if (hint) {
        for (PoolChunk& x : pg->ext) if (x.buffer == hint && covers(&x, off)) { c = &x; break; }
        if (!c) for (PoolChunk& x : pg->spans) if (x.buffer == hint && covers(&x, off)) { c = &x; span = true; break; }
    }
    if (!c && last_valid() && t_last.pool == pg && covers(t_last.chunk, off) && (!hint || t_last.chunk->buffer == hint)) {
        c = t_last.chunk;
        for (const PoolChunk& x : pg->spans) if (&x == c) { span = true; break; }
    }
    if (!c) {     // not the buffer the last resolve gave: the cell's chunk, else a span holding the address
        const int32_t e = pg->cell_ext[static_cast<size_t>(off / pg->stride)];
        if (e >= 0 && covers(&pg->ext[static_cast<size_t>(e)], off)) c = &pg->ext[static_cast<size_t>(e)];
        if (!c) for (PoolChunk& x : pg->spans) if (covers(&x, off)) { c = &x; span = true; break; }
        if (c) g_unmatched.fetch_add(1, std::memory_order_relaxed);
    }
    if (!c) return;     // an imported fallback chunk (the GPU writes guest memory itself) or nothing resolved there
    const uint64_t end = std::min<uint64_t>({off + size, pg->size, c->off + c->size});
    const uint64_t serial = recording_serial() ? recording_serial() : current_serial();
    if (span) g_span_writes.fetch_add(1, std::memory_order_relaxed);
    for (uint64_t p = off / kPage; p <= (end - 1) / kPage; ++p) {
        std::atomic<uint64_t>& w = pg->page_gpu[static_cast<size_t>(p)];
        if (w.load(std::memory_order_relaxed) < serial) w.store(serial, std::memory_order_release);
    }
    if (!pcopy::land_record(pg, c, off, end - off, serial)) return;
    if (!on_render_thread()) return;
    if (cfg().once_queued) stop_serving(pg, c, off, end, serial);
    if (pg->track && pcopy::land_keep()) {     // only this shadow stays current through the landing (nvn_pool_track.h)
        const uint64_t cbase = c->off / kPage;
        for (uint64_t p = off / kPage; p <= (end - 1) / kPage; ++p) ptrack_gpu_write(pg->track, p, &c->page_gen[static_cast<size_t>(p - cbase)]);
    }
}

void pcopy_shutdown() {
    pcopy::land_stop();               // the device is idle and the pools go next
    pcopy::stock_stop();              // ... and their shadows' buffers are destroyed, not kept, from here on
    t_last = LastResolve{};
}

void pcopy_evict_for(uint64_t bytes) {
    NXFT("pool.copy.evict");
    const uint64_t cap = cfg().cap;
    const uint64_t done = poll_completed(), cur = current_serial();
    const uint32_t now = static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed));
    std::vector<PoolChunk*> cand;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        for (PoolChunk* c : g_reg)
            if (c->buffer && c->used_serial <= done && c->used_serial < cur) cand.push_back(c);
    }
    std::sort(cand.begin(), cand.end(), [now](const PoolChunk* a, const PoolChunk* b) { return now - a->used_at > now - b->used_at; });
    uint64_t freed = 0;
    g_evict_want = bytes;      // a dropped shadow of that size hands its buffer on through the stock; the others are destroyed
    for (PoolChunk* c : cand) {
        if (!pcopy::stock_over(bytes, cap)) break;
        if (pcopy_busy(*c)) continue;
        const uint64_t sz = c->size;
        if (!pool_drop_chunk_if_live(c->owner, c)) continue;     // under g_live_mu, like lazy_drop_idle
        freed += sz;
        g_evicted.fetch_add(1, std::memory_order_relaxed);
    }
    g_evict_want = 0;
    if (freed)
        NXLOG_I(kTag, "pool_copy: over the cap of %llu MiB: dropped %llu MiB of the least recently used shadows", static_cast<unsigned long long>(cap >> 20),
                static_cast<unsigned long long>(freed >> 20));
}

void pcopy_log_summary(uint32_t presents) {
    (void)presents;
    static auto last = std::chrono::steady_clock::now();
    static uint64_t p_copied = 0, p_landed = 0, p_why[kWhyCount] = {}, p_unsized = 0;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::max(1e-3, std::chrono::duration<double>(now - last).count());
    last = now;
    const uint64_t copied = g_copied.load(), landed = pcopy::shared().landed.load();
    const size_t pending = pcopy::land_pending();
    NXLOG_I(kTag, "pool_copy: %lld MiB of shadows live in %llu chunks (%llu spans made, %llu dropped by the cap); copied in %.1f MiB/s, landed %.1f MiB/s; "
                  "%llu writes recorded, %zu waiting; %llu import fallbacks, %llu unreadable pages, %llu writes matched by cell, %llu span writes",
            static_cast<long long>(g_live.load() >> 20), static_cast<unsigned long long>(g_chunks.load()), static_cast<unsigned long long>(g_spans.load()),
            static_cast<unsigned long long>(g_evicted.load()), (copied - p_copied) / 1048576.0 / secs, (landed - p_landed) / 1048576.0 / secs,
            static_cast<unsigned long long>(pcopy::shared().records.load()), pending, static_cast<unsigned long long>(g_fallback.load()),
            static_cast<unsigned long long>(pcopy::shared().unreadable.load()), static_cast<unsigned long long>(g_unmatched.load()),
            static_cast<unsigned long long>(g_span_writes.load()));
    {   // the split of "copied in", and how much of the live shadows holds a copy at all
        double rate[kWhyCount];
        for (int i = 0; i < kWhyCount; ++i) {
            const uint64_t v = g_why[i].load(std::memory_order_relaxed);
            rate[i] = (v - p_why[i]) / 1048576.0 / secs;
            p_why[i] = v;
        }
        const uint64_t unsized = g_unsized.load(std::memory_order_relaxed);
        uint64_t filled = 0;
        {
            std::lock_guard<std::mutex> lk(g_reg_mu);
            for (const PoolChunk* c : g_reg) filled += c->filled;
        }
        NXLOG_I(kTag, "pool_copy: copied in, by reason, MiB/s: first copy %.1f, first copy after a drop %.1f, after a landing %.1f, written %.1f, hot %.1f, "
                      "untracked %.1f, around a GPU write %.1f; %.1f of it for binds of unknown size; %llu MiB of the live shadows hold a copy",
                rate[kWhyFirst], rate[kWhyAgain], rate[kWhyLanded], rate[kWhyWritten], rate[kWhyHot], rate[kWhyUntracked], rate[kWhyAround],
                (unsized - p_unsized) / 1048576.0 / secs, static_cast<unsigned long long>(filled * kPage >> 20));
        p_unsized = unsized;
    }
    {
        const pcopy::StockStats st = pcopy::stock_stats();
        NXLOG_I(kTag, "pool_copy: shadow buffers so far: %llu made at a bind (%.1f ms in all), %llu made ahead by the stock thread, %llu shadows got a "
                      "spare; %llu MiB spare now",
                static_cast<unsigned long long>(st.made_here), st.made_here_ms, static_cast<unsigned long long>(st.made_ahead),
                static_cast<unsigned long long>(st.reused), static_cast<unsigned long long>(st.spare_bytes >> 20));
    }
    if (pool_track_on()) {
        const PoolTrackStats t = ptrack_stats();
        NXLOG_I(kTag, "pool_copy: write tracking: %llu pages armed in %llu runs so far, %llu copied pages found written again, %llu hot now, %llu not armable; "
                      "%llu write faults; landings left %llu pages current and found %llu written meanwhile",
                static_cast<unsigned long long>(t.armed_pages), static_cast<unsigned long long>(t.arms), static_cast<unsigned long long>(t.rewrites),
                static_cast<unsigned long long>(t.hot_now), static_cast<unsigned long long>(t.unarmable), static_cast<unsigned long long>(mem::ww::faults()),
                static_cast<unsigned long long>(t.land_kept), static_cast<unsigned long long>(t.land_differ));
    }
    p_copied = copied;
    p_landed = landed;
}

PoolCopyStats pool_copy_stats() {
    PoolCopyStats s;
    s.on = pool_copy_on();
    s.live_bytes = g_live.load();
    s.chunks = g_chunks.load();
    s.allocs = g_allocs.load();
    s.copied_bytes = g_copied.load();
    s.landed_bytes = pcopy::shared().landed.load();
    s.records = pcopy::shared().records.load();
    s.pending = pcopy::land_pending();
    s.write_unmatched = g_unmatched.load();
    s.import_fallback = g_fallback.load();
    s.unreadable = pcopy::shared().unreadable.load();
    s.span_writes = g_span_writes.load();
    s.evicted = g_evicted.load();
    return s;
}

}  // namespace nxrt::nvn::be::gpu
