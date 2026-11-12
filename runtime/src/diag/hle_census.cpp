// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime census: see hle_census.h.
//
// Storage. Each thread that enters a scope gets a Block of its own on its first entry: a cell (TSC ticks, calls) per SVC number, per hook
// site kind and per hook id, and the HLE thunks' cells in pages of 1,024 made on first use (thunk index = (address - kHleBase) / 16). The
// owner updates its cells with a relaxed load and a relaxed store (plain moves, no locked instruction); the reporter reads them racily.
// Blocks are never moved; a block whose thread has ended is reported one last time, then freed. The scope stack (start, nested time) is
// the owner's only.
//
// Time. A scope's time is TSC ticks from its enter to its leave, less the time of the scopes nested in it (exclusive time). Ticks become
// nanoseconds through ft::tsc_to_ns (calibrated against QueryPerformanceCounter). Cost when on: two rdtsc and a few loads and stores per
// scope, a few tens of ns; a scope's own time includes about one rdtsc of that.
//
// Blocking. SVCs that wait (SleepThread, WaitSynchronization, ArbitrateLock, WaitProcessWideKeyAtomic, WaitForAddress, ReplyAndReceive) are
// wall time, not processor time: they are listed apart ("waits") and are not in the "in runtime" sum. Host waits inside an HLE function or
// a hook (a host fence wait, say) are not told apart: they count as that function's time; a share near or over 100 % shows it.
//
// The line (tag "hcensus"), every 10 s at the end of the first outermost scope after the period, per thread with more than 10 ms/s of
// processor time (GetThreadTimes, as the tcpu line):
//   runtime census over 10.0 s, guest-103: cpu 662 ms/s (66.2%), in runtime 301.5 ms/s (45.5% of its cpu) in 412000 calls/s; top:
//   nvnCommandBufferDrawElements 18000/s 2100 ns 37.8 ms/s, svc:SendSyncRequest 900/s 14000 ns 12.6 ms/s, ...; waits 120.4 ms/s:
//   svc:WaitSynchronization 800/s 150000 ns 120.0 ms/s, ...
#include "hle_census.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/ftrace.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"

namespace nxrt::diag::census {

bool g_on = false;

namespace {

constexpr const char* kTag = "hcensus";
constexpr uint64_t kPeriodNs = 10000000000ull;
constexpr double kMinCpuMs = 10.0;        // ms/s: a thread under 1 % of a processor gets no line
constexpr size_t kTop = 12, kTopWaits = 4;
constexpr uint32_t kPage = 1024;
constexpr uint32_t kHlePages = static_cast<uint32_t>(mem::kHleSize / 16 / kPage);
constexpr uint32_t kHooks = 256;          // hook ids at or over this share the last cell
constexpr uint32_t kMaxDepth = 64;        // deeper scopes are not counted (their time stays in the scope below)

struct Cell {
    std::atomic<uint64_t> ticks{0}, calls{0};   // owner: relaxed load + store
    uint64_t prev_ticks = 0, prev_calls = 0;    // reporter only
};
inline void add(std::atomic<uint64_t>& a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }

struct Page {
    Cell c[kPage];
};

struct Frame {
    uint64_t t0, child;
    Cell* cell;
};

struct Block {
    // owner only
    Frame stack[kMaxDepth];
    uint32_t depth = 0;
    // owner writes, reporter reads
    Cell svc[kSvcCount];
    Cell site[3];
    Cell hook[kHooks];
    std::atomic<Page*> hle[kHlePages] = {};
    // fixed at attach
    std::string name;
    uint32_t tid = 0;
    HANDLE h = nullptr;
    uint64_t prev_cpu = 0;                      // reporter only: thread CPU time at the previous line, 100 ns units
    Block* next = nullptr;
    ~Block() {
        for (auto& p : hle) delete p.load(std::memory_order_relaxed);
        if (h) CloseHandle(h);
    }
};

std::mutex g_list_lock;                         // the block list and the report
Block* g_blocks = nullptr;
thread_local Block* t_block = nullptr;
std::atomic<uint64_t> g_next_report{~0ull};     // TSC of the next line
uint64_t g_last_report = 0;                     // under g_list_lock
uint64_t g_period_ticks = 0;                    // kPeriodNs in TSC ticks, set by init()
std::atomic<uint64_t> g_hook_site[kHooks];      // (kind << 62) | site address, for the names
bool g_blocking[kSvcCount];

uint64_t ft64(const FILETIME& f) { return static_cast<uint64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime; }
uint64_t thread_cpu(HANDLE h) {
    FILETIME c, e, k, u;
    return h && GetThreadTimes(h, &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
}

Block* attach() {
    auto* b = new Block;
    b->tid = GetCurrentThreadId();
    if (const kernel::KThread* t = kernel::current_thread()) b->name = t->name;
    b->h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, b->tid);
    b->prev_cpu = thread_cpu(b->h);
    std::lock_guard<std::mutex> lk(g_list_lock);
    b->next = g_blocks;
    g_blocks = b;
    return t_block = b;
}

Cell* cell_of(Block& b, uint32_t k) {
    const uint32_t i = k & 0x0fffffffu;
    switch (static_cast<Kind>(k >> 28)) {
        case kSvc: return &b.svc[i < kSvcCount ? i : kSvcCount - 1];
        case kSite: return &b.site[i < 3 ? i : 2];
        case kHook: return &b.hook[i < kHooks ? i : kHooks - 1];
        case kHle: break;
    }
    const uint32_t pg = i / kPage;
    if (pg >= kHlePages) return nullptr;
    Page* p = b.hle[pg].load(std::memory_order_relaxed);
    if (!p) {
        p = new Page;
        b.hle[pg].store(p, std::memory_order_release);
    }
    return &p->c[i % kPage];
}

std::string name_of(uint32_t k) {
    const uint32_t i = k & 0x0fffffffu;
    char b[160];
    switch (static_cast<Kind>(k >> 28)) {
        case kSvc: std::snprintf(b, sizeof b, "svc:%s", svc_name(i)); return b;
        case kSite: return i == 0 ? "site:inline" : i == 1 ? "site:mid" : "site:entry";
        case kHook: {
            const uint64_t v = g_hook_site[i < kHooks ? i : kHooks - 1].load(std::memory_order_relaxed);
            const uint32_t kind = static_cast<uint32_t>(v >> 62);
            std::snprintf(b, sizeof b, "hook:%s %s", kind == 0 ? "inline" : kind == 1 ? "mid" : "entry",
                          module_describe(v & ((1ull << 62) - 1)).c_str());
            return b;
        }
        case kHle: break;
    }
    const char* n = hle_name_at(mem::kHleBase + static_cast<uint64_t>(i) * 16);
    std::snprintf(b, sizeof b, "%.72s", n && *n ? n : "hle:?");
    return b;
}

struct Row {
    uint32_t key;
    uint64_t ticks, calls;
};

// One line for b over secs seconds; rolls its cells' baselines either way.
void report_block(Block& b, double secs, const std::unordered_map<uint32_t, std::string>& log_names) {
    const uint64_t cpu = thread_cpu(b.h);
    const double cpu_ms = static_cast<double>(cpu > b.prev_cpu ? cpu - b.prev_cpu : 0) / 1e4 / secs;
    b.prev_cpu = cpu;
    std::vector<Row> rows, waits;
    uint64_t run_ticks = 0, run_calls = 0, wait_ticks = 0;
    auto take = [&](Cell& c, uint32_t k) {
        const uint64_t t = c.ticks.load(std::memory_order_relaxed), n = c.calls.load(std::memory_order_relaxed);
        const uint64_t dt = t - c.prev_ticks, dn = n - c.prev_calls;
        c.prev_ticks = t;
        c.prev_calls = n;
        if (!dn && !dt) return;
        const bool blocking = (k >> 28) == kSvc && g_blocking[k & 0xff];
        (blocking ? waits : rows).push_back({k, dt, dn});
        if (blocking) {
            wait_ticks += dt;
        } else {
            run_ticks += dt;
            run_calls += dn;
        }
    };
    for (uint32_t i = 0; i < kSvcCount; ++i) take(b.svc[i], key(kSvc, i));
    for (uint32_t i = 0; i < 3; ++i) take(b.site[i], key(kSite, i));
    for (uint32_t i = 0; i < kHooks; ++i) take(b.hook[i], key(kHook, i));
    for (uint32_t pg = 0; pg < kHlePages; ++pg)
        if (Page* p = b.hle[pg].load(std::memory_order_acquire))
            for (uint32_t i = 0; i < kPage; ++i) take(p->c[i], key(kHle, pg * kPage + i));
    if (cpu_ms < kMinCpuMs || (rows.empty() && waits.empty())) return;

    auto ms_s = [secs](uint64_t ticks) { return static_cast<double>(ft::tsc_to_ns(ticks)) / 1e6 / secs; };
    auto by_time = [](const Row& x, const Row& y) { return x.ticks > y.ticks; };
    std::sort(rows.begin(), rows.end(), by_time);
    std::sort(waits.begin(), waits.end(), by_time);
    auto entries = [&](const std::vector<Row>& v, size_t n) {
        std::string s;
        char buf[256];
        for (size_t i = 0; i < v.size() && i < n; ++i) {
            const double ns = v[i].calls ? static_cast<double>(ft::tsc_to_ns(v[i].ticks)) / static_cast<double>(v[i].calls) : 0.0;
            std::snprintf(buf, sizeof buf, "%s%s %.0f/s %.0f ns %.1f ms/s", i ? ", " : "", name_of(v[i].key).c_str(),
                          static_cast<double>(v[i].calls) / secs, ns, ms_s(v[i].ticks));
            s += buf;
        }
        return s;
    };
    std::string name = b.name;
    if (name.empty()) {
        auto it = log_names.find(b.tid);
        name = it != log_names.end() ? it->second : "tid " + std::to_string(b.tid);
    }
    const double run_ms = ms_s(run_ticks);
    NXLOG_I(kTag, "runtime census over %.1f s, %s: cpu %.0f ms/s (%.1f%%), in runtime %.1f ms/s (%.1f%% of its cpu) in %.0f calls/s; top: %s; waits %.1f ms/s%s%s",
            secs, name.c_str(), cpu_ms, cpu_ms / 10.0, run_ms, cpu_ms > 0 ? 100.0 * run_ms / cpu_ms : 0.0, static_cast<double>(run_calls) / secs,
            rows.empty() ? "none" : entries(rows, kTop).c_str(), ms_s(wait_ticks), waits.empty() ? "" : ": ", entries(waits, kTopWaits).c_str());
}

void report(uint64_t now) {
    std::unique_lock<std::mutex> lk(g_list_lock, std::try_to_lock);
    if (!lk.owns_lock() || now < g_next_report.load(std::memory_order_relaxed)) return;
    const double secs = static_cast<double>(ft::tsc_to_ns(now - g_last_report)) / 1e9;
    g_last_report = now;
    g_next_report.store(now + g_period_ticks, std::memory_order_relaxed);
    std::unordered_map<uint32_t, std::string> names;
    log_each_thread_name([](uint32_t tid, uint64_t, const char* n, void* user) {
        if (n[0]) (*static_cast<std::unordered_map<uint32_t, std::string>*>(user))[tid] = n;
    }, &names);
    for (Block** pb = &g_blocks; *pb;) {
        Block* b = *pb;
        DWORD code = STILL_ACTIVE;
        const bool ended = b->h && GetExitCodeThread(b->h, &code) && code != STILL_ACTIVE;
        if (secs > 0) report_block(*b, secs, names);
        if (ended) {   // its thread writes no more: the last interval is reported above
            *pb = b->next;
            delete b;
        } else {
            pb = &b->next;
        }
    }
}

}  // namespace

void init() {
    g_on = config_get_bool("diag", "hle_census", false);
    if (!g_on) return;
    for (uint32_t n : {0x0Bu, 0x18u, 0x1Au, 0x1Cu, 0x34u, 0x43u}) g_blocking[n] = true;
    ft::tsc_init();
    const uint64_t ns_per_g = ft::tsc_to_ns(1000000000ull);   // ns in 1e9 ticks
    g_period_ticks = static_cast<uint64_t>(static_cast<double>(kPeriodNs) * 1e9 / static_cast<double>(ns_per_g ? ns_per_g : 1));
    g_last_report = ft::tsc();
    g_next_report.store(g_last_report + g_period_ticks, std::memory_order_relaxed);
    NXLOG_I(kTag, "runtime census on ([diag] hle_census): every 10 s, per thread, the time inside the runtime by entry point (SVCs, HLE functions, "
            "hook sites; exclusive of nested entries; waiting SVCs apart)");
}

void enter(uint32_t k) {
    Block* b = t_block ? t_block : attach();
    const uint32_t d = b->depth++;
    if (d >= kMaxDepth) return;
    Frame& f = b->stack[d];
    f.cell = cell_of(*b, k);
    f.child = 0;
    f.t0 = ft::tsc();
}

void leave() {
    const uint64_t t1 = ft::tsc();
    Block* b = t_block;
    if (!b || !b->depth) return;
    const uint32_t d = --b->depth;
    if (d < kMaxDepth) {
        Frame& f = b->stack[d];
        const uint64_t dt = t1 - f.t0;
        if (f.cell) {
            add(f.cell->ticks, dt > f.child ? dt - f.child : 0);
            add(f.cell->calls, 1);
        }
        if (d) b->stack[d - 1].child += dt;
    }
    if (!d && t1 >= g_next_report.load(std::memory_order_relaxed)) report(t1);
}

uint32_t depth_now() { return t_block ? t_block->depth : 0; }

void restore_to(uint32_t d) {
    if (t_block && t_block->depth > d) t_block->depth = d;
}

void note_hook(uint32_t id, uint64_t addr, uint32_t kind) {
    g_hook_site[id < kHooks ? id : kHooks - 1].store(static_cast<uint64_t>(kind & 3) << 62 | addr, std::memory_order_relaxed);
}

}  // namespace nxrt::diag::census
