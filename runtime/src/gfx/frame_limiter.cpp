// SPDX-License-Identifier: GPL-3.0-or-later
// FrameLimiter: schedule on QueryPerformanceCounter; coarse wait on a high-resolution waitable timer; spin for the last
// stretch. The behaviour and the thread rules are the contract in include/nxrt/frame_limiter.h.
// Two schedules: the free one (next = previous + period, a late frame carried a little) and the refresh grid (RefreshSync::grid: frame
// starts on anchor + n refresh intervals, a late frame on the next refresh).
#include "nxrt/frame_limiter.h"

#include "nxrt/log.h"
#include "nxrt/qpc.h"

#include <windows.h>
#include <emmintrin.h>  // _mm_pause

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nxrt {

namespace {
constexpr double kMinMarginNs = 300000.0;   // 0.3 ms: the spin always covers at least this much
constexpr double kMaxMarginNs = 3000000.0;  // 3 ms: the spin never covers more than this
constexpr double kMaxCarryPeriods = 0.1;    // a late frame is made up for by at most this fraction of a period (no bursts)
constexpr double kResyncPeriods = 4.0;      // a schedule more than this many periods behind restarts from now
constexpr std::size_t kRing = 8192;         // frame intervals kept for stats()
constexpr double kGridLateRefreshes = 0.1;  // refresh grid: a frame up to this fraction of a refresh interval late still starts in its slot, at once
constexpr double kDivisorTolerance = 0.005; // a pace within 0.5 % of hz / k counts as hz / k (the tolerance of snap_frame_cap)
constexpr uint64_t kGridReportFrames = 4096;  // refresh grid: one log line with the late count per this many frames
constexpr int kGridLogLines = 16;           // refresh grid: on / off / change lines per session
}  // namespace

struct FrameLimiter::Impl {
    std::atomic<double> target_fps{0.0};
    std::atomic<double> max_fps{0.0};        // the game's ceiling (set_max_fps); 0 = none
    std::atomic<FrameLimiter::PreFrameFn> pre_fn{nullptr};
    std::atomic<void*> pre_user{nullptr};
    std::atomic<double> sync_hz{0.0};        // RefreshSync: 0 = no fixed-refresh display
    std::atomic<bool> sync_grid{false}, sync_ceiling{false};
    std::atomic<double> ceiling_logged{0.0}; // the ceiling whose snap was last logged
    HANDLE timer = nullptr;  // high-resolution waitable timer; null means the Sleep(1) coarse wait

    // Guards the state below. begin_frame holds it only for bookkeeping, never during the wait.
    std::mutex mu;
    bool has_sched = false;
    double sched_ns = 0.0;    // scheduled start of the last frame (double, so a fractional period does not round away)
    bool has_prev = false;
    uint64_t prev_start = 0;  // actual start of the last frame
    uint64_t ring[kRing] = {};
    std::size_t ring_n = 0, ring_pos = 0;
    uint64_t late_ring[kRing] = {};          // lateness (ns) of the frames that waited
    std::size_t late_n = 0, late_pos = 0;
    uint64_t overran_frames = 0;             // frames that did not have to wait
    uint64_t last_wait = 0, last_late = 0;
    // Refresh grid (under mu). The anchor outlives a stretch of unpaced or free-schedule frames, so the phase against the display stays the
    // same across a loading screen or a ceiling that comes and goes; reset_schedule() and another refresh rate drop it.
    int grid_k = 0;           // refresh intervals per frame of the last paced frame; 0 = it was on the free schedule
    double grid_hz = 0.0;     // refresh rate the anchor belongs to; 0 = no anchor
    double grid_t0 = 0.0;     // anchor: grid point n is grid_t0 + n * 1e9 / grid_hz
    int64_t grid_slot = 0;    // grid point of the last grid frame
    uint64_t grid_frames = 0, grid_late = 0;      // since the last report line
    int grid_logged_k = 0, grid_log_lines = 0;

    // Used only by begin_frame, which is called from one thread per limiter.
    double overshoot_ema_ns = 0.0;  // average of (actual wake - requested wake) of the timer

    void record_locked(uint64_t t) {
        if (has_prev) {
            ring[ring_pos] = t - prev_start;
            ring_pos = (ring_pos + 1) % kRing;
            if (ring_n < kRing) ++ring_n;
        }
        has_prev = true;
        prev_start = t;
    }

    // What begin_frame paces to. fps: the target lowered to the game's ceiling (0 = no pacing); with RefreshSync::snap_ceiling a ceiling
    // that governs is first lowered to a divisor of the refresh rate (ceiling: the ceiling it came from, 0 = not snapped). k >= 2: fps is
    // hz / k and the frame goes on the refresh grid. k = 1 never uses the grid: a limiter on its own clock at the display's rate drifts against the display, and
    // avoiding that is the caller's policy.
    struct Pace {
        double fps = 0.0;
        int k = 0;
        double hz = 0.0;
        double ceiling = 0.0;
    };
    Pace pace() const {
        Pace p;
        const double t = target_fps.load(), m = max_fps.load(), hz = sync_hz.load();
        const bool ceiling = m > 0.0 && (!(t > 0.0) || m < t);
        p.fps = ceiling ? m : t;
        if (!(hz > 0.0) || !(p.fps > 0.0)) return p;
        if (ceiling && sync_ceiling.load()) {
            const FrameSnap sn = snap_frame_cap(m, hz);
            if (sn.k >= 2) {
                p.fps = sn.cap;
                p.ceiling = m;
            }
        }
        if (sync_grid.load()) {
            const double n = hz / p.fps;
            if (n < 1e6) {
                const int k = static_cast<int>(std::lround(n));
                if (k >= 2 && std::fabs(n - k) <= kDivisorTolerance * k) {
                    p.k = k;
                    p.hz = hz;
                    p.fps = hz / k;
                }
            }
        }
        return p;
    }

    void wait_until(uint64_t target);
};

namespace {
// How long before the target the timer stops and the spin takes over.
uint64_t margin_ns(double overshoot_ema_ns) {
    return static_cast<uint64_t>(std::min(std::max(kMinMarginNs, 2.0 * overshoot_ema_ns), kMaxMarginNs));
}

// Sleeps on the timer for coarse_ns (relative), then updates the overshoot average. False if the timer call failed.
// The timer is per thread: the main and render threads can both pace on one limiter, and a shared auto-reset timer wakes only one
// waiter, leaving the other blocked for good (the window-play hang at frame 4). The wait is also bounded so a lost signal can't hang.
bool coarse_wait_timer(uint64_t coarse_ns, double* overshoot_ema_ns) {
    thread_local HANDLE tl_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tl_timer) return false;
    LARGE_INTEGER due;
    const LONGLONG units = static_cast<LONGLONG>(coarse_ns / 100);  // 100 ns units; negative = relative
    due.QuadPart = -(units > 0 ? units : 1);
    const uint64_t set_at = FrameLimiter::now_ns();
    if (!SetWaitableTimerEx(tl_timer, &due, 0, nullptr, nullptr, nullptr, 0)) return false;
    WaitForSingleObject(tl_timer, static_cast<DWORD>(coarse_ns / 1000000ull) + 50);
    const uint64_t woke = FrameLimiter::now_ns();
    const uint64_t requested = set_at + coarse_ns;
    const double over = woke > requested ? static_cast<double>(woke - requested) : 0.0;
    *overshoot_ema_ns = *overshoot_ema_ns * 0.875 + over * 0.125;
    return true;
}

// Returns once the monotonic clock has reached target (absolute ns). have_timer: high-resolution timers exist (else Sleep(1) steps).
void wait_until_ns(uint64_t target, bool have_timer, double* overshoot_ema_ns) {
    for (;;) {
        const uint64_t t = FrameLimiter::now_ns();
        if (t >= target) return;
        const uint64_t remaining = target - t;
        const uint64_t margin = margin_ns(*overshoot_ema_ns);
        if (remaining > margin) {
            if (have_timer) {
                if (coarse_wait_timer(remaining - margin, overshoot_ema_ns)) continue;
                // The timer call failed: spin the rest.
            } else {
                Sleep(1);  // fallback when no high-resolution timer exists; no timeBeginPeriod
                continue;
            }
        }
        while (FrameLimiter::now_ns() < target) _mm_pause();
        return;
    }
}
}  // namespace

void FrameLimiter::Impl::wait_until(uint64_t target) { wait_until_ns(target, timer != nullptr, &overshoot_ema_ns); }

void FrameLimiter::sleep_until(uint64_t target_ns) {
    thread_local double overshoot = 0.0;
    wait_until_ns(target_ns, true, &overshoot);    // coarse_wait_timer falls back to the spin when the thread has no timer
}

FrameLimiter::FrameLimiter() : impl_(new Impl) {
    impl_->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

FrameLimiter::~FrameLimiter() {
    if (impl_->timer) CloseHandle(impl_->timer);
    delete impl_;
}

void FrameLimiter::set_target_fps(double fps) { impl_->target_fps.store(fps > 0.0 ? fps : 0.0); }

double FrameLimiter::target_fps() const { return impl_->target_fps.load(); }

void FrameLimiter::set_max_fps(double fps) { impl_->max_fps.store(fps > 0.0 ? fps : 0.0); }

double FrameLimiter::max_fps() const { return impl_->max_fps.load(); }

double FrameLimiter::effective_fps() const { return impl_->pace().fps; }

void FrameLimiter::set_refresh_sync(const RefreshSync& sync) {
    const bool on = sync.hz > 0.0;
    impl_->sync_hz.store(on ? sync.hz : 0.0);
    impl_->sync_grid.store(on && sync.grid);
    impl_->sync_ceiling.store(on && sync.snap_ceiling);
}

double FrameLimiter::refresh_hz() const { return impl_->sync_hz.load(); }

int FrameLimiter::grid_refreshes() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->has_sched ? impl_->grid_k : 0;
}

void FrameLimiter::shift_grid(double ns) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->grid_hz > 0.0) impl_->grid_t0 += ns;
}

void FrameLimiter::set_pre_frame_hook(PreFrameFn fn, void* user) {
    impl_->pre_user.store(user, std::memory_order_relaxed);
    impl_->pre_fn.store(fn, std::memory_order_release);
}

void FrameLimiter::run_pre_frame_hook() {
    if (PreFrameFn fn = impl_->pre_fn.load(std::memory_order_acquire)) fn(impl_->pre_user.load(std::memory_order_relaxed));
}

uint64_t FrameLimiter::begin_frame() {
    run_pre_frame_hook();
    return begin_frame_nohook();
}

namespace {
std::atomic<bool> g_unpaced{false};      // the loading detector's request
std::atomic<bool> g_player_cap{false};   // a frame limit of the player's own is in force
}
void frame_limiter_set_unpaced(bool on) { g_unpaced.store(on, std::memory_order_relaxed); }
bool frame_limiter_unpaced() { return g_unpaced.load(std::memory_order_relaxed) && !g_player_cap.load(std::memory_order_relaxed); }
void frame_limiter_set_player_cap(bool set) { g_player_cap.store(set, std::memory_order_relaxed); }

uint64_t FrameLimiter::begin_frame_nohook() {
    Impl& s = *impl_;
    const uint64_t entry = now_ns();
    const Impl::Pace pace = s.pace();
    const double fps = pace.fps;

    if (!(fps > 0.0) || g_unpaced.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.has_sched = false;
        s.last_wait = 0;
        s.last_late = 0;
        s.record_locked(entry);
        return entry;
    }

    if (pace.ceiling > 0.0 && s.ceiling_logged.exchange(pace.ceiling) != pace.ceiling)
        NXLOG_I("limiter", "game ceiling %g fps paced at %.3f fps (%.3f Hz display: a divisor of the refresh rate)", pace.ceiling, fps, s.sync_hz.load());
    const double period = 1e9 / fps;
    double next;
    int log_k = -1;                    // >= 0: the grid went on, off or to another k; logged after the lock
    uint64_t rep_frames = 0, rep_late = 0;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const double now = static_cast<double>(entry);
        if (pace.k >= 2) {
            // Refresh grid: the frame starts on a grid point, pace.k points after the last one. A frame that comes later than that takes the
            // first point not yet past (it would miss its refresh on the display anyway) and the anchor stays where it is, so neither the
            // phase against the display nor the spacing of the following frames changes, and no interval is shorter than a period.
            const double refresh = 1e9 / pace.hz;
            if (s.grid_hz != pace.hz) {
                // No vblank time reaches the limiter, so the phase is the start of the first frame paced on this display.
                s.grid_hz = pace.hz;
                s.grid_t0 = now;
                s.grid_slot = 0;
            } else {
                const bool follows = s.has_sched && s.grid_k > 0;      // the last frame was a grid frame
                const double at = (now - s.grid_t0) / refresh;         // now, in grid points
                int64_t slot = s.grid_slot + pace.k;
                if (!follows || at > static_cast<double>(slot) + kGridLateRefreshes) {
                    slot = static_cast<int64_t>(std::ceil(at - kGridLateRefreshes));
                    if (follows) ++s.grid_late;
                }
                s.grid_slot = slot;
            }
            next = s.grid_t0 + static_cast<double>(s.grid_slot) * refresh;
            if (++s.grid_frames >= kGridReportFrames) {
                rep_frames = s.grid_frames;
                rep_late = s.grid_late;
                s.grid_frames = s.grid_late = 0;
            }
        } else if (!s.has_sched || s.grid_k > 0) {
            next = now;
        } else {
            next = s.sched_ns + period;
            if (now > next + kResyncPeriods * period) next = now;  // stall: restart from now, no catch-up burst
            else if (now > next + kMaxCarryPeriods * period) next = now - kMaxCarryPeriods * period;  // late: carry only a little
        }
        s.grid_k = pace.k;
        if (pace.k != s.grid_logged_k && s.grid_log_lines < kGridLogLines) {
            ++s.grid_log_lines;
            s.grid_logged_k = log_k = pace.k;
        }
        s.sched_ns = next;
        s.has_sched = true;
    }
    if (log_k >= 2) NXLOG_I("limiter", "refresh grid: on, %.3f Hz, a frame every %d refreshes (%.3f fps)", pace.hz, log_k, fps);
    else if (log_k == 0) NXLOG_I("limiter", "refresh grid: off (%.3f fps is paced on the free schedule)", fps);
    if (rep_frames) NXLOG_I("limiter", "refresh grid: %llu frames, %llu started on a later refresh than their slot", static_cast<unsigned long long>(rep_frames),
                            static_cast<unsigned long long>(rep_late));

    const uint64_t target = static_cast<uint64_t>(std::ceil(next));
    const bool had_to_wait = target > entry;
    s.wait_until(target);
    const uint64_t start = now_ns();
    // A wait that ends long after its target means the thread was not scheduled (the timer fired late); a long planned wait means the schedule ran ahead.
    // Either one is a visible freeze, so say which one it was.
    if (start > target + 40000000ull || target > entry + static_cast<uint64_t>(4.0 * period))
        NXLOG_W("limiter", "frame wait: planned %.1f ms, woke %.1f ms after the target (frame period %.2f ms)", static_cast<double>(target > entry ? target - entry : 0) * 1e-6,
                static_cast<double>(start > target ? start - target : 0) * 1e-6, period * 1e-6);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.last_wait = start - entry;
        s.last_late = start > target ? start - target : 0;
        if (s.has_prev) {
            if (had_to_wait) {
                s.late_ring[s.late_pos] = s.last_late;
                s.late_pos = (s.late_pos + 1) % kRing;
                if (s.late_n < kRing) ++s.late_n;
            } else {
                ++s.overran_frames;
            }
        }
        s.record_locked(start);
    }
    return start;
}

void FrameLimiter::reset_schedule() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->has_sched = false;
    impl_->grid_hz = 0.0;     // the display or its mode may have changed: the next grid frame sets a new anchor
    impl_->has_prev = false;  // the interval spanning the pause is not measured
}

FrameLimiter::Stats FrameLimiter::stats() const {
    Stats st;
    std::vector<double> ms, late;
    uint64_t overran = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        ms.reserve(impl_->ring_n);
        for (std::size_t i = 0; i < impl_->ring_n; ++i) ms.push_back(static_cast<double>(impl_->ring[i]) * 1e-6);
        for (std::size_t i = 0; i < impl_->late_n; ++i) late.push_back(static_cast<double>(impl_->late_ring[i]) * 1e-6);
        overran = impl_->overran_frames;
    }
    st.waited = late.size();
    st.overran = overran;
    st.overran_fraction = (late.size() + overran) ? static_cast<double>(overran) / static_cast<double>(late.size() + overran) : 0.0;
    if (!late.empty()) {
        std::sort(late.begin(), late.end());
        st.late_max_ms = late.back();
        st.late_over_half_ms = static_cast<double>(late.end() - std::upper_bound(late.begin(), late.end(), 0.5)) / static_cast<double>(late.size());
        st.late_p99_ms = late[std::min(late.size() - 1, static_cast<std::size_t>(0.99 * static_cast<double>(late.size())))];
    }
    const std::size_t n = ms.size();
    if (n == 0) return st;

    double sum = 0.0, mn = ms[0], mx = ms[0];
    for (double d : ms) {
        sum += d;
        mn = std::min(mn, d);
        mx = std::max(mx, d);
    }
    const double mean = sum / static_cast<double>(n);
    double var = 0.0;
    for (double d : ms) var += (d - mean) * (d - mean);

    const std::size_t k = std::min(n - 1, static_cast<std::size_t>(0.99 * static_cast<double>(n)));
    std::nth_element(ms.begin(), ms.begin() + static_cast<std::ptrdiff_t>(k), ms.end());

    st.frames = n;
    st.mean_ms = mean;
    st.min_ms = mn;
    st.max_ms = mx;
    st.p99_ms = ms[k];
    st.stddev_ms = std::sqrt(var / static_cast<double>(n));
    st.fps = mean > 0.0 ? 1000.0 / mean : 0.0;
    return st;
}

void FrameLimiter::reset_stats() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->ring_n = 0;
    impl_->ring_pos = 0;
    impl_->late_n = 0;
    impl_->late_pos = 0;
    impl_->overran_frames = 0;
    impl_->has_prev = false;  // the interval spanning the reset is not measured
}

uint64_t FrameLimiter::last_wait_ns() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_wait;
}

uint64_t FrameLimiter::last_late_ns() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->last_late;
}

uint64_t FrameLimiter::now_ns() { return qpc::now_ns(); }

double suggest_vrr_cap(double refresh_hz) {
    if (refresh_hz < 100.0) return refresh_hz;     // see the header: under 100 Hz the lower cap costs more than VRR gives
    const double under = std::max(3.0, refresh_hz * refresh_hz / 3600.0);
    const double cap = refresh_hz - under;
    return cap < 1.0 ? 1.0 : cap;
}

FrameSnap snap_frame_cap(double cap, double hz) {
    FrameSnap r;
    r.cap = cap;
    if (!(hz > 0) || !(cap > 0) || cap >= hz) return r;
    const int k = static_cast<int>(std::ceil(hz / (cap * 1.005)));
    if (k < 2) return r;
    r.k = k;
    r.cap = hz / k;
    return r;
}

}  // namespace nxrt
