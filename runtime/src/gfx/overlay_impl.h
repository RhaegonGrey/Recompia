// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay (overlay*.cpp): Overlay::Impl, the state of its features (menu panels, touch button, frame rate counter, on-screen keyboard,
// quit prompt, diagnostics window, the game's layer) in one struct, and what the files share. Not for use outside overlay*.cpp.
#pragma once
#include <functional>

#include "nxrt/overlay.h"

#include <atomic>
#include <mutex>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "nxrt/config.h"
#include "nxrt/host_device.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/nvn.h"
#include "nxrt/platform.h"
#include "nxrt/services_api.h"
#include "extras/overlay_composite.h"
#include "fps_stats.h"

namespace nxrt::gfx {
namespace overlay_detail {

inline constexpr const char* kTag = "overlay";

// The grid: rows of 10 column units, a key spans `w` of them. Shift turns letters upper case and punctuation into its second character (alt).
enum class KbOp : uint8_t { Char, Shift, Space, Back, Ok, Cancel };
struct KbKey { char32_t ch; char32_t alt; int w; KbOp op; };
constexpr int kKbUnits = 10;

void clear_imgui_input(ImGuiContext* ctx);   // overlay.cpp

}  // namespace overlay_detail

using overlay_detail::KbKey;

struct Overlay::Impl {
    Context ctx;
    ImGuiContext* imgui = nullptr;
    ImGuiStyle base_style;            // the style at scale 1; style = base_style scaled by ui_scale (update_scale)
    float ui_scale = 0.f;             // 0: not applied yet
    bool backend_ready = false;
    std::atomic<bool> visible{false};
    std::atomic<bool> touch_btn{false};      // set_touch_button: the corner button and finger-sized controls
    std::atomic<bool> touch_toggle_req{false};   // the ImGui button was tapped while the menu was open: apply_pending toggles outside the frame
    std::atomic<float> disp_w{0.f}, disp_h{0.f};   // the frame size of the last drawn frame (the button's place, for hit tests off the presenting thread)
    bool touch_armed = false;                // a finger went down on the corner button and has not come up yet (main thread)
    bool scaled_touch = false;               // the touch state the style was scaled for
    ImVec4 touch_rect() const;               // x, y, w, h of the corner button in frame pixels
    bool touch_button_event(const input::Event& e);   // the hidden menu's corner button; true = the event is the button's
    void draw_touch_button(bool open);       // inside an ImGui frame
    std::mutex mu;                    // guards the ImGui context: events arrive on the game's main thread, drawing happens on the presenting thread
    int toggle_sc = SDL_SCANCODE_F1;
    bool restore_relative = false;
    std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    uint64_t drawn = 0;

    struct Panel { std::string title; std::function<void()> fn; };
    std::vector<Panel> panels;

    // menu state applied by apply_pending()
    bool want_mode = false;
    PresentMode new_mode = PresentMode::Fifo;
    bool want_fullscreen = false;
    platform::FullscreenMode new_fullscreen = platform::FullscreenMode::Windowed;

    float frame_ms[120] = {};
    int frame_ms_pos = 0;
    int fps_choice = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    bool hdr = false;                 // HDR swapchain: the overlay is drawn into an RGBA8 target and composited (draw)
    extras::OverlayComposite composite;

    // Corner frame-rate counter. set_counter() stores the settings from any thread; the rest is used by the presenting thread only.
    std::atomic<bool> ptr_show{false}, ptr_in_shots{true};
    std::atomic<float> ptr_x{0}, ptr_y{0};
    // The game's layer (overlay_layer.cpp): its functions (set before the first frame) and the picture rectangle, fractions of the frame.
    std::atomic<bool (*)()> layer_active{nullptr};
    std::atomic<void (*)(OverlayCanvas&)> layer_draw{nullptr};
    std::atomic<float> pic_x{0.f}, pic_y{0.f}, pic_w{1.f}, pic_h{1.f};
    bool layer_wanted() const;            // the layer is set and has something to draw this frame
    void draw_game_layer(const FrameInfo& f);   // mu held, inside an ImGui frame
    std::atomic<bool> counter_show{false};
    std::atomic<int> counter_corner{0};
    std::atomic<int> counter_detail{1};
    std::atomic<bool> counter_in_shots{false};
    bool counter_was_on = false;
    bool counter_have_prev = false;
    std::chrono::steady_clock::time_point counter_prev{};
    FpsStats stats;
    double text_s = -1e9;                 // time of the last text refresh, seconds (far past: refresh at once)
    FpsStats::Snapshot snap;              // the snapshot taken at text_s
    std::string counter_lines[4];
    int counter_n = 0;

    void build_ui();
    void update_scale(const VkExtent2D& extent);
    void refresh_counter_text(double now_s);
    void draw_counter(const FrameInfo& f, double now_s);
    // The "Preparing shaders" note: shown once the backlog has stood for a second and a half, gone a second after it is empty.
    bool note_enabled = true;
    bool note_read = false;
    double note_since_s = -1.0, note_empty_s = -1.0;
    int note_peak = 0;
    bool note_visible(double now_s);
    void draw_note(const FrameInfo& f);
    // The "Save failed" notice (services_api.h fsapi::save_failures): shown for 10 s after a save commit was refused. Always on, also in
    // screenshots: the player has to see it.
    uint32_t save_seen = 0;
    double save_until_s = -1.0;
    bool save_notice_visible(double now_s);
    void draw_save_notice(const FrameInfo& f);
    // The short messages of nxrt/notices.h (mods' notify): stacked at the bottom left, oldest on top, a message's picture left of its text.
    // Left out of screenshots. Presenting thread only.
    void draw_notices(const FrameInfo& f, const std::vector<notices::Notice>& list);   // mu held, inside an ImGui frame
    // One ImGui user texture per shown picture, made the first time its message is drawn (the backend uploads it at that frame's render).
    // Once the message is gone it is handed back (WantDestroyNextFrame); the backend destroys it when no frame in flight uses it any more,
    // and then it is freed here. While any is held the overlay keeps drawing frames, so that happens within a few frames.
    struct NoticeTex {
        std::shared_ptr<const notices::Image> img;
        ImTextureData* tex = nullptr;
    };
    std::vector<NoticeTex> notice_tex;
    ImTextureData* notice_texture(const std::shared_ptr<const notices::Image>& img);   // mu held, inside an ImGui frame
    void retire_notice_textures(const std::vector<notices::Notice>* live);   // mu held, after NewFrame; live null: keep every one this frame
    void free_notice_textures();          // ~Overlay, after the backend's shutdown

    // The on-screen keyboard (services_api.h swkbdui). kb_mu guards the question and its text: key events edit it on the game's main thread,
    // the pad and the mouse in draw() on the presenting thread. Lock order: mu (ImGui), then kb_mu, then the swkbdui question's own lock.
    std::mutex kb_mu;
    std::atomic<bool> kb_open{false};
    std::atomic<bool> kb_by_label{false};
    uint64_t kb_id = 0;                   // the question on screen, 0 = none
    uint64_t kb_answered = 0;             // the last question this keyboard answered (ids only grow): never reopened
    services::swkbdui::Request kb_req;
    std::u32string kb_text;               // code points
    size_t kb_caret = 0;
    int kb_shift = 0;                     // 0 off, 1 the next character only, 2 locked
    int kb_row = 1, kb_unit = 0;          // the pad's key: row, column unit
    double kb_rep[5] = {};                // the next repeat of up, down, left, right, B (seconds; DBL_MAX: not held since a press)
    // The pad (set_keyboard_pad, on the thread that polls it): what is held, and presses not yet taken by draw().
    std::atomic<uint64_t> pad_held{0}, pad_new{0};
    uint64_t pad_prev = 0, pad_mask = 0;  // set_keyboard_pad only
    // Steam's on-screen keyboard (host_steam_keyboard_*): wanted for the question on screen (Proton on a Steam Deck, SteamOS, Steam's gamepad UI),
    // and asked for. It is asked for once the text field has a place on screen, and closed with the question.
    bool kb_steam_want = false, kb_steam_open = false;
    int kb_frames = 0;                    // frames drawn for the question on screen (its layout settles after the first)
    bool kb_keys = true;                  // the on-screen keys are shown (with Steam's keyboard: only after a pad press reached the game)
    void kb_steam_close();                // kb_mu held

    bool kb_poll();                       // follows swkbdui::pending(): opens a new question, drops one answered elsewhere; true while open
    bool kb_finish(bool ok);              // kb_mu held: answers the question; false when there is none or OK is not allowed yet (min_len)
    void kb_insert(const std::u32string& t);   // kb_mu held: at the caret, control characters dropped, up to max_len
    void kb_backspace();                  // kb_mu held
    bool kb_press(const KbKey& k);        // kb_mu held: an on-screen key; true when it answered the question
    bool kb_key_event(const input::Event& e);   // a physical key or text; kb_mu and mu not held
    void draw_keyboard(double now_s);     // mu held, inside an ImGui frame

    // The quit prompt (Overlay::open_quit_prompt). Flags only: opened from any thread, answered by key events (main thread), the pad
    // (set_keyboard_pad, main thread) or the mouse (draw, presenting thread). A confirmed prompt stays open until the process ends.
    std::atomic<bool> quit_open{false}, quit_confirmed{false}, settings_wanted{false};
    void quit_answer(bool ok) {
        if (ok) quit_confirmed = true;
        else quit_open = false;
    }
    void quit_to_settings() {   // the prompt's Settings choice: close it, the owner opens the F1 page (take_settings_wanted)
        settings_wanted = true;
        quit_open = false;
    }
    bool quit_key_event(const input::Event& e);   // a physical key while the prompt is open; always consumed
    void draw_quit_prompt();              // mu held, inside an ImGui frame
};

}  // namespace nxrt::gfx
