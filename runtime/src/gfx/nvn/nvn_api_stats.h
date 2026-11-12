// SPDX-License-Identifier: GPL-3.0-or-later
// [prof] render_split, the game-thread side of the NVN API: how long the game's threads spend inside our NVN entry points and how many of
// a frame's calls do work. One `nvn api` log line per 10 s (written at the game's present): per frame, for each thread that called NVN,
// its calls and the ms spent inside them (TSC pair in the thunk, nvn_abi.h; the dispatch into the thunk is not included), then the
// counters below. With the split off every hook is one predictable branch on a bool that is set once at start.
#pragma once
#include <atomic>
#include <cstdint>

#include "nxrt/ftrace.h"

namespace nxrt::nvn::api_stats {

inline bool on() { return ft::g_split_on; }

// Per thread: written by the owning thread only (relaxed load + store, no locked instruction), read by the logging thread.
struct ThreadStats {
    std::atomic<uint64_t> calls{0}, ticks{0};
    char name[32] = {};
};
ThreadStats* thread_stats();      // this thread's row, made on its first call (rows are kept for the whole run)
inline void note_call(uint64_t ticks) {
    ThreadStats* s = thread_stats();
    s->calls.store(s->calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    s->ticks.store(s->ticks.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
}

enum Count : int {
    kTexReg, kTexRegSame,         // nvnTexturePoolRegisterTexture/Image calls; those that found the same texture and view in the slot
    kSmpReg, kSmpRegSame,         // nvnSamplerPoolRegisterSampler(Builder) calls; those that found the same sampler in the slot
    kFlush, kFlushHit, kFlushScan,// the game's FlushMappedRange calls (pool and buffer); textures every textures_cpu_wrote marked written and
                                  // the registry entries it looked at (storage map and sparse textures), from those flushes and from the
                                  // render thread's callers (write watch, texture patches)
    kLists, kRecords, kRecordBytes, kCopyBytes,   // lists ended (EndRecording), their records and bytes; bytes copied by CopyCommands
    kCounts
};
extern std::atomic<uint64_t> g_count[kCounts];
inline void count(Count c, uint64_t n = 1) {
    if (on()) g_count[c].fetch_add(n, std::memory_order_relaxed);
}

void frame();                     // the game presented (nvnQueuePresentTexture): logs the line every 10 s

}  // namespace nxrt::nvn::api_stats
