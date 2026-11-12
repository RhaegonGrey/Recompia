// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation timing ([latency] present_stats) and display-locked pacing ([latency] vblank_lock).
//
// One record per game frame (one per present call of the game), filled by the threads the frame crosses:
//
//   main thread     present_timing_release()          (b) the frame-boundary limiter released the frame (lowlat.cpp, pace_boundary_done)
//   game thread     present_timing_game_present()     (a) the game called present; the record takes the release (b) that came before it
//   render thread   present_timing_render_begin()     (c) the replay of the frame is done and its present starts; present_timing_present_release()
//                                                         is (b) instead when the limiter paced this present in the present call
//   submit thread   present_timing_queue_present()    (d) vkQueuePresentKHR called, and how long it blocked; how long the present was held
//   probe watcher   present_timing_gpu_done()         (e) the GPU finished the frame (a timeline value the present's submission signals)
//   present waiter  present_timing_shown()            (f) vkWaitForPresentKHR returned for the frame's present id: the image is on the screen
//
// (f) needs VK_KHR_present_wait (enabled at device creation only when one of the two keys is on, present_wait_wanted()). Under Proton and
// gamescope it completes when the compositor reports the frame presented, so (f) is the vblank the image went out on plus that report's
// delay. Without the extension the records end at (d), and the frame intervals are those of the vkQueuePresentKHR calls, not of the screen.
//
// Every 10 s with present_stats on, two log lines (tag "pacing"): how many frames were shown for 1, 2, 3, 4+ refreshes, the boundary waits
// per game frame, the stage latencies, the phase of the limiter's release against the vblank and the margin of each frame's GPU completion
// against the vblank it was due on.
//
// vblank_lock (on a fixed-refresh panel with vsync on and a frame cap of refresh / k, k >= 2, FIFO, with VK_KHR_present_wait):
// - the present of frame i is held until just after the vblank before its slot (the vblank frame i-1 was shown on + k - 1 refreshes), so an
//   early frame no longer reaches the screen a refresh before its slot (shown 1 refresh, then 3 for the one before it); a late frame costs one
//   refresh once and the next frames keep k refreshes. The hold sleeps on the submit thread after the frame's submission went to the GPU
//   (submissions queued behind it wait too: in the steady state the next frame's first ones come after the hold has ended);
// - the limiter's refresh grid is moved (FrameLimiter::shift_grid) so release -> shown stays at a target that a share of the frames
//   ([latency] vblank_lock_late, 1 %) is allowed to miss: the target rises with each late frame and sinks slowly with each on-time one.
//   The phase is held with a filtered, dead-banded step; the refresh a late frame or a hitch adds to every frame after it (the hold keeps
//   the spacing) is walked off within a few frames. Without the shift the hold would add a refresh of latency after every late frame.
#pragma once
#include <cstdint>

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::extras {

bool present_timing_on();           // present_stats or vblank_lock: the records are kept
bool present_wait_wanted();         // the device enables VK_KHR_present_wait (when offered) for the measurement
void present_timing_set_limiter(FrameLimiter* lim);       // the limiter that paces the frames (its refresh grid); null = none
void present_timing_set_waiter(bool runs);                // renderer: the present waiter runs (else the records end at the queue)

uint64_t present_timing_game_present(uint32_t boundary_waits);   // game thread; boundary_waits = pace_note_game_present(); returns the frame (0: off)
void present_timing_release(uint64_t t_ns);                    // main thread: the boundary wait released the next frame
void present_timing_render_begin(uint64_t frame);               // render thread, before the frame's present (0: a repeated frame, not recorded)
uint64_t present_timing_render_frame();                         // render thread: the frame being presented (0: none)
void present_timing_present_release(uint64_t t_ns);             // render thread: the limiter in the present call released the current frame
// Submit thread: the frame's vkQueuePresentKHR ran from t0 to t1 after being held for held_ns ([latency] vblank_lock).
void present_timing_queue_present(uint64_t frame, uint64_t held_ns, uint64_t t0, uint64_t t1);
void present_timing_gpu_done(uint64_t frame, uint64_t t_ns);
void present_timing_shown(uint64_t frame, uint64_t t_ns);
// Submit thread, before vkQueuePresentKHR: the time to hold the frame's present until (0: present now). fifo: the swapchain is FIFO.
uint64_t present_timing_hold_until(uint64_t frame, bool fifo);

}  // namespace nxrt::extras
