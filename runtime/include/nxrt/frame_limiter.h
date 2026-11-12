// SPDX-License-Identifier: GPL-3.0-or-later
// Frame limiter for the low-latency design: begin_frame() waits for the frame's scheduled start BEFORE the caller samples input,
// so input is read as late as possible and the display queue stays short. The wait is a high-resolution waitable timer for
// the bulk and a spin on the performance counter for the last stretch. Schedules are absolute (next = previous + period), so
// the average rate does not drift for small hiccups; a frame that starts late is made up for by at most 10 % of a period (so
// the next interval is never shorter than 0.9 period: no catch-up bursts) and after a stall of more than 4 periods the
// schedule restarts from now.
// On a fixed-refresh display with vsync (set_refresh_sync) a pace of refresh / k runs on the refresh grid instead: frame starts sit on
// anchor + n refresh intervals, k intervals apart; a late frame takes the next refresh and the anchor never moves.
#pragma once
#include <cstdint>

namespace nxrt {

class FrameLimiter {
public:
    FrameLimiter();
    ~FrameLimiter();
    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    // 0 = unlimited (begin_frame never waits; it still records frame times). Safe to call from any thread.
    void set_target_fps(double fps);
    double target_fps() const;

    // The game's own ceiling (0 = none): frames never start closer together than 1 / max, whatever the target (unlimited included). Pokopia sets
    // it to 60 while an area transition plays (#144: a part of it breaks on frames shorter than 1/60 s). Loading frames (frame_limiter_unpaced)
    // stay unpaced. With RefreshSync::snap_ceiling the ceiling in force is the next divisor of the refresh rate at or under it.
    // Safe to call from any thread.
    void set_max_fps(double fps);
    double max_fps() const;
    double effective_fps() const;       // what begin_frame paces to: the target, lowered to the ceiling (0 = no pacing)

    // The display shows whole frames at a fixed refresh rate (vsync on a panel without VRR). hz = 0 (the default) turns all of it off.
    struct RefreshSync {
        double hz = 0;                  // the display's refresh rate
        // Refresh grid: while the pace is hz / k (k >= 2, within 0.5 %), frame starts sit on anchor + n / hz, k grid points apart. A frame
        // that comes more than 0.1 refresh interval after its point starts on the first point not yet past, so the display shows it one
        // refresh longer and the frames after it keep their spacing and their phase against the display; the free schedule starts it at
        // once and moves the whole schedule to an arbitrary phase. The anchor is the start of the first grid frame (the limiter is not
        // told when the display refreshes); it holds until reset_schedule() or another hz. A pace that is not hz / k, and k = 1, stay
        // on the free schedule.
        bool grid = false;
        // The game's ceiling (set_max_fps), while it is what lowers the pace, is itself lowered to a divisor of the refresh rate
        // (snap_frame_cap: 60 on 90 Hz -> 45; 60 on 60 Hz stays 60): a ceiling between two divisors shows frames for uneven numbers of
        // refreshes, exactly what the snapped frame cap avoids. Never raised, so the ceiling's promise holds.
        bool snap_ceiling = false;
    };
    void set_refresh_sync(const RefreshSync& sync);     // safe to call from any thread
    double refresh_hz() const;          // RefreshSync::hz (0: no fixed-refresh display)
    int grid_refreshes() const;         // refresh intervals per frame of the last frame paced on the refresh grid; 0 = free schedule or unpaced
    // Moves the refresh grid's anchor by ns (positive: later), so later frame starts keep their spacing at another phase against the display.
    // For a caller that measures when frames reach the screen (present_timing.h, [latency] vblank_lock); a no-op without an anchor. Any thread.
    void shift_grid(double ns);

    // A function that runs at the very start of every begin_frame(), on the thread that calls it (the presenting thread). Play mode uses it to
    // apply present-mode changes at a point where nothing else touches the swapchain. nullptr removes it. Safe to call from any thread.
    using PreFrameFn = void (*)(void* user);
    void set_pre_frame_hook(PreFrameFn fn, void* user);

    // Call at the top of every frame, before polling input. Blocks until the scheduled start, returns the frame start time
    // in nanoseconds of the monotonic clock now_ns().
    uint64_t begin_frame();

    // The same wait without the pre-frame hook, for a limiter that runs on the thread that samples input (the game's main thread) while the hook
    // still has to run on the presenting thread: that thread calls run_pre_frame_hook() itself (S18).
    uint64_t begin_frame_nohook();
    void run_pre_frame_hook();

    // Restart the schedule (after a pause, a minimise, a mode change).
    void reset_schedule();

    struct Stats {
        uint64_t frames = 0;            // frame intervals measured (begin_frame calls minus one)
        double mean_ms = 0;             // mean interval between frame starts
        double min_ms = 0, max_ms = 0;
        double p99_ms = 0;              // 99th percentile of the intervals
        double stddev_ms = 0;
        double fps = 0;                 // 1000 / mean_ms
        // Limiter accuracy, separate from what the frame body costs: frames whose body finished before the scheduled start had to
        // wait ("waited"); for those, how late the start came against the schedule. A frame whose body overran the period
        // ("overran") starts at once and is not the limiter's doing.
        uint64_t waited = 0, overran = 0;
        double overran_fraction = 0;    // overran / (waited + overran)
        double late_p99_ms = 0, late_max_ms = 0;   // lateness of waited frames
        double late_over_half_ms = 0;   // fraction of the waited frames that started more than 0.5 ms late (a robust accuracy measure with few frames, unlike p99)
    };
    Stats stats() const;                // over the intervals since reset_stats() (at most the last 8192)
    void reset_stats();

    // Last wait: how long begin_frame() slept/spun (ns), and how late the frame started against its schedule (ns, >= 0).
    uint64_t last_wait_ns() const;
    uint64_t last_late_ns() const;

    static uint64_t now_ns();           // QueryPerformanceCounter in nanoseconds
    // The limiter's wait (high-resolution timer for the bulk, spin for the last stretch) for any thread: returns once now_ns() >= target_ns.
    static void sleep_until(uint64_t target_ns);

private:
    struct Impl;
    Impl* impl_;
};

// For a VRR (G-Sync / FreeSync) display: a cap a little under the refresh rate keeps the frame rate inside the VRR window so that
// the display never falls back to plain vsync or tearing. refresh - max(3, refresh^2 / 3600) (Blur Busters formula, at least 3
// under): 120 -> 116, 144 -> 138.24, 240 -> 224. Below 100 Hz it is the refresh rate itself (60 -> 60, 90 -> 90): there 3 fps is
// a felt loss, a screen without VRR (Steam Deck) shows the lower rate unevenly, and a game that counts a frame within 1.5 ms of
// 1/60 s as exactly 1/60 s (Pokopia) runs 5 % slow at 57.
double suggest_vrr_cap(double refresh_hz);

// Frame cap snapped to a divisor of the refresh rate (F5): with vsync on, a cap that is not a divisor shows frames for uneven numbers of
// refreshes (40 on 90 Hz: 2, 2, 3, 2, 2, 3 ... refreshes). k = ceil(hz / (cap * 1.005)) (0.5 % tolerance for reported rates such as 79.98),
// cap' = hz / k, never above the requested cap beyond the tolerance. 90 Hz: 40 -> 30, 45 -> 45, 30 -> 30; 80 Hz: 40 -> 40; 60 Hz: 40 -> 30,
// 45 -> 30; 40 Hz: 40 -> 40 (k = 1: no snap). Only for 0 < cap < hz with hz > 0; otherwise (or when k would be 1) k = 0 and cap is returned.
struct FrameSnap {
    double cap = 0;     // the cap to run at (hz / k when snapped, else the requested cap)
    int k = 0;          // refresh intervals per frame when snapped (>= 2), else 0
};
FrameSnap snap_frame_cap(double cap, double hz);

// While the game loads (a black boot screen, a loading screen: a presented frame with almost no draws) no limiter waits: the loading code
// advances once per frame, so pacing it at 60 Hz stretches boot and loads. The NVN replay sets it ([display] unpaced_loading, default off).
void frame_limiter_set_unpaced(bool on);
bool frame_limiter_unpaced();   // true only while loading AND no frame limit of the player's own holds
// A frame limit the player set ([display] fps_limit = a number) holds through loading screens too: on a Mac a 60 limit showed 110 fps for
// the first 16 s of every start, because boot loading is unpaced. auto and unlimited keep the faster unpaced loading. Set by play mode.
void frame_limiter_set_player_cap(bool set);
}  // namespace nxrt
