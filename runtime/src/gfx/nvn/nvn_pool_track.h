// SPDX-License-Identifier: GPL-3.0-or-later
// Which pages of a copy pool ([nvn] pool_copy, nvn_pool_copy.cpp) the CPU wrote since a shadow last copied them, so a bind copies a page
// again only after a write. Pages are made read-only when they are copied (nxrt/write_watch.h); the first write faults once and the page
// counts as written. A page written again soon after every copy (a uniform ring the game refills each frame, a range the GPU's results
// land in) is "hot": it is left writable and copied at every use for a while, instead of faulting every frame.
//
// A page has a generation, raised each time it is armed again after a write. A shadow that stores the generation it copied is current
// while the page is still armed and the generation is the same; two shadows over one page (a cell and a span) each keep their own.
// All calls: the render thread, except create / destroy / stop (the thread that makes or destroys the pool; no bind of it runs then)
// and the ptrack_land_* calls (the landing, nvn_pool_land.cpp: under its mutex, which stop holds too).
#pragma once
#include <cstdint>

namespace nxrt::nvn::be::gpu {

struct PoolTrack;

// [nvn] pool_copy_track = true (default) | false: read once.
bool pool_track_on();
// A tracker over [base, base + size) (page aligned). Null: the range cannot be tracked (outside the heap's primary view), or tracking is off.
PoolTrack* ptrack_create(const uint8_t* base, uint64_t size);
void ptrack_destroy(PoolTrack* t);
// Every page becomes writable again and none is tracked any more (a cell of the pool is imported instead of copied: an import must not
// see read-only pages).
void ptrack_stop(PoolTrack* t);
// True when a copy of pool page `page` that stored generation `copied` still holds what guest memory holds.
bool ptrack_current(const PoolTrack* t, uint64_t page, uint32_t copied);
// Pool pages [first, first + count) are about to be copied (call before reading them): those the CPU wrote are armed again under a new
// generation, hot ones are left writable. gens[i] receives the generation to store with the copy of page first + i; 0: copy it at every
// use (hot, or it cannot be armed). `frame`: the present count.
void ptrack_before_copy(PoolTrack* t, uint64_t first, uint64_t count, uint32_t frame, uint32_t* gens);

// ---- GPU writes ([nvn] pool_copy_land_keep) -------------------------------------------------------------------------------------
// The GPU writes a page through one shadow, and the landing copies those bytes into guest memory. That copy is not a CPU write: a page
// that was current before it is current after it, in the shadow the write went through, so no bind fetches the same bytes back.
// Render thread, when the write is recorded: the page gets a new generation, which *gen (what that shadow stored for the page) follows
// when the shadow was current. Any other shadow over the page stops being current: it does not get the write.
void ptrack_gpu_write(PoolTrack* t, uint64_t page, uint32_t* gen);
// The landing, before it copies into pages [first, first + count): how many pages from `first` on are armed and unwritten (0: the first
// is not). They become writable and stay current when ptrack_land_end follows the copy; pages not in such a run are written the
// ordinary way (mem::ww::before_os_write) and count as written.
uint64_t ptrack_land_begin(PoolTrack* t, uint64_t first, uint64_t count);
// The copy into a run of ptrack_land_begin is done: its pages are armed again. False: they could not be, and count as written.
bool ptrack_land_end(PoolTrack* t, uint64_t first, uint64_t count);
// The landing found the CPU's bytes of a page of such a run different from the shadow's (a CPU write between begin and end): written.
void ptrack_land_differs(PoolTrack* t, uint64_t first, uint64_t count);

struct PoolTrackStats {
    uint64_t arms = 0;          // arm calls
    uint64_t armed_pages = 0;   // pages armed by them
    uint64_t rewrites = 0;      // copied pages found written again
    uint64_t hot_now = 0;       // pages currently hot
    uint64_t unarmable = 0;     // pages that could not be armed (copied at every use)
    uint64_t land_kept = 0;     // pages a landing left current
    uint64_t land_differ = 0;   // pages a landing found written while it copied
};
PoolTrackStats ptrack_stats();

}  // namespace nxrt::nvn::be::gpu
