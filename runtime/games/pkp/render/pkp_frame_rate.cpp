// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play mode: frame rates above 60 at the right game speed (pkp_play.h; split out of pkp_play.cpp). Offsets in `main` of the
// 2.0.1 update.
//
// Frame rate. The main loop's frame step (0xbfc404) turns the measured frame time into a frame multiplier in 60 Hz units that 90 functions
// read (the float at 0x34c03f0): 1.0 within 1.5 ms of 1/60 s, else dt * 60 clamped to [1, 3]. Faster than 60 fps the multiplier stays 1
// and the game runs fast. The mid hook at the join point (0xbfc4fc: s0 the multiplier, s8 the frame time) lets frames shorter than 1/60 s
// count as dt * 60 (down to 1/8). At 60 fps and below nothing changes. The general value tween (0xe51624: the area-transition
// iris and others) steps its counter by (u32)(multiplier + 0.5), which is 0 above 120 fps; mid hooks at its four step sites
// (0xe51688, 0xe516f0, 0xe5173c, 0xe51788; they change the lifted code) replace the step with whole 60 Hz frames, carrying the
// fraction in an accumulator per tween (hook_tween_step).
// Event hold (#297): the game's scripted events were built for a step of exactly 1.0. Above 60 fps DJ Rotom's mood 100 event never got past
// "YOOOOOO!" and at a 60 fps limit it finishes; the consumer that breaks on fractional steps has not been found among the multiplier's readers.
// So while the player cannot turn the camera (the follow camera's turn branch, hook_cam_stick, closed for 200 ms: an event, a dialogue, a
// transition, a menu) or the NVN replay marks loading frames, and for [pkp] event_hold_ms (default 1500) after, the frame limiter's ceiling
// holds the game at 60 fps and the multiplier keeps the retail rule. In free play the frame limit and the fractional multiplier apply.
// Where the frame cap snaps to the refresh rate (a Steam Deck with vsync) the limiter lowers that 60 to the next divisor of it (45 on a
// 90 Hz panel: [latency] snap_ceiling); the retail rule covers it, a step above 1.0 being what the console does on a slow frame.
// The 60 fps cap is the game's own: the main loop's frame step (0x89def8, profiler zone "Sync") waits, sleeping in 200 us task-graph waits,
// until tick frequency / 60 ticks (60 = 60 / 0x13fc00(), a constant 1.0) have passed since the previous frame began. The mid hook right
// after that division (0x89e27c, x22 = the period in ticks) sets the period to 0, so the runtime's frame limiter at the frame boundary
// ([display] fps_limit) alone paces the game ([pkp] game_frame_limit = 1 keeps the game's 60 Hz wait). In the frame step's fixed-step mode
// (byte +0xb4 of the main-loop object, copied to sp+0xc) the frame time is 1 / 60 whatever the real time; the mid hook before that division
// (0x89e398, s0 = 60.0, x28 now, x25 the previous frame's tick, d8 the tick frequency) puts the measured rate in s0 for frames shorter
// than 1/60 s, the same rule as the multiplier.
#include "input/pkp_play_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "nxrt/extras.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"
#include "nxrt/lowlat.h"
#include "nxrt/play.h"

namespace nxrt::games::pkp::play_detail {

// ---- frame rate ----
uint64_t mono_ns();

// once per frame: set or clear the 60 fps ceiling of the event hold; true while it holds
// ponytail: holds menus too (same closed camera branch), an event-only signal needs the script player's state
bool event_hold(State& s) {
    if (!s.hold_on.load(std::memory_order_relaxed)) return false;
    const uint64_t now = mono_ns();
    if (frame_limiter_unpaced()) s.seq_ns.store(now, std::memory_order_relaxed);
    const uint64_t turn = s.turn_ns.load(std::memory_order_relaxed);
    if (turn == 0 || now - turn > 200000000ull) s.seq_ns.store(now, std::memory_order_relaxed);
    const uint64_t last = s.seq_ns.load(std::memory_order_relaxed);
    const bool want = last != 0 && now - last < s.hold_ns;
    if (want != s.holding) {
        s.holding = want;
        if (FrameLimiter* lim = play::limiter()) {
            lim->set_max_fps(want ? kGameMaxFps : 0.0);
            extras::lowlat_note_cap(lim->effective_fps());
        }
        if (want) {
            s.hold_start_ns = now;
            ++s.holds;
            if (s.holds <= 20) {
                FrameLimiter* lim = play::limiter();
                NXLOG_I(kTag, "event hold %llu: game held at 60 fps (paced at %.2f)", static_cast<unsigned long long>(s.holds), lim ? lim->effective_fps() : 0.0);
            }
        } else if (s.holds <= 20) {
            NXLOG_I(kTag, "event hold %llu: released after %llu ms", static_cast<unsigned long long>(s.holds),
                    static_cast<unsigned long long>((now - s.hold_start_ns) / 1000000ull));
        }
    }
    return want;
}

int hook_frame_multiplier(::Ctx* c, uint64_t, void*) {
    State& s = st();   // the registry's game-frame tick ran just before, at the same site (pkp_game.cpp)
    if (!s.fps_fix.load(std::memory_order_relaxed)) return 0;
    if (event_hold(s)) {   // the retail rule while an event plays
        s.last_mult.store(c->v[0].f32[0], std::memory_order_relaxed);
        return 0;
    }
    const float dt = c->v[8].f32[0];
    // #275: only while the game really runs above 60 fps. At 60 fps or less (a 60 Hz display with vsync, a cap of 60 or lower) every frame
    // stays on screen a whole refresh or more, and a frame that ran short (the one after a late frame, or one let through early by the
    // present's back pressure) moved the world by less than the time it is shown: walking and camera turns lurched about once a second at
    // a steady 60 fps, and constantly with the limit off and vsync on. The retail rule is right there. Hysteresis: on above 64, off below 62.
    if (dt > 0.f) s.avg_dt += (std::min(dt, 0.1f) - s.avg_dt) * 0.0625f;   // about 16 frames
    const bool above = s.avg_dt < (s.above60.load(std::memory_order_relaxed) ? 1.f / 62.f : 1.f / 64.f);
    s.above60.store(above, std::memory_order_relaxed);
    // the game's own rule keeps 1.0 within 1.5 ms of 1/60 s; only shorter frames change
    if (above && dt > 0.f && dt < (1.f / 60.f - 0.0015152f)) {
        const float m = std::max(dt * 60.f, 0.125f);
        c->v[0].f32[0] = m;
        s.last_mult.store(m, std::memory_order_relaxed);
        s.fps_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.last_mult.store(c->v[0].f32[0], std::memory_order_relaxed);
    }
    return 0;
}

// General value tween f_7100e51624 (the area-transition iris and others): the counter steps by (u32)(multiplier + 0.5) per frame, which is 0
// for the fractional multiplier above 120 fps. Counts whole 60 Hz frames instead, carrying the fraction per tween (keyed by the tween object),
// so the tween keeps its 60 fps timing. n <= 3: acc < 1 and the multiplier <= 3. The counter can count down, so a restart is detected by
// the counter not being last +- step (the game started or replaced the tween), which drops the carried fraction.
int hook_tween_step(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.fps_fix.load(std::memory_order_relaxed)) return 0;
    const uint64_t obj = c->x[19];
    const uint32_t cnt = static_cast<uint32_t>(c->x[10]);
    float n;
    {
        std::lock_guard<std::mutex> lk(s.tween_mu);
        State::TweenAcc* e = nullptr;
        for (State::TweenAcc& t : s.tween) {
            if (t.obj == obj) { e = &t; break; }
        }
        if (!e) {
            e = &s.tween[s.tween_next++ % 64];
            *e = State::TweenAcc{};
            e->obj = obj;
        } else if (cnt != e->last + e->step && cnt != e->last - e->step) {
            e->acc = 0.f;
        }
        e->acc += c->v[0].f32[0];
        n = std::floor(e->acc);
        e->acc -= n;
        e->last = cnt;
        e->step = static_cast<uint32_t>(n);
    }
    c->v[0].f32[0] = n;
    c->v[1].f32[0] = 0.f;   // the game's fadd + fcvtzu give exactly n
    return 0;
}

// the game's own 60 Hz frame wait: off unless [pkp] game_frame_limit (the runtime's limiter paces to [display] fps_limit)
int hook_sync_period(::Ctx* c, uint64_t, void*) {
    State& s = st();
    s.sync_calls.fetch_add(1, std::memory_order_relaxed);
    if (!s.game_limit.load(std::memory_order_relaxed)) c->x[22] = 0;
    return 0;
}

// fixed-step frame time 1 / 60: frames shorter than 1/60 s get their measured rate, while the game runs above 60 fps
int hook_fixed_step_rate(::Ctx* c, uint64_t, void*) {
    State& s = st();
    s.fixed_calls.fetch_add(1, std::memory_order_relaxed);
    if (!s.fps_fix.load(std::memory_order_relaxed) || !s.above60.load(std::memory_order_relaxed) || s.holding) return 0;   // #275: see hook_frame_multiplier
    const uint64_t now = c->x[28], prev = c->x[25];
    const double freq = c->v[8].f64[0];
    if (now <= prev || freq <= 0) return 0;
    const double rate = freq / static_cast<double>(now - prev);
    if (rate > 60.0 / (1.0 - 0.0015152 * 60.0)) c->v[0].f32[0] = static_cast<float>(std::min(rate, 480.0));
    return 0;
}

}  // namespace nxrt::games::pkp::play_detail
