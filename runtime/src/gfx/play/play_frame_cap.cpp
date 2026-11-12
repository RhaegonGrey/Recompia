// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: present mode choice, the fps snap and the frame cap policy.
#include "play_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

// vsync off: immediate (tearing, lowest latency; mailbox where immediate is missing). vsync on: FIFO, except with [display] fps_limit = 0
// (uncapped): mailbox, so the game renders as fast as it can and the display still shows whole frames.
gfx::PresentMode present_mode_for(const DisplaySettings& d) {
    if (!d.vsync) return gfx::PresentMode::Immediate;
    return d.fps_cap == 0 ? gfx::PresentMode::Mailbox : gfx::PresentMode::Fifo;
}

// [display] fps_snap = auto|true|false (controls.ini / F1 "Cap FPS to 30 for Steam Deck"; NXRT_DISPLAY_FPS_SNAP overrides it), read on every apply_frame_cap. auto (default) = on for a detected Steam Deck
// only (host_is_steam_deck: its 90 / 60 Hz panel has no VRR), off on every other machine (a desktop screen may be VRR, and a
// player's chosen 40 must stay 40). Returns the divisor k (>= 2) the cap snaps to, or 0 with *reason set to why it does not.
// The first half of that decision, without the cap: the player has vsync on and fps_snap says the screen is a fixed-refresh panel.
static bool snap_wanted(const DisplaySettings& d, const char** reason) {
    std::string mode = config_get_str("display", "fps_snap", "");    // the env override, else the player's setting
    if (mode.empty()) mode = d.fps_snap;
    const bool forced_on = mode == "true" || mode == "on" || mode == "1";
    const bool forced_off = mode == "false" || mode == "off" || mode == "0";
    if (!d.vsync) { *reason = "vsync off"; return false; }
    if (forced_off) { *reason = "fps_snap off"; return false; }
    if (!forced_on && !host_is_steam_deck()) { *reason = "fps_snap auto, not a Steam Deck"; return false; }
    return true;
}

int frame_snap_k(const DisplaySettings& d, double cap, float hz, double* snapped, const char** reason) {
    *snapped = cap;
    if (!snap_wanted(d, reason)) return 0;
    if (cap <= 0) { *reason = "uncapped"; return 0; }
    if (hz <= 0) { *reason = "refresh rate unknown"; return 0; }
    if (cap >= hz) { *reason = "cap not below the refresh rate"; return 0; }
    const FrameSnap sn = snap_frame_cap(cap, hz);
    if (sn.k < 2) { *reason = "cap within 0.5% of the refresh rate"; return 0; }
    *snapped = sn.cap;
    return sn.k;
}

void log_frame_cap(double req, double snapped, float hz, int k, const char* reason) {
    static std::mutex mu;
    static double l_req = -2, l_snapped = -2, l_hz = -2;
    static int l_k = -2;
    std::lock_guard<std::mutex> lk(mu);
    if (req == l_req && snapped == l_snapped && hz == l_hz && k == l_k) return;
    l_req = req; l_snapped = snapped; l_hz = hz; l_k = k;
    if (k >= 2) NXLOG_I(kTag, "frame cap %g -> %g (%g Hz panel, every %d refreshes)", req, snapped, static_cast<double>(hz), k);
    else NXLOG_I(kTag, "frame cap %g (%g Hz, no snap: %s)", req, static_cast<double>(hz), reason);
}

void apply_frame_cap(const Settings& s) {
    G& st = g();
    double cap = s.display.fps_cap;
    if (cap < 0) {
        // fps_limit = auto: the display's refresh rate (no limit while it is unknown, e.g. headless runs)
        const float hz = st.window ? platform::display_refresh_hz() : 0.f;
        cap = hz > 0 ? static_cast<double>(hz) : 0;
    }
    // A fixed-refresh panel with vsync (what fps_snap decides, whatever the cap)
    const float panel_hz = st.window ? platform::display_refresh_hz() : 0.f;
    const char* why_not_fixed = "";
    const bool fixed = panel_hz > 0 && snap_wanted(s.display, &why_not_fixed);
    {   // there the limiter keeps frames on the display's refresh grid; [latency] grid = false: every pace stays on its free schedule.
        // The game's ceiling follows the snap too ([latency] snap_ceiling), with any cap and with mailbox presents: the snapped cap may be
        // above it (a 90 Hz panel at its refresh rate, the ceiling 60).
        static const bool grid = config_get_bool("latency", "grid", true);
        static const bool snap_ceiling = config_get_bool("latency", "snap_ceiling", true);
        FrameLimiter::RefreshSync sync;
        if (fixed) {
            sync.hz = static_cast<double>(panel_hz);
            sync.grid = grid;
            sync.snap_ceiling = snap_ceiling;
        }
        st.limiter.set_refresh_sync(sync);
    }
    if (s.display.vsync && s.display.fps_cap == 0) {
        cap = 0;     // uncapped with vsync: mailbox presents, no limiter
    } else if (s.display.vsync) {
        // The display paces; a cap only matters when it is below the refresh rate. With no cap below the refresh rate, the frame limiter still keeps just
        // under it (S21, [latency] vsync_cap): the game otherwise renders as fast as the swapchain takes frames, the queue behind the display
        // fills (three images) and every picture is shown one to two frames after it was simulated; just under the refresh rate the queue stays empty.
        // On a fixed-refresh panel that cap beats against the display: the limiter's clock is not the display's, so at 0.05 % under the
        // reported rate a frame is shown twice about every 2000 refreshes (22 s at 90 Hz), and more often or never when the real rate
        // differs from the reported one. There the FIFO queue alone paces a rate at the refresh rate ([latency] fifo_pace): no limiter
        // wait, every refresh gets the next frame, and the frames queued behind the display are bounded by the waits for frames in
        // flight ([latency] frames_ahead, gpu_frames_ahead) instead of by a clock. The price is up to that queue of latency.
        const float hz = panel_hz;
        static const bool vsync_cap = config_get_bool("latency", "vsync_cap", true);
        static const bool fifo_pace = config_get_bool("latency", "fifo_pace", true);
        const double req = cap;
        double snapped = cap;
        const char* reason = "";
        const int k = frame_snap_k(s.display, cap, hz, &snapped, &reason);
        // k < 2 on a fixed panel: the cap is the refresh rate, above it, or within the snap's 0.5 % under it
        const bool display_paces = fixed && fifo_pace && k < 2;
        log_frame_cap(req, snapped, hz, k, display_paces ? "at the refresh rate: vsync alone paces, the limiter does not wait" : reason);
        if (k >= 2) cap = snapped;      // hz / k: the limiter period is exactly k refresh intervals
        else if (display_paces) cap = 0;
        else if (cap <= 0 || (hz > 0 && cap >= hz)) cap = (vsync_cap && hz > 0) ? static_cast<double>(hz) * 0.9995 : 0;
    } else {
        log_frame_cap(cap, cap, 0.f, 0, "vsync off");
    }
    st.limiter.set_target_fps(cap);
    // A frame limit the player set (a number, not auto and not unlimited) holds through loading screens too: on a Mac a 60 limit showed
    // 110 fps for the first 16 s of every start, because boot loading unpaces the game on purpose. auto and unlimited keep the faster
    // unpaced loading (nvn_replay's detector asks; frame_limiter_unpaced answers).
    nxrt::frame_limiter_set_player_cap(s.display.fps_cap > 0);
    // Reflex / Anti-Lag pace to the same cap (remembered for a renderer that is created later), the game's ceiling included (set_max_fps)
    extras::lowlat_note_cap(st.limiter.effective_fps());
}

}  // namespace detail
}  // namespace nxrt::play
