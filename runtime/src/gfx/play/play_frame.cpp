// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the frame-boundary hook (update_hook), the keep-alive pump and the on-screen keyboard service.
#include "play_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

// Keep-alive pump: the window's events are normally pumped at the frame boundary. While the game boots or loads it can go ten seconds and
// more without one, and Windows then calls the window "Not Responding". busy_pump runs before every SVC (svc_set_busy_hook); on the
// thread that owns the window (the kernel runs it on that thread only) it pumps the events when the last pump is 100 ms old.
std::atomic<uint64_t> g_last_pump_ms{0};
void busy_pump() {
    keyboard_service();
    const uint64_t t = SDL_GetTicks();
    if (t - g_last_pump_ms.load(std::memory_order_relaxed) < 100) return;
    g_last_pump_ms.store(t, std::memory_order_relaxed);
    platform::pump_events();
}

// Master volume (live). The mute while the window has no focus is set every frame (update_hook) and on focus changes.
void apply_audio(const AudioSettings& a) { audio::set_master_volume(static_cast<float>(a.volume)); }

void apply_unfocused_mute() {
    G& st = g();
    audio::set_muted(st.window && st.settings.audio.mute_unfocused && !platform::focused());
}

// ---- hooks ----------------------------------------------------------------------------------------------------------------------------
void pre_frame(void*) {
    // On the presenting thread, before the frame: the one place a present-mode change cannot collide with a frame in flight.
    G& st = g();
    const int want = st.want_present.exchange(-1);
    if (st.renderer && want >= 0) st.renderer->set_present_mode(static_cast<gfx::PresentMode>(want));
    if (st.overlay) st.overlay->apply_present_pending();
}

// set while update_hook runs: keyboard_service stays out of it
thread_local bool t_in_update = false;

int update_hook(::Ctx*, uint64_t, void*) {
    G& st = g();
    if (st.device && st.device->device_lost()) {
        const game::Info& gi = game::info();
        const std::string name = *gi.short_title ? gi.short_title : gi.title;     // "Pokopia"
        const std::string message = "The graphics device was lost, so " + name + " must close. Restart the game and attach " + gi.id + ".log if the problem repeats.";
        if (st.window) MessageBoxA(static_cast<HWND>(platform::native_hwnd()), message.c_str(), (name + " - graphics device lost").c_str(), MB_OK | MB_ICONERROR);
        exit_process_impl(2, "fail", message, false);
    }
    ft::mark("main.update");
    struct InUpdate {
        InUpdate() { t_in_update = true; }
        ~InUpdate() { t_in_update = false; }
    } in_update;
    // frame pacing (S18): the limiter waits here, before the input is sampled, so the picture shown is the one simulated one render time
    // earlier. The wait for frames still in flight comes first, then the limiter's schedule.
    // All of it runs once per presented frame: the game's main loop runs more iterations than it presents frames (Pokopia ~79 against ~50 a
    // second), and per iteration the limiter took several slots per frame while the in-flight list (one id per sim_begin, one popped per
    // present) filled up until wait_in_flight held the main thread for presents that only come after more main-loop work.
    const bool frame_start = extras::pace_boundary_due();
    if (frame_start) {
        extras::LowLatency* ll = extras::lowlat();
        if (extras::pace_in_main()) {
            if (ll) ll->wait_in_flight();
            // With Reflex on and a frame cap, the Reflex sleep that follows is the limiter (minimum interval between simulation starts)
            if (!(ll && ll->paces_frames())) { NXFT("limiter.main"); st.limiter.begin_frame_nohook(); }
            extras::pace_boundary_done();
        }
        // low latency (S14): the sleep that lets the frame start as late as the GPU allows, then the input sample marker; the events are pumped right after
        if (ll) {
            ll->sim_begin();
            ll->input_sample();
        }
    }
    const uint64_t now = now_ns();
    st.frames++;
    st.update_seq++;

    const bool on_pump_thread = std::this_thread::get_id() == st.pump_thread;
    if (on_pump_thread) g_last_pump_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    if (!on_pump_thread && !st.warned_thread) {
        st.warned_thread = true;
        st.thread_mismatch.store(true);
        NXLOG_W(kTag, "the frame-boundary hook runs on another thread than the one that started play mode: SDL events are not pumped");
    }
    if (st.window && on_pump_thread) platform::pump_events();

    // settings queued by the page
    {
        std::unique_lock<std::mutex> lk(st.mu);
        if (st.have_pending) {
            Settings s = st.pending;
            st.have_pending = false;
            const std::string old_mode = st.settings.display.window_mode;
            const int old_monitor = st.settings.display.monitor;
            const int old_w = st.settings.display.width, old_h = st.settings.display.height;
            st.settings = s;
            st.settings_epoch++;
            lk.unlock();
            st.engine->set_settings(s, now);
            apply_frame_cap(s);
            display_tuning(s.display);
            if (const GamePlay* gp = game_play(); gp && gp->settings_changed) gp->settings_changed(s);     // the game's display options, lock-on
            extras::set_settings(s.extras);
            extras::hdr_refresh_levels();
            apply_fps_counter(s.fps);
            st.want_present.store(static_cast<int>(present_mode_for(s.display)));
            apply_audio(s.audio);
            if (st.window && s.display.monitor != old_monitor && s.display.monitor > 0) {
                if (platform::move_to_display(s.display.monitor)) st.limiter.reset_schedule();
                apply_frame_cap(s);                        // another display can mean another refresh rate
            }
            if (s.display.window_mode != old_mode) {
                set_window_mode(s.display.window_mode);
                apply_frame_cap(s);                        // another window mode can mean another refresh rate (vsync cap)
            }
            // The windowed size is live while the window is windowed; in fullscreen it is the size the window goes back to (the next start).
            if (st.window && (s.display.width != old_w || s.display.height != old_h) && platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
                platform::set_window_size(s.display.width, s.display.height);
            std::string err;
            if (!save_settings(st.path, s, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
            else NXLOG_I(kTag, "settings applied and saved to %s", st.path.c_str());
        }
    }

    apply_unfocused_mute();

    // events
    input::Event buf[128];
    for (;;) {
        const size_t got = input::queue_drain(buf, 128);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) handle_event(buf[i], now);
    }
    poll_pads(now);
    // Quit was chosen in the overlay's prompt (keyboard, pad or mouse; the mouse answers on the presenting thread): the process ends here,
    // on the main thread, the way the window's close button does it.
    if (st.overlay && st.overlay->take_quit_confirmed()) quit_now("quit from the menu");
    // Settings was chosen in the same prompt: the F1 page opens here, on the main thread, as the F1 key does
    if (st.overlay && st.overlay->take_settings_wanted()) st.overlay->set_visible(true);
    const GamePlay* gp = game_play();
    if (gp && gp->frame_begin) gp->frame_begin();
    if (st.engine) st.engine->frame_tick();

    // what the game is doing: the context, the pointer
    const uint64_t pi = st.last_pi_ns.load();
    // "In game" = the game called ProcessInput during one of the last few frame boundaries. Counted in frames, not host time: a slow frame (the game
    // can take 100 ms or more per frame under load) must not read as a menu.
    // Also within 100 ms of host time: a catch-up burst of frame boundaries with no input call (1-2 frames about once a second at ~50 fps)
    // dropped the context to the menu and released the mouse for a moment.
    const bool in_game = pi != 0 && (st.frames.load() - st.last_pi_frame.load() <= 4 || pi + 100000000ull >= now);
    // The game's part (play_game.h GamePlay::frame): the gyro's pitch-rate scale for this step, the context while in game. This
    // step's length is known here, before the game reads the sample (the timestep hook on the same frame update runs first: hooks of one address run
    // in registration order).
    // One of the game's menus over play (bag, shop, choice box: GamePlay::menu_in_game): the frame is a menu frame for the context, the
    // pointer and the mouse capture (with the menu pointer off, play stays play).
    const bool game_menu = gp && gp->menu_in_game && gp->menu_in_game(in_game && st.settings.mouse.menu_cursor) && in_game;
    const bool playing = in_game && !game_menu;
    GameFrame gf;
    if (gp && gp->frame) {
        // Without a [time] step the step is the time since the previous frame update, i.e. what the frame limiter's wait above let pass
        // (1/60 s for the first one; held to 1 ms .. 100 ms so a stall or a burst is not taken for a step).
        static uint64_t last_update_ns = 0;
        const double measured = last_update_ns ? std::clamp(static_cast<double>(now - last_update_ns) * 1e-9, 0.001, 0.1) : static_cast<double>(timestep::kGameStep);
        last_update_ns = now;
        const double step = timestep::active() ? static_cast<double>(timestep::current_dt()) : measured;
        gp->frame(playing, step, &gf);
    }
    if (st.source_owner) st.source_owner->set_look_scale(gf.look_scale);
    Ctx ctx;
    if (!in_game) ctx = Ctx::Menu;
    else if (game_menu) ctx = Ctx::Menu;
    else ctx = gf.play_ctx;
    st.engine->set_context(ctx, now);

    {   // button prompts: the style that shows, and the icon textures behind it
        PromptInputs pin;
        pin.requested = st.settings.prompts.style;
        pin.label_abxy = st.settings.pad.label_abxy;
        pin.ctx = ctx;
        pin.settings_epoch = st.settings_epoch;
        pin.last = st.last_dev;
        pin.last_pad_kind = st.last_pad_kind;
        input::GamepadInfo pads[1];
        pin.connected_pad_kind = input::gamepad_list(pads, 1) > 0 ? static_cast<int>(pads[0].kind) : -1;
        prompts_update(st.settings, pin);
    }

    const bool overlay_visible = st.overlay && st.overlay->blocks_game();
    if (overlay_visible && !st.overlay_was_visible) st.engine->release_all(now);
    st.overlay_was_visible = overlay_visible;

    if (st.window) {
        platform::set_text_input(st.overlay && st.overlay->keyboard_open());   // typed text for the on-screen keyboard only
        const bool want = st.capture_mouse && platform::focused() && !platform::minimized() && !overlay_visible && playing &&
                          st.capture_state.load() == 0 && !hid::jc2_mouse_wants_pointer();
        if (want != platform::relative_mouse()) {
            platform::set_relative_mouse(want);
            if (want) input::mouse_delta_take();      // nothing that piled up before the capture
        }
    }
    {   // the map: dragging with the left button rotates it (the left stick), with the middle button moves it (the right stick); the speed is the pointer's speed
        static float px = 0, py = 0;
        static uint64_t last_ns = 0;
        static bool have = false;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        mm::Pointer& pp = mm::pointer();
        const bool on_map = st.window && ctx == Ctx::Map && st.settings.mouse.menu_cursor && pp.enabled() && (platform::focused() || tt::enabled()) && !overlay_visible;
        if (on_map && have && (pp.held() || pp.mid_held())) {
            int ww = 1, wh = 1;
            platform::pixel_size(&ww, &wh);
            // the stick follows the pointer's speed: a full stick is a drag of 0.8 window heights per second, whatever the frame rate
            const float dt = std::max(1e-3f, static_cast<float>(now - last_ns) / 1e9f);
            const float k = 1.f / (0.8f * static_cast<float>(std::max(1, wh)) * dt);
            const float dx = std::max(-1.f, std::min(1.f, (pp.x() - px) * k)), dy = std::max(-1.f, std::min(1.f, (pp.y() - py) * k));
            if (pp.held()) { lx = dx; ly = -dy; }
            else { rx = dx; ry = -dy; }
        }
        px = pp.x();
        py = pp.y();
        last_ns = now;
        have = true;
        st.engine->set_aux_sticks(lx, ly, rx, ry);
    }
    {   // the mouse as a pointer in the menus (S28): the cursor, and the snapshot the widget hooks read this frame
        mm::Pointer& ptr = mm::pointer();
        ptr.set_enabled(st.settings.mouse.menu_cursor);
        // (a scripted run has no focus: the test tooling drives the pointer with synthetic events)
        const bool menu_up = st.window && !playing && ctx == Ctx::Menu && (platform::focused() || tt::enabled()) && !platform::minimized() && !overlay_visible;
        {   // why the menu pointer is or is not up, on each change: always in test runs, otherwise at most 24 lines a session, so a report
            // that the mouse does nothing in menus says which part of the gate was closed
            static int last = -1, lines = 0;
            const int sig = (st.window ? 1 : 0) | (playing ? 2 : 0) | (static_cast<int>(ctx) << 2) | (overlay_visible ? 256 : 0) |
                            (platform::focused() ? 512 : 0) | (platform::minimized() ? 1024 : 0);
            if (sig != last && (tt::enabled() || lines < 24)) {
                last = sig;
                ++lines;
                NXLOG_I("play", "menu pointer gate: window %d playing %d ctx %d (menu %d) minimized %d overlay %d -> %d (focused %d, [mouse] menu_cursor %d)",
                        st.window ? 1 : 0, playing ? 1 : 0, static_cast<int>(ctx), static_cast<int>(Ctx::Menu), platform::minimized() ? 1 : 0,
                        overlay_visible ? 1 : 0, menu_up ? 1 : 0, platform::focused() ? 1 : 0, st.settings.mouse.menu_cursor ? 1 : 0);
            } else if (sig != last) {
                last = sig;
            }
        }
        mm::Geometry geo;
        platform::pixel_size(&geo.win_w, &geo.win_h);
        const DisplayLive dl = gp && gp->display_live ? gp->display_live() : DisplayLive{};
        geo.render_w = dl.render_w;
        geo.render_h = dl.render_h;
        geo.display_aspect = dl.aspect > 0 ? dl.aspect : 0;
        geo.keep_aspect = st.settings.display.keep_aspect;
        if (st.overlay) {   // the game's overlay layer covers the picture (letterbox bars left out)
            const mm::Rect r = mm::picture_rect(geo);
            const double ww = std::max(1, geo.win_w), wh = std::max(1, geo.win_h);
            st.overlay->set_picture_rect(static_cast<float>(r.x / ww), static_cast<float>(r.y / wh), static_cast<float>(r.w / ww), static_cast<float>(r.h / wh));
        }
        if (const Act a = gp && gp->menu_frame ? gp->menu_frame(menu_up, geo) : Act::None; a != Act::None) st.engine->pulse(a, now);
        if (st.overlay && st.pointer_draw) st.overlay->set_pointer(menu_up && ptr.enabled() && ptr.cursor_visible(menu_up), ptr.x(), ptr.y(), true);
        // In play with the Joy-Con 2 mouse on its surface the game draws its own cursor: the OS cursor is hidden (unless the overlay is up).
        const bool game_cursor = playing && !overlay_visible && hid::jc2_mouse_active();
        if (st.window && !platform::relative_mouse())
            platform::set_cursor_visible(!game_cursor && (overlay_visible || !(ptr.enabled() && menu_up) || ptr.cursor_visible(menu_up)));
        if (st.window) {   // the Joy-Con 2 mouse (hid): the pointer on the picture, the buttons and the wheel
            hid::HostMouse hm;
            bool inside = false;
            mm::window_to_picture(geo, st.mouse_x, st.mouse_y, &hm.u, &hm.v, &inside);
            hm.buttons = !overlay_visible && jc2_owns_clicks() ? st.jc2_buttons : 0u;   // npad buttons only while they are the device's
            hm.wheel_x = st.jc2_wheel_x;
            hm.wheel_y = st.jc2_wheel_y;
            hm.focused = (platform::focused() || tt::enabled()) && !platform::minimized() && !overlay_visible;
            hm.captured = platform::relative_mouse();
            hid::jc2_mouse_feed(hm);
            st.jc2_wheel_x = st.jc2_wheel_y = 0;
        }
    }
    if (!playing) input::mouse_delta_take();         // aim only counts what arrives while the game is playing (not under one of its menus)

    if (st.log_status && now - st.last_status_ns > 1000 * kMs) {
        st.last_status_ns = now;
        GameStatus gs;
        if (gp && gp->status) gp->status(&gs);
        char game_part[512] = "";
        if (gp && gp->status_text) gp->status_text(game_part, sizeof game_part);
        NXLOG_I(kTag, "status: ctx=%s in_game=%d aim=%s frames=%llu process_input=%llu aim_frames=%llu events=%llu pads=%zu captured=%d%s", ctx_name(ctx),
                in_game ? 1 : 0, gs.aim, static_cast<unsigned long long>(st.frames.load()),
                static_cast<unsigned long long>(st.pi_calls.load()), static_cast<unsigned long long>(st.aim_frames.load()),
                static_cast<unsigned long long>(st.events.load()), input::gamepad_count(), platform::relative_mouse() ? 1 : 0, game_part);
    }
    if (st.selftest.active()) st.selftest.tick(st.renderer, &st.limiter, selftest_host());
    return 0;
}

// The software keyboard applet blocks the game: the thread that asked waits for the answer, the main thread soon waits too, and no frame is
// presented (Pokopia's name entry: the freeze watchdog fired 10 s after the question, frame count still). update_hook, which pumps the window
// and feeds the overlay, does not run then. keyboard_service does that part on the window's thread while it is blocked in the kernel
// (kernel::set_blocked_hook) or about to make an SVC (busy_pump), every 8 ms at most: the window's events and the pads go to the overlay's
// on-screen keyboard, the mouse is free, typing is on, and the game's last frame is presented again so the keyboard is drawn. Nothing
// happens while no question is open, or from inside update_hook.
void keyboard_service() {
    static thread_local uint64_t last_ms = 0;
    const uint64_t t = SDL_GetTicks();
    if (t - last_ms < 8 || t_in_update) return;
    last_ms = t;
    G& st = g();
    if (!st.started || !st.overlay || !st.overlay->keyboard_open()) return;
    const uint64_t now = now_ns();
    platform::pump_events();          // also headless (scripted runs): SDL's virtual pads update here
    if (st.window) {
        g_last_pump_ms.store(t, std::memory_order_relaxed);
        platform::set_text_input(true);
        if (platform::relative_mouse()) platform::set_relative_mouse(false);
        platform::set_cursor_visible(true);
    }
    input::Event buf[64];
    for (;;) {
        const size_t got = input::queue_drain(buf, 64);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) handle_event(buf[i], now);
    }
    poll_pads(now);
    if (st.overlay->take_quit_confirmed()) quit_now("quit from the menu");
    nvn::present_last_frame_again();
}

}  // namespace detail
}  // namespace nxrt::play
