// SPDX-License-Identifier: GPL-3.0-or-later
// Page write tracking by page protection (include/nxrt/write_watch.h). Four bitmaps per region, one bit per 4 KiB page:
//   prot    the page is read-only because of us (set by arm, cleared by whoever makes it writable again)
//   valid   armed and not written since (the owner's copy of the page is current)
//   written the CPU wrote it since the owner last asked (take_written)
//   ban     never arm (another view of the page exists)
// plus a summary of `written`: one bit per word of it (wsum), set after the word's bit, so take_written reads only the words that may hold
// a written page instead of the whole bitmap (a region over the 3.5+ GiB heap is ~14k words: ~0.1 ms a call, ~11 calls a frame).
// The fault handler and before_os_write may run on any thread at any time; they only use atomics, VirtualProtect and the region's
// slim reader/writer lock: whoever makes pages writable holds it shared, arm() and region_destroy() hold it exclusive, so a page's bit
// and its protection change together as seen by everybody else (no page ends up read-only without its bit, or armed and writable). Regions are
// published in a fixed table; a reader counts itself in g_busy while it looks at a region, and region_destroy waits for that count to
// drop after taking the region out of the table.
#include "nxrt/write_watch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::mem::ww {

struct Region {
    uint64_t base = 0;          // address of page 0
    uint64_t npages = 0;
    uint64_t nwords = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> prot, valid, written, ban;
    std::unique_ptr<std::atomic<uint64_t>[]> guard;     // PAGE_NOACCESS because of guard() (a subset of prot)
    std::unique_ptr<std::atomic<uint64_t>[]> wsum;      // bit w: written[w] may be non-zero (set after written[w]; never cleared before it is read)
    uint64_t nsum = 0;
    int slot = -1;
    SRWLOCK lock = SRWLOCK_INIT;
    std::unique_ptr<std::atomic<uint16_t>[]> releasing;   // per page: releasers that saw it armed and have not finished its VirtualProtect (release_page)
};

struct SharedLock {
    explicit SharedLock(Region* r) : r_(r) { AcquireSRWLockShared(&r_->lock); }
    ~SharedLock() { ReleaseSRWLockShared(&r_->lock); }
    Region* r_;
};
struct ExclusiveLock {
    explicit ExclusiveLock(Region* r) : r_(r) { AcquireSRWLockExclusive(&r_->lock); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&r_->lock); }
    Region* r_;
};

namespace {

constexpr const char* kTag = "mem.ww";
constexpr int kSlots = 512;
std::atomic<Region*> g_slots[kSlots];
std::atomic<int> g_hi{0};              // slots [0, g_hi) may be in use
std::atomic<int> g_busy{0};            // fault handlers and os-write calls looking at the table
std::atomic<int> g_live{0};
std::atomic<uint64_t> g_faults{0};
std::atomic<uint64_t> g_waits{0};      // rounds a fault waited for another releaser (statistics, tests)
std::atomic<uint64_t> g_guard_faults{0};   // accesses that hit a guarded page
std::atomic<GuardHandler> g_guard_fn{nullptr};
std::mutex g_mu;                       // create / destroy
PVOID g_veh = nullptr;

// Operating-system writes in progress (OsWriteScope): arm() leaves these ranges alone, so a page cannot become read-only between the
// release in before_os_write and the end of the ReadFile that fills it. When every slot is taken, g_os_overflow stops all arming.
constexpr int kOsSlots = 32;
struct OsRange { std::atomic<uint64_t> lo{0}, hi{0}; };
OsRange g_os[kOsSlots];
std::atomic<int> g_os_overflow{0};

bool os_write_overlaps(uint64_t lo, uint64_t hi) {
    if (g_os_overflow.load(std::memory_order_seq_cst)) return true;
    for (OsRange& o : g_os) {
        const uint64_t h = o.hi.load(std::memory_order_seq_cst);
        if (h && o.lo.load(std::memory_order_seq_cst) < hi && lo < h) return true;
    }
    return false;
}

uint64_t bit(uint64_t page) { return uint64_t{1} << (page & 63); }

// A host page (4 KiB on Windows, 16 KiB on Apple silicon) is the smallest range the OS can protect, and the POSIX shim applies a
// host page the union of its guest pages' protections (os_posix_mem.cpp, host_page_protect): arming one 4 KiB guest page of a
// 16 KiB host page would leave the page writable, so the fault this tracker lives on would never come and the owner's copy would
// go stale. Every range arm(), guard() and release_page() touch is rounded out to the host page and the bits go with it, so the
// bitmap and the protection always agree. On a 4 KiB host page these helpers change nothing.
uint64_t host_pages() {
    static const uint64_t n = [] {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return std::max<uint64_t>(1, static_cast<uint64_t>(si.dwPageSize) / kPage);
    }();
    return n;
}

// The guest pages of the host page `page` shares, clamped to the region.
void host_group(const Region* r, uint64_t page, uint64_t& first, uint64_t& count) {
    const uint64_t n = host_pages();
    first = page & ~(n - 1);
    count = std::min(n, r->npages - first);
}

// [first, first + count) rounded out to whole host pages (clamped to the region): first and count in, first and count out.
void to_host_pages(const Region* r, uint64_t& first, uint64_t& count) {
    if (host_pages() == 1 || !count) return;
    const uint64_t last = std::min<uint64_t>(first + count, r->npages);
    uint64_t f0 = 0, c0 = 0, f1 = 0, c1 = 0;
    host_group(r, first, f0, c0);
    host_group(r, last - 1, f1, c1);
    first = f0;
    count = f1 + c1 - f0;
}

std::atomic<bool> g_sum_scan{true};                    // take_written reads wsum (set_summary_scan); off: every word, as before
std::atomic<uint64_t> g_st_takes{0}, g_st_takes_empty{0}, g_st_take_pages{0}, g_st_take_words{0};

// The written bits of pages [first, first + count) are set: their summary bits follow (after, so a reader that clears a summary bit and
// then reads the word sees every page whose summary bit it cleared).
void sum_mark(Region* r, uint64_t first, uint64_t count) {
    if (!count) return;
    const uint64_t w0 = first >> 6, w1 = (first + count - 1) >> 6;
    for (uint64_t w = w0; w <= w1;) {
        const uint64_t s = w >> 6, lo = w & 63, hi = std::min<uint64_t>(64, lo + (w1 - w + 1));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        r->wsum[s].fetch_or(mask, std::memory_order_acq_rel);
        w += hi - lo;
    }
}

bool test(const std::atomic<uint64_t>* bm, uint64_t page) { return (bm[page >> 6].load(std::memory_order_acquire) & bit(page)) != 0; }

// Sets or clears the bits of pages [first, first + count) word by word.
void set_bits(std::atomic<uint64_t>* bm, uint64_t first, uint64_t count, bool on) {
    uint64_t p = first;
    const uint64_t end = first + count;
    while (p < end) {
        const uint64_t w = p >> 6;
        const uint64_t lo = p & 63;
        const uint64_t hi = std::min<uint64_t>(64, lo + (end - p));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        if (on) bm[w].fetch_or(mask, std::memory_order_acq_rel);
        else bm[w].fetch_and(~mask, std::memory_order_acq_rel);
        p += hi - lo;
    }
}

bool all_set(const std::atomic<uint64_t>* bm, uint64_t first, uint64_t count) {
    uint64_t p = first;
    const uint64_t end = first + count;
    while (p < end) {
        const uint64_t w = p >> 6;
        const uint64_t lo = p & 63;
        const uint64_t hi = std::min<uint64_t>(64, lo + (end - p));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        if ((bm[w].load(std::memory_order_acquire) & mask) != mask) return false;
        p += hi - lo;
    }
    return true;
}

bool any_set(const std::atomic<uint64_t>* bm, uint64_t first, uint64_t count) {
    uint64_t p = first;
    const uint64_t end = first + count;
    while (p < end) {
        const uint64_t w = p >> 6;
        const uint64_t lo = p & 63;
        const uint64_t hi = std::min<uint64_t>(64, lo + (end - p));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        if (bm[w].load(std::memory_order_acquire) & mask) return true;
        p += hi - lo;
    }
    return false;
}

DWORD page_protect(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    return mbi.Protect;
}

// One page of a region becomes writable and counts as written. True when the page is ours to handle (the faulting write may retry).
// The caller holds the region's lock shared.
bool release_page(Region* r, uint64_t page) {
    // The whole host page becomes writable, so every guest page on it is released (and counts as written) together: their bits
    // must not keep saying "armed" while the OS lets writes through.
    uint64_t g0 = 0, gn = 0;
    host_group(r, page, g0, gn);
    const uint64_t b = bit(g0);
    const uint64_t w = g0 >> 6;
    // Announce the release before the bit goes: whoever finds the bit already clear can then tell "somebody is making this page writable"
    // (the count is up from before the bit went until after the VirtualProtect) from "nobody is": a foreign read-only page.
    std::atomic<uint16_t>& rel = r->releasing[g0];
    const bool armed = (r->prot[w].load(std::memory_order_acquire) & b) != 0;
    if (armed) rel.fetch_add(1, std::memory_order_seq_cst);
    const uint64_t old = r->prot[w].fetch_and(~b, std::memory_order_acq_rel);
    set_bits(r->prot.get(), g0, gn, false);
    set_bits(r->valid.get(), g0, gn, false);
    set_bits(r->written.get(), g0, gn, true);
    sum_mark(r, g0, gn);
    const uint64_t addr = r->base + g0 * kPage;
    const SIZE_T len = static_cast<SIZE_T>(gn * kPage);
    if (old & b) {
        bool ok = true;
        DWORD was = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_READWRITE, &was)) {
            ok = false;
        } else if ((was & 0xff) != PAGE_READONLY) {    // somebody else set this protection: give it back and let the fault through
            DWORD tmp = 0;
            VirtualProtect(reinterpret_cast<void*>(addr), len, was, &tmp);
            ok = false;
        }
        rel.fetch_sub(1, std::memory_order_seq_cst);
        return ok;
    }
    if (armed) rel.fetch_sub(1, std::memory_order_seq_cst);   // lost the race for the bit: not a releaser (and must not make others wait for it)
    // Not armed (any more): another releaser (also holding the lock shared) may be making it writable right now. Wait for it only while
    // such a release is in progress; with none, a read-only page is somebody else's and the fault goes on at once. The count is read before
    // the protection: a releaser that cleared the bit before our fetch_and has counted itself in before that, so a zero count means every
    // such release has finished and its effect is visible in the protection we read next.
    // The wait has no round limit: a releaser that is counted in will finish (it only has its VirtualProtect left), but it may be descheduled
    // for a long time on a busy machine, and giving up early would pass a fault on that is ours (seen as an unhandled access violation).
    for (int i = 0;; ++i) {
        const uint16_t busy = rel.load(std::memory_order_seq_cst);
        const DWORD pr = page_protect(addr) & 0xff;
        if (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE) return true;
        if (pr != PAGE_READONLY || busy == 0) return false;
        g_waits.fetch_add(1, std::memory_order_relaxed);
        if (i < 50) YieldProcessor();
        else if (i < 1000) Sleep(0);
        else Sleep(1);
    }
}

// [diag] ww_stats (env NXRT_DIAG_WW_STATS=1): faults per region and per 64 KiB block, the 10 busiest blocks logged every 10 s.
std::mutex g_fs_lock;
std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_fs_blocks;   // (region base, 64 KiB block address) -> faults
std::unordered_map<uint64_t, uint64_t> g_fs_regions;     // region base -> faults
ULONGLONG g_fs_since = 0;
void fault_stat(const Region* r, uint64_t addr) {
    static const bool on = config_get_bool("diag", "ww_stats", false);
    if (!on) return;
    std::lock_guard<std::mutex> lk(g_fs_lock);
    g_fs_blocks[{r->base, addr & ~0xffffull}]++;
    g_fs_regions[r->base]++;
    const ULONGLONG now = GetTickCount64();
    if (!g_fs_since) g_fs_since = now;
    if (now - g_fs_since < 10000) return;
    const double secs = static_cast<double>(now - g_fs_since) / 1000.0;
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> v(g_fs_blocks.begin(), g_fs_blocks.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [base, n] : g_fs_regions) NXLOG_I("ww", "faults over %.1f s: region %llx: %.0f/s", secs, static_cast<unsigned long long>(base), n / secs);
    for (size_t i = 0; i < v.size() && i < 10; ++i)
        NXLOG_I("ww", "  region %llx block %llx: %.0f faults/s", static_cast<unsigned long long>(v[i].first.first), static_cast<unsigned long long>(v[i].first.second),
                v[i].second / secs);
    g_fs_blocks.clear();
    g_fs_regions.clear();
    g_fs_since = now;
}

// The guarded pages among [first, first + count) become read-only (armed) again. The caller holds the region's lock exclusive.
void unguard_locked(Region* r, uint64_t first, uint64_t count) {
    for (uint64_t p = first; p < first + count;) {
        if (!test(r->guard.get(), p)) { ++p; continue; }
        uint64_t q = p;
        while (q < first + count && test(r->guard.get(), q)) ++q;
        // protection first, then the bit: a fault that finds the bit clear must find the page accessible (the read path takes no lock)
        DWORD was = 0;
        VirtualProtect(reinterpret_cast<void*>(r->base + p * kPage), static_cast<SIZE_T>((q - p) * kPage), PAGE_READONLY, &was);
        set_bits(r->guard.get(), p, q - p, false);
        p = q;
    }
}

bool guarded_at(uint64_t addr) {
    bool any = false;
    g_busy.fetch_add(1, std::memory_order_seq_cst);
    const int hi = g_hi.load(std::memory_order_seq_cst);
    for (int i = 0; i < hi && !any; ++i) {
        Region* r = g_slots[i].load(std::memory_order_seq_cst);
        if (!r || addr < r->base || addr >= r->base + r->npages * kPage) continue;
        any = test(r->guard.get(), (addr - r->base) / kPage);
    }
    g_busy.fetch_sub(1, std::memory_order_seq_cst);
    return any;
}

void drop_guard(uint64_t addr, uint64_t size);   // below

LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    // macOS reports a write to a page this process made read-only as SIGBUS (EXCEPTION_IN_PAGE_ERROR), not as an access
    // violation; both reach here with the write/address parameters the shim fills.
    if ((rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && rec->ExceptionCode != EXCEPTION_IN_PAGE_ERROR) || rec->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR kind = rec->ExceptionInformation[0];     // 0 read, 1 write, 8 execute
    if (kind != 0 && kind != 1) return EXCEPTION_CONTINUE_SEARCH;
    if (g_live.load(std::memory_order_acquire) == 0) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t addr = static_cast<uint64_t>(rec->ExceptionInformation[1]);
    nxrt::HitchTimer ht(nxrt::kHpFaults);      // the hitch line: time in this handler (a wait for arm's exclusive lock included)
    // A guarded page (read or write): the owner makes it current first (outside every lock: it may wait for the GPU). A write then faults
    // again on the read-only page and is tracked as usual.
    if (guarded_at(addr)) {
        g_guard_faults.fetch_add(1, std::memory_order_relaxed);
        if (GuardHandler fn = g_guard_fn.load(std::memory_order_acquire)) fn(addr & ~(kPage - 1));
        drop_guard(addr & ~(kPage - 1), kPage);   // still guarded (no handler, or it found nothing): read-only again, as it is
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (kind != 1) {
        // a guard dropped by another thread since the access: the page is readable again, let the read retry
        const DWORD pr = page_protect(addr) & 0xff;
        if (pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE) {
            bool in_region = false;
            const int hi0 = g_hi.load(std::memory_order_seq_cst);
            for (int i = 0; i < hi0 && !in_region; ++i) {
                Region* r = g_slots[i].load(std::memory_order_seq_cst);
                in_region = r && addr >= r->base && addr < r->base + r->npages * kPage;
            }
            if (in_region) return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    bool ours = false;
    g_busy.fetch_add(1, std::memory_order_seq_cst);
    const int hi = g_hi.load(std::memory_order_seq_cst);
    for (int i = 0; i < hi; ++i) {
        Region* r = g_slots[i].load(std::memory_order_seq_cst);
        if (!r || addr < r->base || addr >= r->base + r->npages * kPage) continue;
        SharedLock lk(r);
        const bool mine = release_page(r, (addr - r->base) / kPage);
        if (mine) fault_stat(r, addr);
        ours |= mine;
    }
    g_busy.fetch_sub(1, std::memory_order_seq_cst);
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    g_faults.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Calls fn(region, first page, count) for the part of [addr, addr + size) inside every published region.
template <class Fn>
void for_regions(uint64_t addr, uint64_t size, Fn&& fn) {
    if (g_live.load(std::memory_order_acquire) == 0 || size == 0) return;
    g_busy.fetch_add(1, std::memory_order_seq_cst);
    const int hi = g_hi.load(std::memory_order_seq_cst);
    for (int i = 0; i < hi; ++i) {
        Region* r = g_slots[i].load(std::memory_order_seq_cst);
        if (!r) continue;
        const uint64_t lo = std::max(addr, r->base), end = std::min(addr + size, r->base + r->npages * kPage);
        if (lo >= end) continue;
        const uint64_t first = (lo - r->base) / kPage, last = (end - 1 - r->base) / kPage;
        SharedLock lk(r);
        fn(r, first, last - first + 1);
    }
    g_busy.fetch_sub(1, std::memory_order_seq_cst);
}

void drop_guard(uint64_t addr, uint64_t size) {
    if (g_live.load(std::memory_order_acquire) == 0 || size == 0) return;
    g_busy.fetch_add(1, std::memory_order_seq_cst);
    const int hi = g_hi.load(std::memory_order_seq_cst);
    for (int i = 0; i < hi; ++i) {
        Region* r = g_slots[i].load(std::memory_order_seq_cst);
        if (!r) continue;
        const uint64_t lo = std::max(addr, r->base), end = std::min(addr + size, r->base + r->npages * kPage);
        if (lo >= end) continue;
        const uint64_t first = (lo - r->base) / kPage, last = (end - 1 - r->base) / kPage;
        uint64_t f = first, c = last - first + 1;
        to_host_pages(r, f, c);
        if (!any_set(r->guard.get(), f, c)) continue;
        ExclusiveLock lk(r);
        unguard_locked(r, f, c);
    }
    g_busy.fetch_sub(1, std::memory_order_seq_cst);
}

struct Gauges {
    Gauges() {
        nxrt::gauge_register("mem.ww.faults", [] { return static_cast<int64_t>(g_faults.load(std::memory_order_relaxed)); });
        nxrt::gauge_register("mem.ww.regions", [] { return static_cast<int64_t>(g_live.load(std::memory_order_relaxed)); });
    }
    ~Gauges() {
        nxrt::gauge_unregister("mem.ww.faults");
        nxrt::gauge_unregister("mem.ww.regions");
    }
};
Gauges g_gauges;

}  // namespace

Region* region_create(uint64_t addr, uint64_t size) {
    if (!size || !heap_exists()) return nullptr;
    const uint64_t start = addr & ~(kPage - 1), end = (addr + size + kPage - 1) & ~(kPage - 1);
    if (start < kHeapBase || end > kHeapBase + heap_reserved()) return nullptr;
    auto r = std::make_unique<Region>();
    r->base = start;
    r->npages = (end - start) / kPage;
    r->nwords = (r->npages + 63) / 64;
    for (auto* bm : {&r->prot, &r->valid, &r->written, &r->ban, &r->guard}) {
        bm->reset(new std::atomic<uint64_t>[r->nwords]);
        for (uint64_t i = 0; i < r->nwords; ++i) (*bm)[i].store(0, std::memory_order_relaxed);
    }
    r->releasing.reset(new std::atomic<uint16_t>[r->npages]);
    for (uint64_t i = 0; i < r->npages; ++i) r->releasing[i].store(0, std::memory_order_relaxed);
    r->nsum = (r->nwords + 63) / 64;
    r->wsum.reset(new std::atomic<uint64_t>[r->nsum]);
    for (uint64_t i = 0; i < r->nsum; ++i) r->wsum[i].store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_fault);
        if (!g_veh) {
            NXLOG_W(kTag, "AddVectoredExceptionHandler failed: no write tracking");
            return nullptr;
        }
        int slot = -1;
        for (int i = 0; i < kSlots; ++i)
            if (!g_slots[i].load()) { slot = i; break; }
        if (slot < 0) {
            NXLOG_W(kTag, "region table full (%d): no write tracking for %llx", kSlots, static_cast<unsigned long long>(start));
            return nullptr;
        }
        r->slot = slot;
        g_slots[slot].store(r.get(), std::memory_order_seq_cst);
        if (slot >= g_hi.load()) g_hi.store(slot + 1, std::memory_order_seq_cst);
        g_live.fetch_add(1, std::memory_order_seq_cst);
    }
    // Pages that another view shows can never be tracked. Published first, so an alias made from now on bans them through ban_section.
    std::vector<std::pair<uint64_t, uint64_t>> views;
    heap_alias_ranges(views);
    for (const auto& v : views) {
        const uint64_t lo = std::max(start, kHeapBase + v.first), hi = std::min(end, kHeapBase + v.first + v.second);
        if (lo < hi) set_bits(r->ban.get(), (lo - start) / kPage, (hi - lo) / kPage, true);
    }
    return r.release();
}

void region_destroy(Region* r) {
    if (!r) return;
    // Writable again first (faults on armed pages wait for the lock and then find the page writable), then out of the table.
    {
    ExclusiveLock lk(r);
    for (uint64_t p = 0; p < r->npages;) {
        if (!test(r->prot.get(), p)) { ++p; continue; }
        uint64_t q = p;
        while (q < r->npages && test(r->prot.get(), q)) ++q;
        set_bits(r->prot.get(), p, q - p, false);
        set_bits(r->guard.get(), p, q - p, false);
        DWORD was = 0;
        VirtualProtect(reinterpret_cast<void*>(r->base + p * kPage), static_cast<SIZE_T>((q - p) * kPage), PAGE_READWRITE, &was);
        p = q;
    }
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_slots[r->slot].store(nullptr, std::memory_order_seq_cst);
        g_live.fetch_sub(1, std::memory_order_seq_cst);
    }
    while (g_busy.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
    delete r;
}

uint64_t region_base(const Region* r) { return r ? r->base : 0; }

bool arm(Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    nxrt::HitchTimer ht(nxrt::kHpWwArm);
    ExclusiveLock lk(r);
    to_host_pages(r, first, count);
    if (any_set(r->ban.get(), first, count)) return false;
    const uint64_t a = r->base + first * kPage, n = count * kPage;
    // A file read (or a mapping change in memory.cpp) into these pages is in progress: leave them alone. Its scope is published before it
    // releases pages under the shared lock, so either this sees it, or its release waits for this arm and undoes it.
    if (os_write_overlaps(a, a + n)) return false;
    // every page committed read-write (the guest may have changed a protection; a page it made read-only is not ours to take)
    for (uint64_t cur = a; cur < a + n;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
        const DWORD pr = mbi.Protect & 0xff;
        const bool already = pr == PAGE_READONLY && all_set(r->prot.get(), (cur - r->base) / kPage,
                                                            (std::min<uint64_t>(a + n, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize) - cur) / kPage);
        if ((pr != PAGE_READWRITE && !already) || (mbi.Protect & PAGE_GUARD)) return false;
        cur = std::min<uint64_t>(a + n, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    // Under the exclusive lock no fault is handled until both bits and protection are in place; a write that faults meanwhile waits and
    // then releases the page. A write that lands before the protection does not fault, and the owner's copy (made later) still sees it.
    set_bits(r->prot.get(), first, count, true);
    set_bits(r->valid.get(), first, count, true);
    DWORD was = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(a), static_cast<SIZE_T>(n), PAGE_READONLY, &was)) {
        set_bits(r->valid.get(), first, count, false);
        set_bits(r->prot.get(), first, count, false);
        return false;
    }
    return true;
}

bool all_valid(const Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    return all_set(r->valid.get(), first, count);
}

bool page_valid(const Region* r, uint64_t page) { return r && page < r->npages && test(r->valid.get(), page); }
bool page_banned(const Region* r, uint64_t page) { return !r || page >= r->npages || test(r->ban.get(), page); }

void invalidate(Region* r, uint64_t first, uint64_t count) {
    if (!r || first >= r->npages) return;
    count = std::min(count, r->npages - first);
    if (count) set_bits(r->valid.get(), first, count, false);
}

// Under the exclusive lock no fault of the region is handled, so bits and protection change together for everybody else, as in arm(). A
// write that faulted on one of the pages just before finds it writable afterwards and goes on (release_page: not armed, read-write).
uint64_t owner_write_begin(Region* r, uint64_t first, uint64_t count) {
    if (!r || first >= r->npages) return 0;
    count = std::min(count, r->npages - first);
    ExclusiveLock lk(r);
    uint64_t n = 0;
    while (n < count && test(r->valid.get(), first + n) && test(r->prot.get(), first + n) && !test(r->guard.get(), first + n)) ++n;
    if (!n) return 0;
    DWORD was = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(r->base + first * kPage), static_cast<SIZE_T>(n * kPage), PAGE_READWRITE, &was)) return 0;
    set_bits(r->valid.get(), first, n, false);
    set_bits(r->prot.get(), first, n, false);
    return n;
}

bool owner_write_end(Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    if (arm(r, first, count)) return true;
    set_bits(r->written.get(), first, count, true);
    sum_mark(r, first, count);
    return false;
}

void set_guard_handler(GuardHandler fn) { g_guard_fn.store(fn, std::memory_order_release); }

bool guard(Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    ExclusiveLock lk(r);
    to_host_pages(r, first, count);
    if (!all_set(r->prot.get(), first, count) || any_set(r->ban.get(), first, count)) return false;
    const uint64_t a = r->base + first * kPage, n = count * kPage;
    if (os_write_overlaps(a, a + n)) return false;
    set_bits(r->guard.get(), first, count, true);
    DWORD was = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(a), static_cast<SIZE_T>(n), PAGE_NOACCESS, &was)) {
        VirtualProtect(reinterpret_cast<void*>(a), static_cast<SIZE_T>(n), PAGE_READONLY, &was);
        set_bits(r->guard.get(), first, count, false);
        return false;
    }
    return true;
}

void unguard(Region* r, uint64_t first, uint64_t count) {
    if (!r || first >= r->npages) return;
    count = std::min(count, r->npages - first);
    if (!count) return;
    to_host_pages(r, first, count);
    if (!any_set(r->guard.get(), first, count)) return;
    ExclusiveLock lk(r);
    unguard_locked(r, first, count);
}

bool all_armed(const Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    return all_set(r->prot.get(), first, count);
}

uint64_t guard_faults() { return g_guard_faults.load(std::memory_order_relaxed); }

namespace {
void take_word(Region* r, uint64_t w, std::vector<uint64_t>& out) {
    if (!r->written[w].load(std::memory_order_relaxed)) return;
    uint64_t v = r->written[w].exchange(0, std::memory_order_acq_rel);
    while (v) {
        const unsigned long b = static_cast<unsigned long>(__builtin_ctzll(v));
        v &= v - 1;
        const uint64_t page = w * 64 + b;
        if (page < r->npages) out.push_back(page);
    }
}
}  // namespace

// A write that completed before this call (its fault handler returned: both its written bit and its summary bit are set) is found: the
// summary bit is cleared (exchange) before its word is read, so a bit set in between is either read now or leaves its summary bit for the
// next call. Same result as reading every word; a write racing with the call shows here or in the next call, as before.
void take_written(Region* r, std::vector<uint64_t>& out) {
    if (!r) return;
    const size_t n0 = out.size();
    g_st_takes.fetch_add(1, std::memory_order_relaxed);
    if (g_sum_scan.load(std::memory_order_relaxed)) {
        uint64_t words = 0;
        for (uint64_t s = 0; s < r->nsum; ++s) {
            if (!r->wsum[s].load(std::memory_order_relaxed)) continue;
            uint64_t v = r->wsum[s].exchange(0, std::memory_order_acq_rel);
            while (v) {
                const uint64_t w = s * 64 + static_cast<uint64_t>(__builtin_ctzll(v));
                v &= v - 1;
                ++words;
                if (w < r->nwords) take_word(r, w, out);
            }
        }
        g_st_take_words.fetch_add(words, std::memory_order_relaxed);
    } else {
        for (uint64_t w = 0; w < r->nwords; ++w) take_word(r, w, out);
        for (uint64_t s = 0; s < r->nsum; ++s) r->wsum[s].store(0, std::memory_order_relaxed);   // (stale bits only cost a word read later)
        g_st_take_words.fetch_add(r->nwords, std::memory_order_relaxed);
    }
    if (out.size() == n0) g_st_takes_empty.fetch_add(1, std::memory_order_relaxed);
    g_st_take_pages.fetch_add(out.size() - n0, std::memory_order_relaxed);
}

void set_summary_scan(bool on) { g_sum_scan.store(on, std::memory_order_relaxed); }

TakeStats take_stats() {
    TakeStats t;
    t.calls = g_st_takes.load(std::memory_order_relaxed);
    t.empty = g_st_takes_empty.load(std::memory_order_relaxed);
    t.pages = g_st_take_pages.load(std::memory_order_relaxed);
    t.words = g_st_take_words.load(std::memory_order_relaxed);
    return t;
}

namespace {
// An OS-call slot over [p, p + n) (OsWriteScope, OsReadScope): arm() and guard() leave the range alone while it is held.
int os_slot_acquire(const void* p, uint64_t n) {
    const uint64_t lo = reinterpret_cast<uint64_t>(p) & ~(kPage - 1), hi = (reinterpret_cast<uint64_t>(p) + n + kPage - 1) & ~(kPage - 1);
    for (int i = 0; i < kOsSlots; ++i) {
        uint64_t expect = 0;
        if (g_os[i].hi.load(std::memory_order_relaxed) == 0 && g_os[i].lo.compare_exchange_strong(expect, lo | 1, std::memory_order_seq_cst)) {
            g_os[i].lo.store(lo, std::memory_order_seq_cst);
            g_os[i].hi.store(hi, std::memory_order_seq_cst);
            return i;
        }
    }
    g_os_overflow.fetch_add(1, std::memory_order_seq_cst);
    return kOsSlots;
}

void os_slot_release(int slot) {
    if (slot < 0) return;
    if (slot == kOsSlots) {
        g_os_overflow.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    g_os[slot].hi.store(0, std::memory_order_seq_cst);
    g_os[slot].lo.store(0, std::memory_order_seq_cst);
}
}  // namespace

OsWriteScope::OsWriteScope(const void* p, uint64_t n) {
    if (!n) return;
    slot_ = os_slot_acquire(p, n);
    before_os_write(p, n);
}

OsWriteScope::~OsWriteScope() { os_slot_release(slot_); }

OsReadScope::OsReadScope(const void* p, uint64_t n) {
    if (!n) return;
    slot_ = os_slot_acquire(p, n);   // first: guard() cannot take a page of the range back between the fetch and the OS call
    before_os_read(p, n);
}

OsReadScope::~OsReadScope() { os_slot_release(slot_); }

void before_os_read(const void* p, uint64_t n) {
    // guarded pages only: their owner makes them current and they become read-only (armed) again, which the OS can read. Armed pages
    // stay armed: a read does not change them.
    if (!n || g_live.load(std::memory_order_acquire) == 0) return;
    const uint64_t lo = reinterpret_cast<uint64_t>(p) & ~(kPage - 1), hi = reinterpret_cast<uint64_t>(p) + n;
    for (uint64_t a = lo; a < hi; a += kPage)
        if (guarded_at(a)) {
            if (GuardHandler fn = g_guard_fn.load(std::memory_order_acquire)) fn(a);
            drop_guard(a, kPage);
        }
}

void before_os_write(const void* p, uint64_t n) {
    // guarded pages first: their owner makes them current (the write may cover only part of a page), outside the region locks
    if (n && g_live.load(std::memory_order_acquire) != 0) {
        const uint64_t lo = reinterpret_cast<uint64_t>(p) & ~(kPage - 1), hi = reinterpret_cast<uint64_t>(p) + n;
        for (uint64_t a = lo; a < hi; a += kPage)
            if (guarded_at(a)) {
                if (GuardHandler fn = g_guard_fn.load(std::memory_order_acquire)) fn(a);
                drop_guard(a, kPage);
            }
    }
    for_regions(reinterpret_cast<uint64_t>(p), n, [](Region* r, uint64_t first, uint64_t count) {
        for (uint64_t q = first; q < first + count; ++q)
            if (test(r->prot.get(), q)) release_page(r, q);
    });
}

void forget(uint64_t addr, uint64_t size, bool ban) {
    for_regions(addr, size, [ban](Region* r, uint64_t first, uint64_t count) {
        if (ban) set_bits(r->ban.get(), first, count, true);
        if (!any_set(r->prot.get(), first, count) && !any_set(r->valid.get(), first, count)) return;
        for (uint64_t q = first; q < first + count; ++q)           // guarded pages: read-only first (the owner's newer copy is dropped)
            if (test(r->guard.get(), q)) {              // protection first, then the bit (see unguard_locked)
                DWORD was = 0;
                VirtualProtect(reinterpret_cast<void*>(r->base + q * kPage), kPage, PAGE_READONLY, &was);
                r->guard[q >> 6].fetch_and(~bit(q), std::memory_order_acq_rel);
            }
        for (uint64_t q = first; q < first + count; ++q)           // armed pages go back to read-write (the caller may then set its own)
            if (test(r->prot.get(), q)) release_page(r, q);
        set_bits(r->valid.get(), first, count, false);
        set_bits(r->written.get(), first, count, true);
        sum_mark(r, first, count);
    });
}

void ban_section(uint64_t offset, uint64_t size) { forget(kHeapBase + offset, size, true); }

uint64_t faults() { return g_faults.load(std::memory_order_relaxed); }

}  // namespace nxrt::mem::ww
