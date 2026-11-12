// SPDX-License-Identifier: GPL-3.0-or-later
// Frame trace: a tiny zone recorder for frame-time work. Off by default (one relaxed load per zone). With [prof] trace = <file>
// (env NXRT_PROF_TRACE) every thread appends {start, duration, zone} events to a preallocated array and a writer thread flushes
// them to a text file once a second, so a run that ends by TerminateProcess loses nothing. tools/prof/ftrace_report.py turns the
// file into frame-time percentiles, stall lists and a Chrome trace.
//
//   NXFT("replay");                       // scope; recorded when it ends
//   NXFT_MIN("svc", 50000);               // scope; recorded only when it took at least 50 us
//   nxrt::ft::mark("frame");              // instant (zero duration)
#pragma once
#include <atomic>
#include <cstdint>
#if defined(_MSC_VER) || !defined(_WIN32)
#include <intrin.h>   // __rdtsc: MSVC on Windows, compat/intrin.h on macOS
#endif

namespace nxrt::ft {

extern std::atomic<bool> g_on;
inline bool on() { return g_on.load(std::memory_order_relaxed); }

uint64_t now_ns();                         // monotonic, same base as FrameLimiter::now_ns
uint16_t zone_id(const char* name);        // registers a name once (static local at each site)
void event(uint16_t zone, uint64_t t0, uint64_t t1, uint32_t arg = 0);
void mark(const char* name, uint32_t arg = 0);
void frame();                              // the "frame" mark (one per presented frame); also feeds the stall sampler
void start_from_config();                  // reads [prof] trace / trace_events; a no-op when unset or already started
void stop();                               // final flush

// Cheap stamps for hot paths (per-draw scopes, the render-thread split): the TSC, converted to time only when reported. tsc_to_ns
// uses a ratio calibrated against QueryPerformanceCounter at tsc_init() and refined as the run goes on. Durations only.
inline uint64_t tsc() { return __rdtsc(); }
void tsc_init();                           // first calibration (about 20 ms); called by start_from_config and split_init
uint64_t tsc_to_ns(uint64_t ticks);

// [prof] render_split: always-on, near-free render-thread time split and per-frame counters, one log line per 10 s. Buckets are TSC
// deltas of existing top-level scopes. Counters are bumped on the render thread only (when the split is on).
// kSplSetup: per replay call setup (tex_poll_cpu_writes + mirror_begin_submit + out_end_submit), inside kSplReplay.
enum SplitBucket { kSplReplay, kSplPresent, kSplPresentEnd, kSplSubmit, kSplGpuWait, kSplIdle, kSplSetup, kSplBuckets };
enum SplitCount { kCntDraws, kCntCmdBind, kCntCmdDraw, kCntCmdDyn, kCntCmdBarrier, kCntPasses, kCntEpoch, kCntSetHit, kCntSetMiss, kCntSubmissions, kCntSkipped, kCntMerged,
                  // [nvn] resolve_once / pool_lookup cache / epoch_exact (second render split line): texture resolutions done and served
                  // from the per-draw array, pool_lookup last-hit cache hits and misses (every thread, approximate), epoch bumps by source
                  // and the bumps epoch_exact skipped
                  kCntResolve, kCntResolveHit, kCntPoolHit, kCntPoolMiss, kCntEpGpuWrote, kCntEpEndPass, kCntEpBeginPass, kCntEpFeedback,
                  kCntEpVol3d, kCntEpTargets, kCntEpSubmit, kCntEpSkipEndPass, kCntEpSkipTargets, 
                  // write-watch polls at the start of a replay call: texture region polls (and those that found no page), its pages,
                  // mirror absorb_writes calls (and those that found none), their pages, bitmap words read by take_written
                  kCntTexPoll, kCntTexPollEmpty, kCntTexPages, kCntAbsorb, kCntAbsorbEmpty, kCntAbsorbPages, kCntWwWords, kCntCounts };
extern bool g_split_on;
extern uint64_t g_split_ticks[kSplBuckets];
extern uint64_t g_split_cnt[kCntCounts];
void split_init();                         // reads [prof] render_split
void split_frame(uint64_t presents);       // render thread, once per present: logs the line every 10 s
inline void split_count(SplitCount c, uint64_t n = 1) { if (g_split_on) g_split_cnt[c] += n; }
inline void split_add(SplitCount c, uint64_t n) { if (g_split_on) g_split_cnt[c] += n; }
// Adds the scope's TSC time to a bucket. Render thread only for the buckets of that thread.
struct SplitScope {
    int b;
    uint64_t t0;
    explicit SplitScope(int bucket) : b(bucket), t0(g_split_on && bucket < kSplBuckets ? tsc() : 0) {}
    ~SplitScope() { if (t0) g_split_ticks[b] += tsc() - t0; }
};

struct Scope {
    uint16_t id;
    uint32_t arg;
    uint64_t min_ns;
    uint64_t t0;
    explicit Scope(uint16_t i, uint64_t min = 0, uint32_t a = 0) : id(i), arg(a), min_ns(min), t0(on() ? now_ns() : 0) {}
    void set_arg(uint32_t a) { arg = a; }
    ~Scope() {
        if (t0) {
            const uint64_t t1 = now_ns();
            if (t1 - t0 >= min_ns) event(id, t0, t1, arg);
        }
    }
};

}  // namespace nxrt::ft

#define NXFT_CAT2(a, b) a##b
#define NXFT_CAT(a, b) NXFT_CAT2(a, b)
#define NXFT(name) static const uint16_t NXFT_CAT(nxft_id_, __LINE__) = ::nxrt::ft::zone_id(name); ::nxrt::ft::Scope NXFT_CAT(nxft_s_, __LINE__)(NXFT_CAT(nxft_id_, __LINE__))
#define NXFT_MIN(name, min_ns) static const uint16_t NXFT_CAT(nxft_id_, __LINE__) = ::nxrt::ft::zone_id(name); ::nxrt::ft::Scope NXFT_CAT(nxft_s_, __LINE__)(NXFT_CAT(nxft_id_, __LINE__), (min_ns))
