// SPDX-License-Identifier: GPL-3.0-or-later
// Frame trace recorder (include/nxrt/ftrace.h). Events go into a preallocated array claimed with one atomic add; a writer thread
// appends them to the trace file once a second. File format, one record per line:
//   # zone <id> <name>        # thread <index> <os tid> <name>        <t_ns> <dur_ns> <thread index> <zone id> <arg>
#include "nxrt/ftrace.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::ft {

std::atomic<bool> g_on{false};

namespace {

struct Ev {
    uint64_t t0;
    uint32_t dur;      // ns, saturated
    uint16_t zone;
    uint16_t thread;
    uint32_t arg;
};

constexpr uint32_t kMaxThreads = 1024;

Ev* g_ev = nullptr;
size_t g_cap = 0;
std::atomic<size_t> g_n{0};
std::mutex g_mu;                                   // names and the file
std::vector<std::string> g_zone_names;
std::atomic<uint16_t> g_next_thread{0};
std::string g_thread_names[kMaxThreads];
uint32_t g_thread_tids[kMaxThreads];
std::atomic<bool> g_thread_named[kMaxThreads];
std::atomic<bool> g_stop{false};
std::thread g_writer;
FILE* g_file = nullptr;
size_t g_written = 0;
size_t g_zones_written = 0;
size_t g_threads_written = 0;
bool g_started = false;
thread_local int t_index = -1;

int thread_index() {
    if (t_index >= 0) return t_index;
    const uint16_t i = g_next_thread.fetch_add(1);
    if (i >= kMaxThreads) { t_index = static_cast<int>(kMaxThreads - 1); return t_index; }
    g_thread_tids[i] = GetCurrentThreadId();
    t_index = i;
    return t_index;
}

// The names and the thread table (g_mu held: zone_id() and the stop path use the same lock).
void flush_names_locked() {
    if (!g_file) return;
    // names first so the reader knows every id it meets
    for (; g_zones_written < g_zone_names.size(); ++g_zones_written) std::fprintf(g_file, "# zone %zu %s\n", g_zones_written, g_zone_names[g_zones_written].c_str());
    const size_t nt = std::min<size_t>(g_next_thread.load(), kMaxThreads);
    for (; g_threads_written < nt; ++g_threads_written) {
        const char* nm = g_thread_names[g_threads_written].empty() ? "-" : g_thread_names[g_threads_written].c_str();
        std::fprintf(g_file, "# thread %zu %u %s\n", g_threads_written, g_thread_tids[g_threads_written], nm);
    }
}

// The events, formatted and written without g_mu. A second of events is about 100 thousand lines, tens of milliseconds; while the lock was
// held for that long, every thread that records a mark (zone_id takes g_mu) waited for it, so the trace itself made a stall once a second.
// Only the writer thread, and the stop path after it was joined, touch g_written and the file here.
// The lines ("t0 dur thread zone arg") are put together by hand in blocks of 64 KiB: a trace of a game in play is a few hundred thousand
// events a second, and the writer's time is taken from the game being traced, so it does not pay for an fprintf call (stream lock,
// format parsing) per event.
char* put_num(char* p, uint64_t v, char after) {
    char digits[20];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) *p++ = digits[--n];
    *p++ = after;
    return p;
}

void flush_events() {
    if (!g_file) return;
    const size_t n = std::min(g_n.load(std::memory_order_acquire), g_cap);
    constexpr size_t kBlock = 64 * 1024, kLineMax = 20 + 4 * 10 + 5;     // a 64-bit and four 32-bit numbers, each with its separator
    static char block[kBlock];
    char* p = block;
    for (; g_written < n; ++g_written) {
        if (p + kLineMax > block + kBlock) {
            std::fwrite(block, 1, static_cast<size_t>(p - block), g_file);
            p = block;
        }
        const Ev& e = g_ev[g_written];
        p = put_num(p, e.t0, ' ');
        p = put_num(p, e.dur, ' ');
        p = put_num(p, e.thread, ' ');
        p = put_num(p, e.zone, ' ');
        p = put_num(p, e.arg, '\n');
    }
    if (p != block) std::fwrite(block, 1, static_cast<size_t>(p - block), g_file);
    std::fflush(g_file);
}

void flush_locked() {
    flush_names_locked();
    flush_events();
}

void writer_main() {
    log_set_thread_name("ftrace-writer");
    while (!g_stop.load()) {
        for (int i = 0; i < 10 && !g_stop.load(); ++i) Sleep(100);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            flush_names_locked();
        }
        flush_events();
    }
}

}  // namespace

// TSC -> ns. The ratio comes from a (QPC, TSC) pair taken at tsc_init and is refreshed from the same anchor at most once a second, so
// it gets more exact the longer the run is. Plain doubles: written only under g_tsc_mu, read racily (a torn read is impossible for a
// double on x64 and a stale ratio is as good as the new one).
namespace {
std::mutex g_tsc_mu;
uint64_t g_tsc_q0 = 0, g_tsc_c0 = 0, g_tsc_last_q = 0;
double g_ns_per_tick = 0.0;
double q_to_ns(uint64_t dq) { return static_cast<double>(dq) * 1e9 / static_cast<double>(qpc::freq()); }
}  // namespace

void tsc_init() {
    std::lock_guard<std::mutex> lk(g_tsc_mu);
    if (g_ns_per_tick > 0) return;
    g_tsc_q0 = qpc::ticks();
    g_tsc_c0 = __rdtsc();
    Sleep(20);
    const uint64_t q1 = qpc::ticks(), c1 = __rdtsc();
    const double ns = q_to_ns(q1 - g_tsc_q0);
    g_ns_per_tick = c1 > g_tsc_c0 && ns > 0 ? ns / static_cast<double>(c1 - g_tsc_c0) : 0.3;
    g_tsc_last_q = q1;
}

uint64_t tsc_to_ns(uint64_t ticks) {
    if (g_ns_per_tick <= 0) tsc_init();
    const uint64_t q = qpc::ticks();
    if (q - g_tsc_last_q > qpc::freq()) {
        std::lock_guard<std::mutex> lk(g_tsc_mu);
        const uint64_t c = __rdtsc();
        const double ns = q_to_ns(q - g_tsc_q0);
        if (c > g_tsc_c0 && ns > 1e8) g_ns_per_tick = ns / static_cast<double>(c - g_tsc_c0);
        g_tsc_last_q = q;
    }
    return static_cast<uint64_t>(static_cast<double>(ticks) * g_ns_per_tick);
}

bool g_split_on = false;
uint64_t g_split_ticks[kSplBuckets] = {};
uint64_t g_split_cnt[kCntCounts] = {};

void split_init() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!config_get_bool("prof", "render_split", false)) return;
    tsc_init();
    g_split_on = true;
    NXLOG_I("prof", "render split on: one line per 10 s");
}

void split_frame(uint64_t presents) {
    if (!g_split_on) return;
    static uint64_t t_last = 0, p_last = 0, ticks_last[kSplBuckets] = {}, cnt_last[kCntCounts] = {};
    const uint64_t now = __rdtsc();
    if (!t_last) { t_last = now; p_last = presents; return; }
    const uint64_t el = tsc_to_ns(now - t_last);
    if (el < 10000000000ull) return;
    const uint64_t frames = presents > p_last ? presents - p_last : 0;
    if (frames == 0) { t_last = now; return; }
    const double f = static_cast<double>(frames);
    double ms[kSplBuckets];
    for (int i = 0; i < kSplBuckets; ++i) { ms[i] = static_cast<double>(tsc_to_ns(g_split_ticks[i] - ticks_last[i])) / 1e6 / f; ticks_last[i] = g_split_ticks[i]; }
    double c[kCntCounts];
    for (int i = 0; i < kCntCounts; ++i) { c[i] = static_cast<double>(g_split_cnt[i] - cnt_last[i]) / f; cnt_last[i] = g_split_cnt[i]; }
    const double interval = static_cast<double>(el) / 1e6 / f;
    const double busy = interval - ms[kSplIdle];
    NXLOG_I("prof", "render split, %llu frames, %.1f fps, ms/frame: interval %.2f, render busy %.2f, idle %.2f | replay %.2f (gpu_wait %.2f), present %.2f (end %.2f), submit %.2f, other %.2f | per frame: draws %.0f, cmds bind %.0f draw %.0f dyn %.0f barrier %.0f, passes %.1f, epoch bumps %.0f, set_reuse hit %.0f miss %.0f, submissions %.1f | bind_coalesce: calls skipped %.0f, merged away %.0f",
            static_cast<unsigned long long>(frames), 1000.0 / interval, interval, busy, ms[kSplIdle], ms[kSplReplay], ms[kSplGpuWait], ms[kSplPresent], ms[kSplPresentEnd], ms[kSplSubmit],
            busy - ms[kSplReplay] - ms[kSplPresent] - ms[kSplSubmit], c[kCntDraws], c[kCntCmdBind], c[kCntCmdDraw], c[kCntCmdDyn], c[kCntCmdBarrier], c[kCntPasses], c[kCntEpoch], c[kCntSetHit], c[kCntSetMiss], c[kCntSubmissions], c[kCntSkipped], c[kCntMerged]);
    const double pl = c[kCntPoolHit] + c[kCntPoolMiss], sets = c[kCntSetHit] + c[kCntSetMiss];
    NXLOG_I("prof", "render split resolve, per frame: texture resolves %.0f (%.2f per draw) + %.0f from the draw's array | pool_lookup %.0f, cache hit %.1f %% | set_reuse hit %.1f %% | epoch bumps: gpu_wrote %.0f, end_pass %.0f, begin pass %.0f, feedback %.0f, vol3d %.0f, targets %.0f, submission %.1f, skipped end_pass %.0f targets %.0f",
            c[kCntResolve], c[kCntDraws] > 0 ? c[kCntResolve] / c[kCntDraws] : 0.0, c[kCntResolveHit], pl, pl > 0 ? 100.0 * c[kCntPoolHit] / pl : 0.0,
            sets > 0 ? 100.0 * c[kCntSetHit] / sets : 0.0, c[kCntEpGpuWrote], c[kCntEpEndPass], c[kCntEpBeginPass], c[kCntEpFeedback], c[kCntEpVol3d], c[kCntEpTargets],
            c[kCntEpSubmit], c[kCntEpSkipEndPass], c[kCntEpSkipTargets]);
    NXLOG_I("prof", "render split setup, ms/frame %.3f (texpoll + mirror + outend) | per frame: tex polls %.1f (empty %.1f, pages %.1f), absorb_writes %.1f (empty %.1f, pages %.1f = %.0f KiB), write-watch words read %.0f",
            ms[kSplSetup], c[kCntTexPoll], c[kCntTexPollEmpty], c[kCntTexPages], c[kCntAbsorb], c[kCntAbsorbEmpty], c[kCntAbsorbPages], c[kCntAbsorbPages] * 4.0, c[kCntWwWords]);
    t_last = now;
    p_last = presents;
}

uint64_t now_ns() { return qpc::now_ns(); }

uint16_t zone_id(const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (size_t i = 0; i < g_zone_names.size(); ++i)
        if (g_zone_names[i] == name) return static_cast<uint16_t>(i);
    g_zone_names.emplace_back(name);
    return static_cast<uint16_t>(g_zone_names.size() - 1);
}

void event(uint16_t zone, uint64_t t0, uint64_t t1, uint32_t arg) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const size_t i = g_n.fetch_add(1, std::memory_order_relaxed);
    if (i >= g_cap) return;
    const int ti = thread_index();
    if (!g_thread_named[ti].load(std::memory_order_relaxed)) {
        const char* n = log_thread_name();
        g_thread_names[ti] = n ? n : "";
        g_thread_named[ti].store(true, std::memory_order_relaxed);
    }
    const uint64_t d = t1 > t0 ? t1 - t0 : 0;
    g_ev[i] = {t0, d > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(d), zone, static_cast<uint16_t>(ti), arg};
}

void mark(const char* name, uint32_t arg) {
    if (!on()) return;
    const uint16_t z = zone_id(name);
    const uint64_t t = now_ns();
    event(z, t, t, arg);
}

namespace {
std::atomic<uint64_t> g_last_frame{0};
std::atomic<uint32_t> g_stall_ms{0};
std::thread g_sampler;

// [prof] stall_ms = N: when no frame was presented for N ms, dump every thread's stack to <trace>.stacks (up to 4 times per stall).
struct CpuSnap {
    std::vector<ThreadRecord> th;
    std::vector<uint64_t> cyc;
};

CpuSnap take_snap() {
    CpuSnap s;
    std::vector<ThreadRecord> recs = diag_threads_dup();   // private handles: an unregistering thread cannot close ours mid-query
    for (const ThreadRecord& r : recs) {
        ULONG64 c = 0;
        if (r.host_handle) QueryThreadCycleTime(static_cast<HANDLE>(r.host_handle), &c);
        s.cyc.push_back(c);
    }
    s.th = recs;
    for (ThreadRecord& r : s.th) r.host_handle = nullptr;   // the snapshot keeps tid/name only
    diag_threads_release(recs);
    return s;
}

double tsc_per_ns() {
    const uint64_t qa = qpc::ticks();
    const unsigned long long c0 = __rdtsc();
    Sleep(60);
    const uint64_t qb = qpc::ticks();
    const unsigned long long c1 = __rdtsc();
    const double ns = static_cast<double>(qb - qa) * 1e9 / static_cast<double>(qpc::freq());
    return ns > 0 ? static_cast<double>(c1 - c0) / ns : 3.0;
}

void sampler_main(std::string path) {
    log_set_thread_name("ftrace-stall");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const double tpn = tsc_per_ns();
    uint64_t handled_frame = 0, prev_frame = 0;
    int dumps = 0;
    CpuSnap snap = take_snap();               // taken just after a frame was seen
    while (!g_stop.load()) {
        Sleep(3);
        const uint64_t last = g_last_frame.load();
        if (!last) continue;
        if (last != handled_frame) {
            // a new frame: if the gap before it was a stall, say what every thread's CPU time was during it
            if (dumps > 0) {
                const CpuSnap now = take_snap();
                std::fprintf(f, "--- stall ended: %.1f ms between frames; CPU ms per thread in that time:\n", (last - prev_frame) / 1e6);
                std::vector<std::pair<double, std::string>> v;
                for (size_t i = 0; i < now.th.size(); ++i)
                    for (size_t j = 0; j < snap.th.size(); ++j)
                        if (now.th[i].host_tid == snap.th[j].host_tid && now.cyc[i] >= snap.cyc[j]) {
                            v.emplace_back(static_cast<double>(now.cyc[i] - snap.cyc[j]) / tpn / 1e6, now.th[i].name);
                            break;
                        }
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
                for (size_t i = 0; i < v.size() && i < 6; ++i) std::fprintf(f, "    cpu %-12s %7.1f ms\n", v[i].second.c_str(), v[i].first);
                std::fflush(f);
            }
            prev_frame = last;
            handled_frame = last;
            dumps = 0;
            snap = take_snap();
        }
        const uint64_t idle = now_ns() - last;
        const uint64_t want = static_cast<uint64_t>(g_stall_ms.load()) * 1000000ull + static_cast<uint64_t>(dumps) * 12000000ull;
        if (dumps < 4 && idle >= want) {
            const uint64_t t = now_ns();
            const std::string d = diag_thread_dump();
            std::fprintf(f, "=== stall sample %d at t=%llu ns, %.1f ms since the last frame\n%s\n", dumps, static_cast<unsigned long long>(t), idle / 1e6, d.c_str());
            std::fflush(f);
            ++dumps;
        }
    }
    std::fclose(f);
}
}  // namespace

void frame() {
    if (!on()) return;
    static const uint16_t z = zone_id("frame");
    const uint64_t t = now_ns();
    event(z, t, t, 0);
    g_last_frame.store(t);
}

void start_from_config() {
    split_init();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_started) return;
    const std::string path = config_get_str("prof", "trace", "");
    if (path.empty()) return;
    g_cap = static_cast<size_t>(config_get_int("prof", "trace_events", 6000000));
    g_ev = static_cast<Ev*>(VirtualAlloc(nullptr, g_cap * sizeof(Ev), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_ev) return;
    g_file = std::fopen(path.c_str(), "wb");
    if (!g_file) { VirtualFree(g_ev, 0, MEM_RELEASE); g_ev = nullptr; return; }
    g_started = true;
    tsc_init();
    g_on.store(true);
    g_writer = std::thread(writer_main);
    if (const int64_t sm = config_get_int("prof", "stall_ms", 0); sm > 0) {
        g_stall_ms.store(static_cast<uint32_t>(sm));
        g_sampler = std::thread(sampler_main, path + ".stacks");
    }
    NXLOG_I("prof", "frame trace on: %s (%zu events)", path.c_str(), g_cap);
}

void stop() {
    if (!g_started) return;
    g_on.store(false);
    g_stop.store(true);
    if (g_writer.joinable()) g_writer.join();
    if (g_sampler.joinable()) g_sampler.join();
    std::lock_guard<std::mutex> lk(g_mu);
    flush_locked();
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

}  // namespace nxrt::ft
