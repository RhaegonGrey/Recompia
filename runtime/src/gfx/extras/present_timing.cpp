// SPDX-License-Identifier: GPL-3.0-or-later
// Presentation timing and display-locked pacing (include/nxrt/present_timing.h).
#include "nxrt/present_timing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "pacing";
constexpr uint64_t kRing = 256;                 // frames kept (a record is complete about two frames after its present call)
constexpr uint64_t kLogNs = 10000000000ull;     // one summary every 10 s
constexpr uint64_t kHoldAfterVblankNs = 1000000; // a held present goes this long after the vblank before its slot
// vblank_lock controller (lock_frame)
constexpr int kStartFrames = 32;                // release -> GPU done samples the first target is taken from (their median + kStartMarginNs)
constexpr double kStartMarginNs = 5000000.0;
constexpr double kTargetStepNs = 500000.0;      // quantile step: a late frame raises the target (1 - p) of it, an on-time one lowers it p of it
constexpr double kMinTargetNs = 2000000.0;      // the target stays in [2 ms, 6 refreshes]
constexpr double kWalkNs = 2000000.0;           // an error beyond +-2 ms is walked off directly, a smaller one goes through the phase filter
constexpr double kErrAlpha = 0.125;             // phase: error filter (one-pole) per frame
constexpr double kDeadBandNs = 250000.0;        //   no move while the filtered error is inside +-0.25 ms
constexpr double kPhaseGain = 0.1;              //   share of the filtered error corrected a frame
constexpr double kPhaseStepNs = 150000.0;       //   at most 0.15 ms a frame
constexpr double kEarlierStepNs = 500000.0;     // the grid moves earlier at most 0.5 ms a frame (the limiter re-anchors a release later than 0.1 refresh)
constexpr double kLaterStepRefresh = 0.25;      // the grid walks later at most a quarter refresh a frame (beyond 1.5 refreshes: at once)
constexpr int kInflightMax = 4;                 // grid moves already made that the measured frame cannot show yet (one per frame released ahead)
constexpr double kPi = 3.14159265358979323846;

struct Cfg {
    bool stats = false, lock = false;
    double late = 0.01;                         // vblank_lock: share of the frames allowed to miss their slot
};
const Cfg& cfg() {
    static const Cfg c = [] {
        Cfg v;
        v.stats = config_get_bool("latency", "present_stats", false);
        v.lock = config_get_bool("latency", "vblank_lock", false);
        v.late = std::clamp(config_get_double("latency", "vblank_lock_late", 1.0), 0.1, 10.0) * 0.01;
        return v;
    }();
    return c;
}

struct Rec {
    std::atomic<uint64_t> frame{0};             // the frame the fields belong to (0 while it is being reset)
    std::atomic<uint64_t> call{0}, release{0}, replay{0}, held{0}, q0{0}, q1{0}, gpu{0}, shown{0};
};
Rec g_ring[kRing];
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_pending_release{0};     // boundary release not yet taken by a present call
std::atomic<FrameLimiter*> g_lim{nullptr};
std::atomic<bool> g_waiter{false};
std::atomic<uint64_t> g_waits[3];               // game frames with 0, 1, 2+ boundary waits (cumulative, always counted)
thread_local uint64_t t_render_frame = 0;

// the last frame known to be on the screen, for the hold
std::mutex g_shown_mu;
uint64_t g_shown_frame = 0, g_shown_ns = 0;

Rec* rec(uint64_t f) {
    if (!f) return nullptr;
    Rec& r = g_ring[f % kRing];
    return r.frame.load(std::memory_order_acquire) == f ? &r : nullptr;
}

double ms(uint64_t ns) { return static_cast<double>(ns) * 1e-6; }
double ms_between(uint64_t a, uint64_t b) { return (static_cast<double>(b) - static_cast<double>(a)) * 1e-6; }

// p in [0, 1] of v (sorts v); NaN when empty
double pct(std::vector<float>& v, double p) {
    if (v.empty()) return std::nan("");
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

// Everything below runs on the thread that completes a frame (the present waiter, or the submit thread without it), under g_mu.
std::mutex g_mu;
struct Agg {
    // the last two completed frames (a frame's time on screen is known when the next one is shown)
    uint64_t prev_f = 0, prev_s = 0, pprev_f = 0, pprev_s = 0;
    // window
    uint64_t t0 = 0, frames = 0, shown_n[4] = {}, exact = 0, late = 0, held = 0;
    uint64_t waits_at_t0[3] = {};
    double phase_c = 0, phase_s = 0;
    uint64_t phase_n = 0;
    std::vector<float> dur, rel_call, call_rep, rep_q, qblock, rel_gpu, gpu_shown, rel_shown, margin, hold;
    // vblank_lock controller (reset when the lock turns on)
    float start[kStartFrames] = {};
    int start_n = 0;
    double target_ns = 0;                       // release -> shown wanted (0 = not started)
    double err_f = 0;                           // filtered phase error
    double steps[kInflightMax] = {};            // the last grid moves, newest at step_pos - 1
    int step_pos = 0;
    bool lock_active = false;
    int lock_lines = 0;
    // vblank_lock window
    uint64_t lock_n = 0, lock_late = 0, lock_recover = 0, err_n = 0;
    double shift_pos = 0, shift_neg = 0, target_min = 0, target_max = 0, err_sum = 0;
    std::vector<float> err_abs;
};
Agg g_agg;

void log_window(Agg& a, uint64_t now, double hz, int k) {
    const double secs = static_cast<double>(now - a.t0) * 1e-9;
    uint64_t w[3];
    for (int i = 0; i < 3; ++i) {
        const uint64_t c = g_waits[i].load(std::memory_order_relaxed);
        w[i] = c - a.waits_at_t0[i];
        a.waits_at_t0[i] = c;
    }
    const char* src = g_waiter.load() ? "present wait" : "vkQueuePresentKHR calls, no VK_KHR_present_wait: queue intervals, not the screen";
    if (hz > 0) {
        NXLOG_I(kTag, "present timing: %.1f s, %llu frames (%s, %.2f Hz, %d refreshes a frame): shown for 1/2/3/4+ refreshes %llu/%llu/%llu/%llu, exactly %d: %.1f %%; boundary waits per game frame 0/1/2+ %llu/%llu/%llu",
                secs, static_cast<unsigned long long>(a.frames), src, hz, k, static_cast<unsigned long long>(a.shown_n[0]), static_cast<unsigned long long>(a.shown_n[1]),
                static_cast<unsigned long long>(a.shown_n[2]), static_cast<unsigned long long>(a.shown_n[3]), k, a.frames ? 100.0 * static_cast<double>(a.exact) / static_cast<double>(a.frames) : 0.0,
                static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]));
    } else {
        const double p50 = pct(a.dur, 0.5), p99 = pct(a.dur, 0.99), mx = a.dur.empty() ? 0.0 : a.dur.back();
        NXLOG_I(kTag, "present timing: %.1f s, %llu frames (%s, refresh rate not known: no fixed-refresh panel with vsync): frame on screen p50 %.2f p99 %.2f max %.2f ms; boundary waits per game frame 0/1/2+ %llu/%llu/%llu",
                secs, static_cast<unsigned long long>(a.frames), src, p50, p99, mx, static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]),
                static_cast<unsigned long long>(w[2]));
    }
    char phase[96] = "n/a";
    if (a.phase_n && hz > 0) {
        const double refresh = 1000.0 / hz;
        const double c = a.phase_c / static_cast<double>(a.phase_n), s = a.phase_s / static_cast<double>(a.phase_n);
        double ang = std::atan2(s, c);
        if (ang < 0) ang += 2 * kPi;
        const double rbar = std::min(1.0, std::sqrt(c * c + s * s));
        const double spread = rbar > 1e-9 ? refresh / (2 * kPi) * std::sqrt(-2.0 * std::log(rbar)) : refresh;
        std::snprintf(phase, sizeof phase, "mean %.2f spread %.2f ms", ang / (2 * kPi) * refresh, spread);
    }
    const double qmax = a.qblock.empty() ? 0.0 : (std::sort(a.qblock.begin(), a.qblock.end()), static_cast<double>(a.qblock.back()));
    char lock[400] = "";
    if (a.lock_active && a.target_ns > 0 && hz > 0) {
        const double refresh_ns = 1e9 / hz;
        const double phase_t = refresh_ns - std::fmod(a.target_ns, refresh_ns);     // the release after the vblank its frame is shown on, at the target
        std::snprintf(lock, sizeof lock,
                      "; vblank lock: release->shown target %.2f ms (%.2f-%.2f in the window), target release after vblank %.2f ms, error mean %+.2f |error| p90 %.2f ms "
                      "over %llu frames, %llu frames beyond +-2 ms walked, %llu late (%.2f %%, allowed %.2f %%), grid moved %+.2f / %+.2f ms",
                      a.target_ns * 1e-6, a.target_min * 1e-6, a.target_max * 1e-6, (phase_t >= refresh_ns ? 0.0 : phase_t) * 1e-6,
                      a.err_n ? a.err_sum / static_cast<double>(a.err_n) * 1e-6 : 0.0, pct(a.err_abs, 0.9), static_cast<unsigned long long>(a.err_n),
                      static_cast<unsigned long long>(a.lock_recover), static_cast<unsigned long long>(a.lock_late),
                      a.lock_n ? 100.0 * static_cast<double>(a.lock_late) / static_cast<double>(a.lock_n) : 0.0, cfg().late * 100.0, a.shift_pos * 1e-6, a.shift_neg * 1e-6);
    }
    NXLOG_I(kTag, "present timing ms p50/p99: release->call %.2f/%.2f, call->replay done %.2f/%.2f, replay->queue present %.2f/%.2f, queue present blocked %.2f/%.2f (max %.2f), "
                  "release->GPU done %.2f/%.2f, GPU done->shown %.2f/%.2f, release->shown %.2f/%.2f; release after vblank %s; GPU done before its target vblank p1 %.2f p50 %.2f (%llu frames late); "
                  "held %llu frames, hold p50 %.2f p99 %.2f%s",
            pct(a.rel_call, 0.5), pct(a.rel_call, 0.99), pct(a.call_rep, 0.5), pct(a.call_rep, 0.99), pct(a.rep_q, 0.5), pct(a.rep_q, 0.99), pct(a.qblock, 0.5),
            pct(a.qblock, 0.99), qmax, pct(a.rel_gpu, 0.5), pct(a.rel_gpu, 0.99), pct(a.gpu_shown, 0.5), pct(a.gpu_shown, 0.99), pct(a.rel_shown, 0.5),
            pct(a.rel_shown, 0.99), phase, pct(a.margin, 0.01), pct(a.margin, 0.5), static_cast<unsigned long long>(a.late), static_cast<unsigned long long>(a.held),
            pct(a.hold, 0.5), pct(a.hold, 0.99), lock);
}

void reset_window(Agg& a, uint64_t now) {
    a.t0 = now;
    a.frames = a.exact = a.late = a.held = a.phase_n = a.lock_n = a.lock_late = a.lock_recover = a.err_n = 0;
    for (uint64_t& n : a.shown_n) n = 0;
    a.phase_c = a.phase_s = a.shift_pos = a.shift_neg = a.err_sum = 0;
    a.target_min = a.target_max = a.target_ns;
    for (auto* v : {&a.dur, &a.rel_call, &a.call_rep, &a.rep_q, &a.qblock, &a.rel_gpu, &a.gpu_shown, &a.rel_shown, &a.margin, &a.hold, &a.err_abs}) v->clear();
}

void lock_reset(Agg& a) {
    a.start_n = a.step_pos = 0;
    a.target_ns = a.err_f = 0;
    for (double& s : a.steps) s = 0;
}

// vblank_lock: frame p, shown at s_p (the frame before at s_prev, 0 = unknown), moves the limiter's grid so release -> shown = the target.
//
// The target is a quantile tracker on the frames that miss their slot: a late frame raises it by (1 - p) * 0.5 ms, an on-time one lowers it
// by p * 0.5 ms, so it settles where a share p of the frames is late ([latency] vblank_lock_late). That is the release -> GPU done
// percentile the frames need, without a percentile window that jumps with each spike (the first controller took p99 of 128 frames plus a
// guard and chased it, so the phase never settled). The step trades noise for speed: about 0.7 ms standard deviation of the target, and
// it comes down about 0.2 ms a second (at 45 fps, p = 1 %) when the load drops; a rising load raises it at once through the late frames.
//
// The error e = release -> shown - target, told apart by size:
// - |e| <= 2 ms: the phase (clock drift, the target's small steps). Filtered (one-pole), a dead band of 0.25 ms, a small capped step: the
//   grid sits still in the steady state;
// - e > 2 ms: a late frame or a hitch moved the frames on the screen a refresh or more later and the hold keeps them there (frame i shows no
//   earlier than frame i-1 + k refreshes), so the release is that much early: the grid walks later a quarter refresh a frame, at once by
//   all but half a refresh beyond 1.5 refreshes. Left to the filter, the last few ms of such a step took dozens of frames;
// - e < -2 ms: the release is late for its slot: the grid moves earlier at most 0.5 ms a frame.
// A grid move shows in the measured frames only after the frames already released (about target / frame time of them), so the moves
// still in flight are taken off the error, and the filter sees only what the grid has not answered yet.
void lock_frame(Agg& a, FrameLimiter* lim, const Rec& r, uint64_t p, uint64_t s_p, uint64_t s_prev, int k, double refresh_ns) {
    const uint64_t rel = r.release.load(std::memory_order_relaxed), gpu = r.gpu.load(std::memory_order_relaxed);
    if (!rel || !gpu || gpu < rel || s_p < rel) return;
    const double x = static_cast<double>(s_p - rel);
    if (a.target_ns <= 0) {
        a.start[a.start_n++] = static_cast<float>(gpu - rel);
        if (a.start_n < kStartFrames) return;
        std::vector<float> v(a.start, a.start + kStartFrames);
        a.target_ns = std::clamp(pct(v, 0.5) + kStartMarginNs, kMinTargetNs, 6 * refresh_ns);
        a.target_min = a.target_max = a.target_ns;
        return;
    }
    // late: the frame before stayed k + 1 refreshes or more although this frame's release came on time
    bool late = false;
    if (s_prev) {
        const Rec* rp = rec(p - 1);
        const uint64_t rel_prev = rp ? rp->release.load(std::memory_order_relaxed) : 0;
        const double slot = (static_cast<double>(k) + 0.5) * refresh_ns;
        late = rel_prev && rel > rel_prev && static_cast<double>(s_p - s_prev) >= slot && static_cast<double>(rel - rel_prev) < slot;
    }
    ++a.lock_n;
    a.lock_late += late;
    const double q = cfg().late;
    a.target_ns = std::clamp(a.target_ns + (late ? (1.0 - q) : -q) * kTargetStepNs, kMinTargetNs, 6 * refresh_ns);
    a.target_min = std::min(a.target_min, a.target_ns);
    a.target_max = std::max(a.target_max, a.target_ns);

    const int ahead = std::clamp(static_cast<int>(std::ceil(a.target_ns / (k * refresh_ns))), 1, kInflightMax);
    double inflight = 0;
    for (int i = 1; i <= ahead; ++i) inflight += a.steps[(a.step_pos - i + kInflightMax) % kInflightMax];
    const double e = x - a.target_ns - inflight;      // > 0: the release can come later
    double step = 0;
    if (e > 1.5 * refresh_ns) {
        step = e - 0.5 * refresh_ns;
        ++a.lock_recover;
    } else if (e > kWalkNs) {
        step = std::min(e, kLaterStepRefresh * refresh_ns);
        ++a.lock_recover;
    } else if (e < -kWalkNs) {
        step = std::max(e, -kEarlierStepNs);
        ++a.lock_recover;
    } else {
        a.err_f += kErrAlpha * (e - a.err_f);
        a.err_sum += e;
        ++a.err_n;
        a.err_abs.push_back(static_cast<float>(std::fabs(e) * 1e-6));
        if (std::fabs(a.err_f) > kDeadBandNs) step = std::clamp(kPhaseGain * a.err_f, -kPhaseStepNs, kPhaseStepNs);
        a.err_f -= step;     // answered: the measured error drops by it only frames later, and is counted in flight until then
    }
    a.steps[a.step_pos] = step;
    a.step_pos = (a.step_pos + 1) % kInflightMax;
    if (step == 0) return;
    lim->shift_grid(step);
    (step > 0 ? a.shift_pos : a.shift_neg) += step;
}

// Frame p was shown at s_p and the next one at s_next.
void frame_done(Agg& a, uint64_t p, uint64_t s_p, uint64_t s_next, uint64_t s_prev, uint64_t now) {
    FrameLimiter* lim = g_lim.load(std::memory_order_acquire);
    const double hz = lim ? lim->refresh_hz() : 0.0;
    const int k = lim ? lim->grid_refreshes() : 0;
    const double refresh_ns = hz > 0 ? 1e9 / hz : 0.0;
    if (!a.t0) {
        a.t0 = now;
        for (int i = 0; i < 3; ++i) a.waits_at_t0[i] = g_waits[i].load(std::memory_order_relaxed);
    }
    ++a.frames;
    const double dur = static_cast<double>(s_next - s_p);
    if (refresh_ns > 0) {
        const long n = std::lround(dur / refresh_ns);
        ++a.shown_n[n <= 1 ? 0 : n >= 4 ? 3 : n - 1];
        if (n == k) ++a.exact;
    }
    a.dur.push_back(static_cast<float>(dur * 1e-6));
    if (const Rec* r = rec(p)) {
        const uint64_t rel = r->release.load(std::memory_order_relaxed), call = r->call.load(std::memory_order_relaxed),
                       rep = r->replay.load(std::memory_order_relaxed), q0 = r->q0.load(std::memory_order_relaxed), q1 = r->q1.load(std::memory_order_relaxed),
                       gpu = r->gpu.load(std::memory_order_relaxed), held = r->held.load(std::memory_order_relaxed);
        if (rel && call >= rel) a.rel_call.push_back(static_cast<float>(ms_between(rel, call)));
        if (call && rep >= call) a.call_rep.push_back(static_cast<float>(ms_between(call, rep)));
        if (rep && q0 >= rep) a.rep_q.push_back(static_cast<float>(ms_between(rep, q0)));
        if (q0 && q1 >= q0) a.qblock.push_back(static_cast<float>(ms_between(q0, q1)));
        if (rel && gpu >= rel) a.rel_gpu.push_back(static_cast<float>(ms_between(rel, gpu)));
        if (gpu && s_p >= gpu) a.gpu_shown.push_back(static_cast<float>(ms_between(gpu, s_p)));
        if (rel && s_p >= rel) a.rel_shown.push_back(static_cast<float>(ms_between(rel, s_p)));
        a.held += held != 0;
        a.hold.push_back(static_cast<float>(ms(held)));
        if (rel && refresh_ns > 0) {      // the release's phase after the vblank the frame was shown on (a vblank of the measured grid)
            const double x = (static_cast<double>(rel) - static_cast<double>(s_p)) / refresh_ns;
            const double ang = 2 * kPi * (x - std::floor(x));
            a.phase_c += std::cos(ang);
            a.phase_s += std::sin(ang);
            ++a.phase_n;
        }
        if (gpu && s_prev && refresh_ns > 0) {    // the vblank the frame was due on: the previous frame's + k refreshes (1 without a cap)
            const double target = static_cast<double>(s_prev) + std::max(k, 1) * refresh_ns;
            const double m = (target - static_cast<double>(gpu)) * 1e-6;
            a.margin.push_back(static_cast<float>(m));
            if (static_cast<double>(s_p) > target + 0.5 * refresh_ns) ++a.late;
        }
        const bool lock = cfg().lock && lim && g_waiter.load() && k >= 2 && refresh_ns > 0 && !frame_limiter_unpaced();
        if (lock != a.lock_active && a.lock_lines < 16) {
            ++a.lock_lines;
            if (lock) NXLOG_I(kTag, "vblank lock: on (%.2f Hz, a frame every %d refreshes): presents held to their slot, the limiter's grid follows the vblank", hz, k);
            else NXLOG_I(kTag, "vblank lock: off (%s)", !g_waiter.load() ? "no present wait" : k < 2 ? "no frame cap of refresh / k on a fixed-refresh panel" : "unpaced frames");
        }
        if (lock && !a.lock_active) lock_reset(a);
        a.lock_active = lock;
        if (lock) lock_frame(a, lim, *r, p, s_p, s_prev, k, refresh_ns);
    }
    if (now - a.t0 >= kLogNs) {     // (vblank_lock alone keeps no log, only the window's samples are dropped)
        if (cfg().stats) log_window(a, now, hz, k);
        reset_window(a, now);
    }
}

// Frame f reached the screen (or, without the present waiter, the queue) at s.
void complete(uint64_t f, uint64_t s) {
    std::lock_guard<std::mutex> lk(g_mu);
    Agg& a = g_agg;
    if (a.prev_f && f == a.prev_f + 1 && s >= a.prev_s)
        frame_done(a, a.prev_f, a.prev_s, s, a.pprev_f && a.pprev_f + 1 == a.prev_f ? a.pprev_s : 0, FrameLimiter::now_ns());
    a.pprev_f = a.prev_f;
    a.pprev_s = a.prev_s;
    a.prev_f = f;
    a.prev_s = s;
}
}  // namespace

bool present_timing_on() { return cfg().stats || cfg().lock; }
bool present_wait_wanted() { return present_timing_on(); }
void present_timing_set_limiter(FrameLimiter* lim) { g_lim.store(lim, std::memory_order_release); }

void present_timing_set_waiter(bool runs) {
    static bool logged = false;
    g_waiter.store(runs);
    if (!present_timing_on() || logged) return;
    logged = true;
    NXLOG_I(kTag, "present timing: %s%s", runs ? "on, the screen times come from VK_KHR_present_wait" : "on, VK_KHR_present_wait not available: frame intervals at vkQueuePresentKHR only",
            cfg().lock ? (runs ? "; [latency] vblank_lock wanted" : "; [latency] vblank_lock needs it: off") : "");
}

uint64_t present_timing_game_present(uint32_t boundary_waits) {
    g_waits[std::min<uint32_t>(boundary_waits, 2)].fetch_add(1, std::memory_order_relaxed);
    if (!present_timing_on()) return 0;
    const uint64_t f = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    Rec& r = g_ring[f % kRing];
    r.frame.store(0, std::memory_order_relaxed);
    r.call.store(FrameLimiter::now_ns(), std::memory_order_relaxed);
    r.release.store(g_pending_release.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    for (auto* x : {&r.replay, &r.held, &r.q0, &r.q1, &r.gpu, &r.shown}) x->store(0, std::memory_order_relaxed);
    r.frame.store(f, std::memory_order_release);
    return f;
}

void present_timing_release(uint64_t t_ns) {
    if (present_timing_on()) g_pending_release.store(t_ns, std::memory_order_relaxed);
}

void present_timing_render_begin(uint64_t frame) {
    t_render_frame = frame;
    if (Rec* r = rec(frame)) r->replay.store(FrameLimiter::now_ns(), std::memory_order_relaxed);
}

uint64_t present_timing_render_frame() { return t_render_frame; }

void present_timing_present_release(uint64_t t_ns) {
    if (Rec* r = rec(t_render_frame)) r->release.store(t_ns, std::memory_order_relaxed);
}

void present_timing_queue_present(uint64_t frame, uint64_t held_ns, uint64_t t0, uint64_t t1) {
    Rec* r = rec(frame);
    if (!r) return;
    r->held.store(held_ns, std::memory_order_relaxed);
    r->q0.store(t0, std::memory_order_relaxed);
    r->q1.store(t1, std::memory_order_relaxed);
    if (!g_waiter.load(std::memory_order_relaxed)) complete(frame, t1);
}

void present_timing_gpu_done(uint64_t frame, uint64_t t_ns) {
    if (Rec* r = rec(frame)) r->gpu.store(t_ns, std::memory_order_relaxed);
}

void present_timing_shown(uint64_t frame, uint64_t t_ns) {
    Rec* r = rec(frame);
    if (!r) return;
    r->shown.store(t_ns, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_shown_mu);
        if (frame > g_shown_frame) {
            g_shown_frame = frame;
            g_shown_ns = t_ns;
        }
    }
    complete(frame, t_ns);
}

uint64_t present_timing_hold_until(uint64_t frame, bool fifo) {
    if (!cfg().lock || !fifo || !frame || !g_waiter.load(std::memory_order_relaxed) || frame_limiter_unpaced()) return 0;
    FrameLimiter* lim = g_lim.load(std::memory_order_acquire);
    if (!lim) return 0;
    const int k = lim->grid_refreshes();
    const double hz = lim->refresh_hz();
    if (k < 2 || !(hz > 0)) return 0;
    uint64_t j, s_j;
    {
        std::lock_guard<std::mutex> lk(g_shown_mu);
        j = g_shown_frame;
        s_j = g_shown_ns;
    }
    if (!j || frame <= j || frame - j > 4) return 0;      // nothing recent on the screen to count from
    const double refresh_ns = 1e9 / hz;
    // frame j went out on the vblank at s_j; frame i is due k refreshes after frame i-1: presented after the vblank before that, FIFO cannot show it earlier
    const double t = static_cast<double>(s_j) + (static_cast<double>((frame - j) * static_cast<uint64_t>(k)) - 1.0) * refresh_ns + kHoldAfterVblankNs;
    const double now = static_cast<double>(FrameLimiter::now_ns());
    if (t <= now || t - now > k * refresh_ns) return 0;
    return static_cast<uint64_t>(t);
}

}  // namespace nxrt::extras
