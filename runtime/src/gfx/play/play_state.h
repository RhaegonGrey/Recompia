// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the glue (play.cpp's parts: play_frame, play_events, play_hosts, play_startup ...). The state they share and what they call in each other.
//
// Threads:
//   main (the guest main thread, which created the window)  the Update hook (frame boundary: event pump, input, settings changes) and the
//                                                            ProcessInput hook (raw mouse aim) run here
//   presenting thread (the game's render thread)             FrameLimiter::begin_frame (the pacing wait, and the pre-frame hook that changes the
//                                                            present mode), the ImGui overlay and the settings page
//   hid sampling thread                                      PlaySource::poll (200 Hz): reads the pad snapshot and asks the binding engine for the
//                                                            keyboard and mouse contribution at that instant
#pragma once
#include "nxrt/extras.h"
#include "nxrt/lowlat.h"
#include "nxrt/play.h"

#include <windows.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/ftrace.h"
#include "nxrt/game.h"
#include "play_display.h"
#include "nxrt/kernel.h"
#include "nxrt/svc.h"
#include "nxrt/config.h"
#include "nxrt/host_device.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input.h"
#include "nxrt/services_api.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "nxrt/runtime.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"
#include "menu_mouse.h"
#include "touch_route.h"
#include "play_engine.h"
#include "play_game.h"
#include "play_selftest.h"
#include "play_types.h"
#include "play_vpad.h"
#include "play_ui.h"
#include "prompts.h"
#include "pad_map.h"
#include "pad_orient.h"
#include "play_input_source.h"

namespace nxrt::play {

[[noreturn]] void exit_process_impl(int code, const char* verdict, const std::string& message, bool flush_gpu_cache);   // play_startup.cpp

namespace detail {

inline constexpr const char* kTag = "play";
inline constexpr uint64_t kMs = 1000000ull;

// ---- the state ------------------------------------------------------------------------------------------------------------------------
struct G {
    // settings
    std::mutex mu;                       // guards settings (written on the main thread, copied by the page on the presenting thread) and pending
    Settings settings;
    Settings pending;
    bool have_pending = false;
    bool dirty = false;                  // settings changed in the game (Alt+Enter): written at stop
    std::string path;
    bool loaded = false, started = false, load_ok = true;

    FrameLimiter limiter;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<PlaySource> source_owner;
    std::shared_ptr<hid::InputSource> source;
    std::unique_ptr<gfx::Overlay> overlay;
    gfx::Renderer* renderer = nullptr;
    gfx::Device* device = nullptr;
    bool window = false;

    std::thread::id pump_thread;
    uint64_t base = 0;                   // set by load: [play] game_base, default the game's Info::main_base
    uint32_t hook_update = 0;            // the frame boundary (the game's GamePlay::frame_update)
    bool capture_mouse = true, quit_on_close = true, log_status = false;
    bool pointer_draw = false;         // [play] pointer_draw: the menu pointer is also drawn into the frame (screenshots show it); a test aid
    bool late_sample = true;           // pump the window's events again right before the mouse counts are taken (S11: the counts are as new as ProcessInput)
    bool overlay_combo_held = false;   // poll_pads: the [overlay] pad combination was held at the last poll (the menu toggles on the rising edge)
    bool quit_combo_held = false;      // poll_pads: the [quit] pad combination was held on a pad at the last poll (the prompt opens on the rising edge)

    // frame state (main thread)
    uint64_t update_seq = 0, aim_seq = 0;
    std::atomic<uint64_t> last_pi_ns{0};
    std::atomic<uint64_t> last_pi_frame{0};       // value of `frames` at the last ProcessInput (the game is playing while it keeps being called)
    bool overlay_was_visible = false;
    float mouse_x = 0, mouse_y = 0;
    bool mouse1 = false;
    uint32_t jc2_buttons = 0;            // host mouse buttons as nn::hid::MouseButton bits, for the Joy-Con 2 mouse (hid jc2_mouse.h)
    float jc2_wheel_x = 0, jc2_wheel_y = 0;   // wheel notches since the last feed
    uint64_t last_status_ns = 0;
    bool warned_thread = false;

    // button prompts: the device the player used last (Auto style) and a counter that changes with the bindings
    LastDevice last_dev = LastDevice::None;
    int last_pad_kind = 0;
    uint64_t settings_epoch = 1;

    // pads
    uint64_t last_rumble_ns = 0;
    uint16_t rumble_lo = 0, rumble_hi = 0;

    // present mode requested by the page, applied on the presenting thread
    std::atomic<int> want_present{-1};

    // vibration from the game (hid thread) -> pads (main thread)
    std::atomic<float> vib[2][2] = {};   // [hand][low, high] amplitude

    // rebinding
    std::atomic<int> capture_state{0};   // 0 idle, 1 waiting for an input
    std::mutex cap_mu;
    bool cap_have = false;
    InputId cap_id;

    // live status
    mutable std::mutex st_mu;
    LiveStatus live;
    uint64_t fps_last_frames = 0, fps_last_ns = 0;
    double fps = 0;

    SelfTest selftest;

    // counters
    std::atomic<uint64_t> frames{0}, pi_calls{0}, aim_frames{0}, events{0}, counts{0};
    std::atomic<bool> thread_mismatch{false};
};

G& g();                            // play_state.cpp

inline uint64_t now_ns() { return FrameLimiter::now_ns(); }

SelfTestHost* selftest_host();      // play_automation.cpp

// play_frame_cap.cpp
gfx::PresentMode present_mode_for(const DisplaySettings& d);
int frame_snap_k(const DisplaySettings& d, double cap, float hz, double* snapped, const char** reason);
void apply_frame_cap(const Settings& s);

// play_window.cpp
const char* window_mode_name(platform::FullscreenMode m);
void set_window_mode(const std::string& mode);
void toggle_fullscreen();
void apply_fps_counter(const FpsSettings& f);

// play_events.cpp
void vib_sink(uint32_t slot, int hand, const hid::VibrationValue& v, void*);
void handle_event(const input::Event& e, uint64_t now);
bool jc2_owns_clicks();    // the Joy-Con 2 mouse is in use in play: the left and right buttons are its own, not the bindings'
void poll_pads(uint64_t now);

// play_frame.cpp
extern std::atomic<uint64_t> g_last_pump_ms;
void busy_pump();
void keyboard_service();
void pre_frame(void*);
int update_hook(::Ctx*, uint64_t, void*);

// play_automation.cpp
bool key_sink(const char* name, bool down);
int key_scancode_by_name(const char* name);   // play_events.cpp: for nxrt/key_state.h
bool screenshot_provider(const char* path, bool bare, void*);   // nxrt/screenshot.h: registered at start whenever there is a renderer

// play_hosts.cpp
GameHost& game_host();
UiHost& ui_host();
void toggle_fps_counter();

// play_startup.cpp
[[noreturn]] void quit_now(const char* why);

}  // namespace detail
}  // namespace nxrt::play
