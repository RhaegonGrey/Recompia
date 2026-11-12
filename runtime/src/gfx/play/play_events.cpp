// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: input event routing (handle_event), pad polling and rumble.
#include "play_state.h"
#include "nxrt/key_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

// SDL mouse button (1 left, 2 middle, 3 right, 4 X1, 5 X2) to nn::hid::MouseButton (Left 0, Right 1, Middle 2, Forward 3, Back 4).
uint32_t jc2_button_bit(uint8_t b) {
    switch (b) {
        case 1: return 1u << 0;
        case 3: return 1u << 1;
        case 2: return 1u << 2;
        case 5: return 1u << 3;
        case 4: return 1u << 4;
        default: return 0;
    }
}

// nxrt/key_state.h's name lookup: key settings store SDL's scancode names (parse_input also takes any case); a mouse input is not a key.
int key_scancode_by_name(const char* name) {
    InputId id;
    return name && parse_input(name, &id) && id.kind == InKind::Key ? id.code : -1;
}

void vib_sink(uint32_t slot, int hand, const hid::VibrationValue& v, void*) {
    if (slot != 0 || hand < 0 || hand > 1) return;
    G& st = g();
    st.vib[hand][0].store(v.amp_low);
    st.vib[hand][1].store(v.amp_high);
}

// The pointer is over a slider's track in the game's menu (its menu hooks are active this frame): the wheel steps the value.
bool game_menu_over_value() {
    const GamePlay* gp = game_play();
    return gp && gp->menu_active && gp->menu_over_value && gp->menu_active() && gp->menu_over_value();
}

// The Joy-Con 2 mouse is asked for (mode on, or a request) and on its surface while the game is in play (menus and the map keep their
// bindings and the menu pointer). Its left and right buttons are then the Joy-Con's own buttons (hid_jc2_mouse.cpp jc2_pad_buttons), so
// the bindings must not also send theirs. In auto mode without a request (the mouse merely not captured) clicks stay the bindings'.
bool jc2_owns_clicks() {
    G& st = g();
    return st.engine && ctx_in_play(st.engine->context()) && hid::jc2_mouse_wants_pointer() && hid::jc2_mouse_active();
}

void handle_event(const input::Event& e, uint64_t now) {
    G& st = g();
    using input::EventType;
    st.events++;
    if (e.type == EventType::KeyUp) key_state::set(e.u.key.scancode, false);   // before any early return below: a release is never lost
    // rebinding capture comes first: the page asked for the next input
    if (st.capture_state.load() == 1) {
        InputId id;
        bool got = false, cancel = false;
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) {
            if (e.u.key.scancode == SDL_SCANCODE_ESCAPE) cancel = true;
            else { id = {InKind::Key, e.u.key.scancode}; got = true; }
        } else if (e.type == EventType::MouseButtonDown) {
            id = {InKind::Mouse, e.u.button.button}; got = true;
        } else if (e.type == EventType::MouseWheel && e.u.wheel.dy != 0) {
            id = {InKind::Wheel, e.u.wheel.dy > 0 ? 1 : -1}; got = true;
        }
        if (got) {
            std::lock_guard<std::mutex> lk(st.cap_mu);
            st.cap_id = id;
            st.cap_have = true;
            st.capture_state.store(0);
            return;
        }
        if (cancel) { st.capture_state.store(0); return; }
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) return;    // nothing reaches the game while capturing
    }
    // The frame rate counter hotkey: flips the counter and never reaches the game (press, repeat and release).
    if (st.overlay && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && st.settings.fps.key.kind == InKind::Key &&
        e.u.key.scancode == st.settings.fps.key.code) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle_fps_counter();
        return;
    }
    // The quit key: opens the "Close the game?" prompt and never reaches the game (press, repeat and release). Not while the on-screen
    // keyboard is up: a letter key must type there.
    if (st.overlay && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && st.settings.quit.key.kind == InKind::Key &&
        e.u.key.scancode == st.settings.quit.key.code && !st.overlay->keyboard_open()) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) st.overlay->open_quit_prompt();
        return;
    }
    if (st.overlay && st.overlay->handle_event(e)) return;
    const bool menu_blocked = st.overlay && st.overlay->blocks_game();
    // a finger is not the keyboard and mouse: the prompts stay as they are (touch_route.h)
    const MouseRoute touch_route = route_mouse(e.type == EventType::MouseButtonDown || e.type == EventType::MouseButtonUp || e.type == EventType::MouseMotion ? e.flags : 0);
    if ((e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) || (e.type == EventType::MouseButtonDown && touch_route.sets_last_device) || e.type == EventType::MouseWheel)
        st.last_dev = LastDevice::KeyboardMouse;
    switch (e.type) {
        case EventType::Quit:
            if (st.quit_on_close) quit_now("window closed");
            break;
        case EventType::KeyDown:
            if ((e.u.key.scancode == SDL_SCANCODE_RETURN || e.u.key.scancode == SDL_SCANCODE_KP_ENTER) && (e.mods & SDL_KMOD_ALT)) {
                if (!(e.flags & input::kFlagRepeat)) toggle_fullscreen();
                return;
            }
            if (!(e.flags & input::kFlagRepeat)) mm::pointer().on_other_input();
            if (!(e.flags & input::kFlagRepeat) && !menu_blocked) {
                st.engine->key(e.u.key.scancode, true, now);
                key_state::set(e.u.key.scancode, true);   // the keys the game gets, so a mod does not see a key typed into a menu
            }
            break;
        case EventType::KeyUp:
            st.engine->key(e.u.key.scancode, false, now);
            break;
        case EventType::MouseButtonDown:
            if (e.u.button.button == 1) st.mouse1 = true;
            st.jc2_buttons |= jc2_button_bit(e.u.button.button);
            if (!menu_blocked) mm::pointer().on_button(e.u.button.button, true);
            // While the Joy-Con 2 mouse owns the mouse in play, the left and right buttons belong to the device alone (the middle button stays
            // the mouse-mode key). A release is always passed on: it only matters when the press was seen before the device took over.
            if (!menu_blocked && touch_route.game_click && !(jc2_owns_clicks() && (e.u.button.button == 1 || e.u.button.button == 3))) st.engine->mouse_button(e.u.button.button, true, now);
            break;
        case EventType::MouseButtonUp:
            if (e.u.button.button == 1) st.mouse1 = false;
            st.jc2_buttons &= ~jc2_button_bit(e.u.button.button);
            mm::pointer().on_button(e.u.button.button, false);
            if (touch_route.game_click) st.engine->mouse_button(e.u.button.button, false, now);   // a finger's press never reached the engine, so neither does its release
            break;
        case EventType::MouseWheel: {
            if (menu_blocked) break;
            st.jc2_wheel_x += e.u.wheel.dx;
            st.jc2_wheel_y += e.u.wheel.dy;
            if (jc2_owns_clicks()) break;           // the wheel goes to the Joy-Con 2 mouse only
            const float dy = e.u.wheel.dy;
            // the map: the wheel zooms (the L and R buttons), a longer pulse than a button tap so one notch is a visible step
            if (dy != 0 && st.settings.mouse.menu_cursor && st.engine->context() == Ctx::Map && !(st.overlay && st.overlay->visible())) {
                st.engine->pulse(dy > 0 ? Act::BtnR : Act::BtnL, now, 160);
                break;
            }
            // over a slider's track in a menu the wheel steps the value (right is more) instead of moving the selection
            if (dy != 0 && st.settings.mouse.menu_cursor && mm::pointer().enabled() && game_menu_over_value()) {
                st.engine->pulse(dy > 0 ? Act::DpadRight : Act::DpadLeft, now);
                break;
            }
            const int n = std::max(1, static_cast<int>(std::lround(std::fabs(dy))));
            if (dy != 0) for (int i = 0; i < n; ++i) st.engine->wheel(dy > 0 ? 1 : -1, now);
            break;
        }
        case EventType::MouseMotion:
            if (!(e.flags & input::kFlagRelative)) {
                st.mouse_x = e.u.motion.x;
                st.mouse_y = e.u.motion.y;
                if (!menu_blocked) mm::pointer().on_motion(e.u.motion.x, e.u.motion.y);
                // moving the pointer over a menu counts as using the mouse (the prompts switch to keyboard icons); the raw motion of the aim in play does not
                if (!menu_blocked && st.settings.mouse.menu_cursor && mm::pointer().has_moved() && touch_route.sets_last_device) st.last_dev = LastDevice::KeyboardMouse;
            }
            break;
        case EventType::WindowFocusLost:
            audio::set_muted(st.window && st.settings.audio.mute_unfocused);
            st.engine->release_all(now);
            key_state::clear();
            st.mouse1 = false;
            st.jc2_buttons = 0;
            mm::pointer().on_focus_lost();
            break;
        case EventType::WindowFocusGained:
            audio::set_muted(false);
            st.limiter.reset_schedule();
            break;
        case EventType::WindowRestored:
        case EventType::WindowMinimized:
            st.limiter.reset_schedule();
            break;
        case EventType::GamepadAdded:
            NXLOG_I(kTag, "controller %u connected", static_cast<unsigned>(e.u.gamepad.id));
            // a pad that replaces the one in use: the prompts follow its kind at once (without it they change at the first press)
            if (st.last_dev == LastDevice::Pad) st.last_pad_kind = static_cast<int>(input::gamepad_kind_of(e.u.gamepad.id));
            break;
        case EventType::GamepadRemoved:
            NXLOG_I(kTag, "controller %u disconnected", static_cast<unsigned>(e.u.gamepad.id));
            break;
        default: break;
    }
}

// Shortest gap between two rumble writes to the pads (30 per second).
constexpr uint64_t kRumbleMinGapNs = 1000000000ull / 30;

// Reads every open pad, merges them (buttons OR, the stick with the larger deflection wins) and publishes the result to the source.
void poll_pads(uint64_t now) {
    G& st = g();
    PadFrame pf;
    input::GamepadInfo infos[8];
    const size_t n = input::gamepad_list(infos, 8);
    bool have_six = false;
    uint16_t lo = 0, hi = 0;
    {
        const float l = std::max(st.vib[0][0].load(), st.vib[1][0].load()), h = std::max(st.vib[0][1].load(), st.vib[1][1].load());
        map_rumble(l, h, st.settings.pad, &lo, &hi);
    }
    const Ctx cx = st.engine ? st.engine->context() : Ctx::Menu;
    const bool look_ctx = cx == Ctx::Play;      // the right stick is a look only in play
    // Rumble goes to the pads at most 30 times a second, whatever the frame rate. The game sets its vibration every frame, and while it
    // rumbles (vacuuming, breaking, rolling) the value changes nearly every frame: at 100+ fps one write per change floods a driver that
    // forwards each write over Bluetooth (BetterJoy's virtual pad stops reading the controller and drops stick and button input, #374).
    // A change waits for the next slot; the newest value is the one sent.
    const bool rumble_slot = now - st.last_rumble_ns >= kRumbleMinGapNs;
    const bool rumble_changed = rumble_slot && (lo != st.rumble_lo || hi != st.rumble_hi);
    const bool rumble_refresh = (lo || hi) && now - st.last_rumble_ns > 100 * kMs;
    const uint32_t quit_combo = st.settings.quit.pad;
    bool quit_held = false, overlay_held = false;
    // [overlay] pad: the same combination as the quit one would open two things at once, so it is ignored (warned about once per pair)
    uint32_t overlay_combo = st.settings.overlay.pad;
    if (overlay_combo && overlay_combo == quit_combo) {
        static uint32_t warned = 0;
        if (warned != overlay_combo) {
            warned = overlay_combo;
            NXLOG_W(kTag, "[overlay] pad is the same combination as [quit] pad (%s): ignored, pick another", pad_combo_text(overlay_combo).c_str());
        }
        overlay_combo = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        platform::GamepadRaw raw;
        if (!platform::gamepad_poll(infos[i].id, &raw)) continue;
        // the quit combination: every button of it held on this one pad (physical buttons, before [padmap])
        if (quit_combo && (pad_src_held(raw, st.settings.pad) & quit_combo) == quit_combo) quit_held = true;
        if (overlay_combo && (pad_src_held(raw, st.settings.pad) & overlay_combo) == overlay_combo) overlay_held = true;
        hid::NpadInput tmp;
        map_pad(raw, st.settings.pad, &tmp);
        {   // a pressed button or a stick well off centre makes this the pad the prompts follow
            auto off_centre = [](hid::StickValue v) { return static_cast<int64_t>(v.x) * v.x + static_cast<int64_t>(v.y) * v.y > 20000ll * 20000ll; };
            if (tmp.buttons != 0 || off_centre(tmp.left) || off_centre(tmp.right)) {
                st.last_dev = LastDevice::Pad;
                st.last_pad_kind = static_cast<int>(infos[i].kind);
                mm::pointer().on_other_input();
            }
        }
        if (look_ctx) tmp.right = adjust_look_stick(tmp.right, st.settings.pad);
        pf.buttons |= tmp.buttons;
        auto big = [](hid::StickValue a, hid::StickValue b) {
            return static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.y) * a.y >= static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.y) * b.y ? a : b;
        };
        pf.left = big(pf.left, tmp.left);
        pf.right = big(pf.right, tmp.right);
        if (!have_six) {
            float a[3], gy[3];
            if (platform::gamepad_sensors(infos[i].id, a, gy)) {
                map_sensors(a, gy, st.settings.pad, &pf.six);
                have_six = true;
            }
        }
        if (rumble_changed || rumble_refresh) platform::gamepad_rumble(infos[i].id, lo, hi, 250);
    }
    if (rumble_changed || rumble_refresh) {
        st.rumble_lo = lo;
        st.rumble_hi = hi;
        st.last_rumble_ns = now;
    }
    // The quit prompt opens on the rising edge of the combination. It opens before the overlay sees this poll, so the combination's buttons
    // are held back from now on (set_keyboard_pad below), and after the prompt closes until they are released. A button of it pressed alone a
    // poll earlier did reach the game.
    if (st.overlay && quit_held && !st.quit_combo_held) st.overlay->open_quit_prompt();
    st.quit_combo_held = quit_held;
    // The overlay menu toggles on the rising edge of its combination. Its buttons are held back from the game until released, so closing the
    // menu with them does not press them in the game.
    if (st.overlay && overlay_held && !st.overlay_combo_held && !st.overlay->quit_prompt_open() && !st.overlay->keyboard_open()) st.overlay->toggle();
    st.overlay_combo_held = overlay_held;
    if (overlay_held) pf.buttons = 0;
    // The on-screen keyboard and the quit prompt read the merged pad before it is held back; what it returns stays away from the game (everything while it is
    // open, then the button that closed it until it is released).
    if (st.overlay) {
        st.overlay->set_keyboard_abxy(st.settings.pad.label_abxy);
        pf.buttons &= ~st.overlay->set_keyboard_pad(pf.buttons, pf.left.x, pf.left.y);
    }
    // The settings menu or the on-screen keyboard is open: the pad does not reach the game (the keyboard and mouse are held back the same way in
    // handle_event).
    if (st.overlay && st.overlay->blocks_game()) {
        pf.buttons = 0;
        pf.left = hid::StickValue{};
        pf.right = hid::StickValue{};
    }
    st.source_owner->set_pad(pf);
}

}  // namespace detail
}  // namespace nxrt::play
