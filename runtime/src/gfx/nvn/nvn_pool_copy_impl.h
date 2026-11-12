// SPDX-License-Identifier: GPL-3.0-or-later
// What the files of [nvn] pool_copy share with each other: nvn_pool_copy.cpp (the shadows and the copy into them), nvn_pool_land.cpp
// (GPU writes going back into guest memory) and nvn_pool_stock.cpp (the shadows' buffers). Everybody else uses the interface in
// nvn_gpu_internal.h.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu::pcopy {

constexpr const char* kTag = "nvn.pool";
constexpr uint64_t kPage = 4096;

// statistics both files add to (the summary line, gauges, hitch counters)
struct Shared {
    std::atomic<uint64_t> landed{0};       // bytes copied from shadows into guest memory
    std::atomic<uint64_t> records{0};      // GPU writes recorded
    std::atomic<uint64_t> unreadable{0};   // pages a copy could not read or write (decommitted under it)
};
Shared& shared();

// [d, d + n) <- [s, s + n); on a fault, page by page of `s` (in) or `d` (out): a part that cannot be copied is zero-filled when it is the
// shadow side (in) or left alone (out). Returns the parts that could not be copied.
uint64_t guarded_copy(uint8_t* d, const uint8_t* s, uint64_t n, bool in);
// True when [a, a + n) and [b, b + n) hold the same bytes; false when they differ or one of them cannot be read.
bool guarded_same(const uint8_t* a, const uint8_t* b, uint64_t n);

// ---- nvn_pool_land.cpp ----------------------------------------------------------------------------------------------------------
void land_start();                 // the landing thread, once a copy pool exists
void land_stop();                  // pcopy_shutdown: the thread stops and every record goes
// Render thread: submission `serial` may write pool bytes [off, off + size) of pg through chunk c; they land when it completes. False:
// a record of this submission holds the range already.
bool land_record(PoolGpu* pg, PoolChunk* c, uint64_t off, uint64_t size, uint64_t serial);
void land_forget(PoolChunk& c);    // the chunk is destroyed: its records go, and no landing reads its shadow after this returns
size_t land_pending();             // records waiting for their serial
// Pool byte ranges in [lo, hi) of pg that a GPU write recorded and not landed yet may hold (the queue and the batch being landed), clipped to
// [lo, hi), appended to out unsorted. The shadow's bytes there are newer than guest memory until they land.
void land_busy(const PoolGpu* pg, uint64_t lo, uint64_t hi, std::vector<std::pair<uint64_t, uint64_t>>& out);
bool land_keep();                  // [nvn] pool_copy_land_keep: a landing leaves the pages it copies into current (read once)
// No landing runs while one of these lives (the write tracker of a pool with records is stopped under it).
class LandingPause {
public:
    LandingPause();
    ~LandingPause();
    LandingPause(const LandingPause&) = delete;
    LandingPause& operator=(const LandingPause&) = delete;
};

// ---- nvn_pool_stock.cpp ---------------------------------------------------------------------------------------------------------
void stock_start(uint64_t cell, uint64_t cap);     // a copy pool exists: spares are kept, and made ahead of use ([nvn] pool_copy_spare_mb)
void stock_stop();                                 // pcopy_shutdown: the thread stops, the spares go, later gives destroy
// A buffer for a shadow of `size` bytes: a spare of that size, else a new one made here. False: none could be made.
bool stock_get(uint64_t size, gfx::Buffer* out);
// A dropped shadow's buffer: kept as a spare or destroyed. want: the size an eviction is making room for (0: no eviction); only a buffer
// of that size is kept then.
void stock_give(gfx::Buffer& b, uint64_t want);
// True when stock_get(size) would have to make a buffer that takes the shadows' memory (in use and spare) over `cap`. Spares of other
// sizes are destroyed first to make room.
bool stock_over(uint64_t size, uint64_t cap);
struct StockStats {
    uint64_t made_here = 0;      // buffers made at a bind (on the render thread)
    double made_here_ms = 0;     // ... and the time that took
    uint64_t made_ahead = 0;     // buffers the stock thread made
    uint64_t reused = 0;         // shadows that got a spare
    uint64_t spare_bytes = 0;
};
StockStats stock_stats();

}  // namespace nxrt::nvn::be::gpu::pcopy
