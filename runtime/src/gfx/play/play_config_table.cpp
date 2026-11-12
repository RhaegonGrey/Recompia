// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings table of controls.ini and its generic reader and writer, see play_config_table.h.
#include "play_config_table.h"

#include <cmath>
#include <string>
#include <vector>

#include "ini_text.h"
#include "prompt_resolve.h"
#include "nxrt/extras.h"

namespace nxrt::play {
namespace {

using namespace cfg;

// ---- the words of the choice keys, indexed by the enum value -------------------------------------------------------------------
constexpr const char* kHdrModeWords[] = {"off", "auto", "on", "native"};
constexpr const char* kHdrFormatWords[] = {"auto", "hdr10", "scrgb"};
constexpr const char* kUpscaleWords[] = {"off", "fsr1", "fsr3", "dlss", "xess", "nis", "dlaa", "nearest"};
constexpr const char* kUpscaleQualityWords[] = {"native", "quality", "balanced", "performance", "ultra_performance", "custom"};
constexpr const char* kShadowWords[] = {"default", "high", "very_high", "ultra", "extreme"};   // [display] shadow_quality, indexed by gfx::ShadowQuality
constexpr const char* kAaWords[] = {"off", "smaa_low", "smaa_medium", "smaa_high", "smaa_ultra"};   // [upscale] aa, indexed by extras::AaMode
constexpr const char* kLatencyWords[] = {"off", "auto", "on", "boost"};
constexpr const char* kClockFormatWords[] = {"auto", "12h", "24h"};
constexpr const char* kWindowModeWords[] = {"windowed", "borderless", "exclusive"};
constexpr const char* kAstcWords[] = {"fast", "high", "uncompressed"};
constexpr const char* kFpsSnapWords[] = {"auto", "true", "false"};
constexpr const char* kPicturePresetWords[] = {"neutral", "vivid"};
constexpr int kAnisotropyValues[] = {0, 2, 4, 8, 16};
// The upscale words accepted when reading (spatial is an older name of fsr1) and the mode each one means.
constexpr const char* kUpscaleReadWords[] = {"off", "fsr1", "spatial", "fsr3", "dlss", "xess", "nis", "dlaa", "nearest"};
constexpr extras::UpscaleMode kUpscaleFromWord[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Fsr1,
                                                    extras::UpscaleMode::Fsr3, extras::UpscaleMode::Dlss, extras::UpscaleMode::Xess,
                                                    extras::UpscaleMode::Nis, extras::UpscaleMode::Dlaa, extras::UpscaleMode::Nearest};

// ---- the readers and writers of the Custom rows ----------------------------------------------------------------------------------
bool bad_number(const std::string& k, const std::string& v, std::string* msg) {
    *msg = k + " must be a number, got '" + v + "'";
    return false;
}

// [mouse] yaw_source / pitch_source: the Source-scale sensitivity, stored as radians per count.
bool read_yaw_source(const std::string& k, const std::string& v, Settings* s, std::string* msg) {
    double x = 0;
    if (!to_double(v, &x)) return bad_number(k, v, msg);
    s->mouse.yaw_rad = rad_per_count(x);
    return true;
}
bool read_pitch_source(const std::string& k, const std::string& v, Settings* s, std::string* msg) {
    double x = 0;
    if (!to_double(v, &x)) return bad_number(k, v, msg);
    s->mouse.pitch_rad = rad_per_count(x);
    return true;
}

std::vector<std::string> prompt_style_words() {
    std::vector<std::string> r;
    for (int i = 0; i < kPromptStyleCount; ++i) r.push_back(prompt_style_name(static_cast<PromptStyle>(i)));
    return r;
}
bool read_prompt_style(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    if (parse_prompt_style(v, &s->prompts.style)) return true;
    std::string words;
    for (const std::string& w : prompt_style_words()) words += (words.empty() ? "" : ", ") + w;
    *msg = "style must be one of " + words + ", got '" + v + "'";
    return false;
}
std::string write_prompt_style(const Settings& s) { return prompt_style_name(s.prompts.style); }
// The launcher's preview of each style: "A=xbox/a" per Switch button (prompt_preview), with the factory pad mapping and bindings.
std::vector<std::pair<std::string, std::vector<std::string>>> prompt_style_details(const Settings& defaults) {
    std::vector<std::pair<std::string, std::vector<std::string>>> r;
    for (int i = 0; i < kPromptStyleCount; ++i) {
        const PromptStyle style = static_cast<PromptStyle>(i);
        std::vector<std::string> items;
        for (const PromptPreviewItem& it : prompt_preview(style, defaults)) items.push_back(std::string(it.button) + "=" + it.icon);
        if (!items.empty()) r.emplace_back(prompt_style_name(style), std::move(items));
    }
    return r;
}

bool read_fps_key(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    InputId id;
    if (!parse_input(v, &id) || id.kind != InKind::Key) {
        *msg = "key must be a keyboard key name (F2, Tab, Left Ctrl ...), got '" + v + "'";
        return false;
    }
    s->fps.key = id;
    return true;
}
std::string write_fps_key(const Settings& s) { return input_name(s.fps.key); }

bool read_corner(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    if (parse_corner(v, &s->fps.corner)) return true;
    *msg = "corner must be top_left, top_right, bottom_left or bottom_right, got '" + v + "'";
    return false;
}
std::string write_corner(const Settings& s) { return corner_name(s.fps.corner); }
std::vector<std::string> corner_words() {
    std::vector<std::string> r;
    for (int i = 0; i < 4; ++i) r.push_back(corner_name(static_cast<Corner>(i)));
    return r;
}

bool read_quit_pad(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    if (parse_pad_combo(v, &s->quit.pad)) return true;
    *msg = "pad must be none or pad buttons joined by '+' (back+start, left_shoulder+right_shoulder ...), got '" + v + "'";
    return false;
}
std::string write_quit_pad(const Settings& s) { return pad_combo_text(s.quit.pad); }

bool read_quit_key(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    if (lower(v) == "none" || v.empty()) {
        s->quit.key = {};
        return true;
    }
    InputId id;
    if (!parse_input(v, &id) || id.kind != InKind::Key) {
        *msg = "key must be none or a keyboard key name (F4, End, Pause ...), got '" + v + "'";
        return false;
    }
    s->quit.key = id;
    return true;
}
std::string write_quit_key(const Settings& s) { return s.quit.key.kind == InKind::Key ? input_name(s.quit.key) : std::string("none"); }

bool read_overlay_pad(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    if (parse_pad_combo(v, &s->overlay.pad)) return true;
    *msg = "pad must be none or pad buttons joined by '+' (left_stick+right_stick, back+start ...), got '" + v + "'";
    return false;
}
std::string write_overlay_pad(const Settings& s) { return pad_combo_text(s.overlay.pad); }

// [display] fps_snap: auto, or a yes / no in any of the bool spellings, stored as "true" / "false".
bool read_fps_snap(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    const std::string l = lower(v);
    bool b = false;
    if (l == "auto") s->display.fps_snap = "auto";
    else if (to_bool(l, &b)) s->display.fps_snap = b ? "true" : "false";
    else {
        *msg = "fps_snap must be auto, true or false, got '" + v + "'";
        return false;
    }
    return true;
}
std::string write_fps_snap(const Settings& s) { return s.display.fps_snap; }

// [display] aspect: auto (0), a ratio W:H (16:9, 21:9, 32:9, 16:10) or a number 1.0..5.0.
bool read_aspect(const std::string& k, const std::string& v, Settings* s, std::string* msg) {
    double x = 0;
    const size_t colon = v.find(':');
    double num = 0, den = 0;
    if (lower(v) == "auto") {
        x = 0;
    } else if (colon != std::string::npos && to_double(v.substr(0, colon), &num) && to_double(v.substr(colon + 1), &den) && num > 0 && den > 0) {
        x = num / den;
    } else if (!to_double(v, &x)) {
        *msg = k + " must be auto, W:H or a number, got '" + v + "'";
        return false;
    }
    if (x != 0 && (x < 1.0 || x > 5.0)) {
        *msg = k + " must be 0 (auto) or 1.0..5.0, got " + v;
        return false;
    }
    s->display.aspect = x;
    return true;
}
// A common ratio is written as W:H, so the file stays readable and the launcher's aspect list finds it; any other value as a number.
std::string write_aspect(const Settings& s) {
    if (s.display.aspect <= 0) return "auto";
    static constexpr int kRatios[][2] = {{16, 9}, {16, 10}, {21, 9}, {32, 9}, {4, 3}};
    for (const auto& r : kRatios)
        if (std::fabs(s.display.aspect - static_cast<double>(r[0]) / r[1]) < 1e-6) return std::to_string(r[0]) + ":" + std::to_string(r[1]);
    return num_exact(s.display.aspect);
}

bool read_quality_preset(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    const QualityPresetInfo* q = find_quality_preset(v);
    if (!q) {
        *msg = "quality_preset must be switch2, high, ultra, steamdeck, legion_go_s, laptop, desktop or custom, got '" + v + "'";
        return false;
    }
    s->display.quality_preset = q->id;
    return true;
}
std::string write_quality_preset(const Settings& s) { return s.display.quality_preset; }
std::vector<std::string> quality_preset_words() {
    int n = 0;
    const QualityPresetInfo* q = quality_presets(&n);
    std::vector<std::string> r;
    for (int i = 0; i < n; ++i) r.push_back(q[i].id);
    return r;
}

// [upscale] mode: the modes this build has (extras::upscale_mode_enabled), spatial read as fsr1.
bool read_upscale_mode(const std::string& k, const std::string& v, Settings* s, std::string* msg) {
    int i = 0;
    if (!pick_word(k, v, kUpscaleReadWords, &i, msg)) return false;
    s->extras.upscale.mode = extras::upscale_mode_enabled(kUpscaleFromWord[i]);
    return true;
}
std::string write_upscale_mode(const Settings& s) { return kUpscaleWords[static_cast<int>(s.extras.upscale.mode)]; }

// [picture] preset: fills the four values in; the keys after it in the file win. Read only: the file keeps the four values.
bool read_picture_preset(const std::string&, const std::string& v, Settings* s, std::string* msg) {
    if (extras::picture_preset(lower(v), &s->extras.picture)) return true;
    *msg = "preset must be neutral or vivid";
    return false;
}

// ---- the table ----------------------------------------------------------------------------------------------------------------
#define ROW(sec, k, t) .section = sec, .key = k, .type = SettingType::t
#define FLAG(m) .flag = [](Settings& s) -> bool& { return s.m; }
#define WHOLE(m) .whole = [](Settings& s) -> int& { return s.m; }
#define REAL(m) .real = [](Settings& s) -> double& { return s.m; }
#define TEXT(m) .text = [](Settings& s) -> std::string& { return s.m; }
#define ENUM(m, E) .get = [](const Settings& s) { return static_cast<int>(s.m); }, .set = [](Settings& s, int i) { s.m = static_cast<E>(i); }
#define CUSTOM(r, w) .read = r, .write = w

const SettingDef kTable[] = {
    {ROW("meta", "defaults_rev", Int), WHOLE(defaults_rev), .lo = 0, .hi = 1000,
     .doc = "The factory-defaults revision the file was written with (the game writes it; a file without it counts as 0). An older file gets the newer factory bindings and values at the next start."},
    {ROW("meta", "display_rev", Int), WHOLE(display_rev), .lo = 0, .hi = 1000,
     .doc = "The display-defaults revision the file was written with (the game writes it; a file without it counts as 0). macOS turns vsync on for an older file."},

    {ROW("mouse", "yaw", Real), REAL(mouse.yaw_rad), .doc = "Horizontal mouse sensitivity in radians per count."},
    {ROW("mouse", "pitch", Real), REAL(mouse.pitch_rad), .doc = "Vertical mouse sensitivity in radians per count."},
    {ROW("mouse", "yaw_source", Custom), CUSTOM(read_yaw_source, nullptr), .syntax = "real",
     .doc = "Horizontal sensitivity on the Source engine scale (0.022 degrees per count at 1.0), stored as yaw. When both forms appear the last one wins."},
    {ROW("mouse", "pitch_source", Custom), CUSTOM(read_pitch_source, nullptr), .syntax = "real",
     .doc = "Vertical sensitivity on the Source engine scale, stored as pitch."},
    {ROW("mouse", "invert_y", Bool), FLAG(mouse.invert_y), .doc = "1: moving the mouse up looks down."},
    {ROW("mouse", "invert_x", Bool), FLAG(mouse.invert_x), .doc = "1: moving the mouse right looks left."},
    {ROW("mouse", "menu_cursor", Bool), FLAG(mouse.menu_cursor),
     .doc = "In menus the mouse is a pointer: a visible cursor, left click activates, right click goes back, the wheel scrolls."},
    {ROW("mouse", nullptr, Bool), .key_fn = key_wheel_cycles, FLAG(mouse.wheel_cycles),
     .doc = "Kept in the file; no action reads it since the cycle actions went. A game may rename the key."},

    {ROW("tweaks", "language", Text), TEXT(language), .auto_word = "auto", .nxrt_key = "system.language",
     .doc = "The game's language: auto (the Windows display language when the game has it, else English) or a name such as French. Applies at the next start."},

    {ROW("tweaks", "clock_format", Choice), TEXT(clock_format), .words = kClockFormatWords,
     .doc = "How the game shows the time of day: auto (the game's language decides: 12 hours with AM/PM for English, 24 hours for German), 12h or 24h."},

    {ROW("pad", "label_abxy", Bool), FLAG(pad.label_abxy),
     .doc = "0: Switch layout, by position (the right face button is Switch A). 1: Xbox layout, by the printed letter."},
    {ROW("pad", "deadzone", Real), REAL(pad.stick_deadzone), .lo = 0.0, .hi = 0.5, .ranged = true,
     .doc = "Radial dead zone of the left stick (movement), applied before the game's own."},
    {ROW("pad", "right_deadzone", Real), REAL(pad.right_deadzone), .lo = 0.0, .hi = 0.5, .ranged = true, .fallback = "deadzone",
     .doc = "Radial dead zone of the right stick (looking)."},
    {ROW("pad", "invert_y", Bool), FLAG(pad.invert_y),
     .doc = "1: the right stick's up looks down, in play only. Adds to the game's own invert option."},
    {ROW("pad", "look_sens", Real), REAL(pad.look_sens), .lo = 0.25, .hi = 3.0, .ranged = true,
     .doc = "Multiplies the right stick's deflection in play."},
    {ROW("pad", "trigger_threshold", Real), REAL(pad.trigger_threshold), .lo = 0.05, .hi = 1.0, .ranged = true,
     .doc = "Analog trigger travel that counts as ZL / ZR pressed."},
    {ROW("pad", "stick_max", Real), REAL(pad.stick_max), .lo = 0.5, .hi = 1.0, .ranged = true,
     .doc = "Fraction of the left stick's travel that counts as fully pushed (1 = off), for pads that top out below the edge."},
    {ROW("pad", "right_stick_max", Real), REAL(pad.right_stick_max), .lo = 0.5, .hi = 1.0, .ranged = true, .fallback = "stick_max",
     .doc = "The same for the right stick."},
    {ROW("pad", "gyro", Bool), FLAG(pad.gyro), .doc = "Pass the pad's gyro to the game's six-axis sensor."},
    {ROW("pad", "gyro_scale", Real), REAL(pad.gyro_scale), .doc = "Gyro rate multiplier."},
    {ROW("pad", "rumble", Bool), FLAG(pad.rumble), .doc = "Pass the game's rumble to the pad."},
    {ROW("pad", "rumble_scale", Real), REAL(pad.rumble_scale), .doc = "Rumble strength multiplier."},

    {ROW("timing", "tap_ms", Int), WHOLE(timing.tap_ms), .lo = 1, .hi = 5000, .doc = "A press shorter than this is a tap (ms)."},
    {ROW("timing", "hold_ms", Int), WHOLE(timing.hold_ms), .lo = 1, .hi = 5000, .doc = "A hold starts after this (ms)."},
    {ROW("timing", "double_ms", Int), WHOLE(timing.double_ms), .lo = 1, .hi = 5000,
     .doc = "The second press of a double tap must come within this of the first release (ms)."},
    {ROW("timing", "pulse_ms", Int), WHOLE(timing.pulse_ms), .lo = 1, .hi = 5000,
     .doc = "How long a tap or a wheel notch stays pressed on the Switch side (ms)."},

    {ROW("display", "window_mode", Choice), TEXT(display.window_mode), .words = kWindowModeWords,
     .doc = "Exclusive falls back to borderless when the display refuses it. Alt+Enter and the F1 page switch modes while playing."},
    {ROW("display", "vsync", Bool), FLAG(display.vsync), .doc = "0: no forced vsync (immediate or mailbox presents, VRR friendly) with the frame limit."},
    {ROW("display", "fps_limit", Int), WHOLE(display.fps_cap), .lo = 0, .hi = 1000, .auto_word = "auto", .auto_at = -1, .renamed_from = "fps_cap",
     .doc = "Frame limit: 0 = unlimited, auto = the display's refresh rate. With vsync on, a limit applies only below the refresh rate."},
    {ROW("display", "fps_snap", Custom), CUSTOM(read_fps_snap, write_fps_snap), .words = kFpsSnapWords, .syntax = "choice",
     .nxrt_key = "display.fps_snap",
     .doc = "With vsync on, a frame limit below the refresh rate snaps to a divisor of it. auto: on a detected Steam Deck only."},
    {ROW("display", "width", Int), WHOLE(display.width), .lo = 320, .hi = 16384, .doc = "Windowed client width, remembered."},
    {ROW("display", "height", Int), WHOLE(display.height), .lo = 320, .hi = 16384, .doc = "Windowed client height, remembered."},
    {ROW("display", "keep_aspect", Bool), FLAG(display.keep_aspect), .doc = "Letterbox instead of stretching."},
    {ROW("display", "vram_cap", Bool), FLAG(display.vram_cap),
     .doc = "On a GPU with 4 GB of video memory or less, a render size that is not set is kept to 2560x1440."},
    {ROW("display", "render_scale", Int), WHOLE(display.render_scale), .lo = 25, .hi = 400, .auto_word = "auto", .auto_at = 0,
     .doc = "Internal resolution in percent; ignored with a fixed size. Next start."},
    {ROW("display", "render_width", Int), WHOLE(display.render_width), .lo = 320, .hi = 16384, .zero_ok = true,
     .doc = "Fixed internal width (with render_height); 0 = follow the window. Next start."},
    {ROW("display", "render_height", Int), WHOLE(display.render_height), .lo = 320, .hi = 16384, .zero_ok = true,
     .doc = "Fixed internal height (with render_width); 0 = follow the window. Next start."},
    {ROW("display", "aspect", Custom), CUSTOM(read_aspect, write_aspect), .lo = 1.0, .hi = 5.0, .ranged = true, .auto_word = "auto", .syntax = "ratio",
     .doc = "Shape of the 3D view and the HUD: auto (the internal resolution's), W:H (16:9, 21:9, 16:10) or a number. Next start."},
    {ROW("display", "fov_offset", Real), REAL(display.fov_offset), .lo = -30.0, .hi = 40.0, .ranged = true, .exact = true,
     .doc = "Degrees added to the vertical field of view of every game camera; 0 = the game's own."},
    {ROW("display", "dynamic_resolution", Bool), FLAG(display.dynamic_resolution),
     .doc = "The game's own dynamic resolution (it lowers the render size when the GPU is late)."},
    {ROW("display", "anisotropy", Int), WHOLE(display.anisotropy), .allowed = kAnisotropyValues,
     .doc = "0 = what the game's samplers ask for; else at least that on every filtering sampler."},
    {ROW("display", "lod_bias", Real), REAL(display.lod_bias), .lo = -2.0, .hi = 2.0, .ranged = true, .exact = true,
     .doc = "Texture LOD bias added to every sampler; negative is sharper."},
    {ROW("display", "shadow_quality", Choice), ENUM(display.shadow_quality, gfx::ShadowQuality), .words = kShadowWords, .nxrt_key = "display.shadow_quality",
     .doc = "Shadow map resolution: the game's shadow maps made 2x (high), 3x (very_high), 4x (ultra) or 8x (extreme) larger per side. Live."},
    {ROW("display", "bloom", Bool), FLAG(display.bloom), .doc = "The game's bloom."},
    {ROW("display", "chromatic_aberration", Bool), FLAG(display.chromatic_aberration), .doc = "The game's chromatic aberration."},
    {ROW("display", "quality_preset", Custom), CUSTOM(read_quality_preset, write_quality_preset), .word_fn = quality_preset_words, .syntax = "choice",
     .doc = "The set of values last picked on the F1 Graphics page (games with quality hooks); custom after a change by hand."},
    {ROW("display", "fast_gpu_memory", Bool), FLAG(display.fast_gpu_memory), .nxrt_key = "nvn.pool_mirror",
     .doc = "Device-local copies of the memory pools draws read (up to 2 GiB of video memory). Next start."},
    {ROW("display", "fewer_gpu_waits", Bool), FLAG(display.fewer_gpu_waits), .nxrt_key = "nvn.pass_barriers",
     .doc = "No full barrier after passes that only wrote their attachments. Next start."},
    {ROW("display", "shader_prewarm", Bool), FLAG(display.shader_prewarm), .nxrt_key = "nvn.prewarm",
     .doc = "Build the pipelines recorded by earlier runs ahead of the draws. Next start."},
    {ROW("display", "astc_quality", Choice), TEXT(display.astc_quality), .words = kAstcWords, .nxrt_key = "gfx.astc_quality",
     .doc = "ASTC texture quality on this GPU (uncompressed: RGBA8, lots of video memory). Next start."},
    {ROW("display", "monitor", Int), WHOLE(display.monitor), .lo = 0, .hi = 32, .auto_word = "auto", .auto_at = 0,
     .doc = "The display the window lives on, 1 = the first; auto = wherever the window opens."},

    {ROW("audio", "volume", Real), REAL(audio.volume), .lo = 0.0, .hi = 1.0, .ranged = true, .exact = true, .nxrt_key = "audio.volume",
     .doc = "Master gain, applied live."},
    {ROW("audio", "mute_unfocused", Bool), FLAG(audio.mute_unfocused), .doc = "Silent while the window has no focus."},
    {ROW("audio", "device", Text), TEXT(audio.device), .empty_as = "default", .nxrt_key = "audio.device",
     .doc = "default, or part of an output device's name. Next start."},

    {ROW("hdr", "mode", Choice), ENUM(extras.hdr.mode, extras::HdrMode), .words = kHdrModeWords, .nxrt_key = "hdr.mode", .doc = "HDR output."},
    {ROW("hdr", "format", Choice), ENUM(extras.hdr.format, extras::HdrFormat), .words = kHdrFormatWords, .nxrt_key = "hdr.format",
     .doc = "Swapchain format (auto: HDR10 when offered, else scRGB)."},
    {ROW("hdr", "peak_nits", Real), REAL(extras.hdr.peak_nits), .lo = 200.0, .hi = 10000.0, .ranged = true, .zero_ok = true, .exact = true,
     .nxrt_key = "hdr.peak_nits", .doc = "0 = the display's reported peak."},
    {ROW("hdr", "paper_white_nits", Real), REAL(extras.hdr.paper_white_nits), .lo = 80.0, .hi = 500.0, .ranged = true, .zero_ok = true,
     .exact = true, .nxrt_key = "hdr.paper_white_nits", .doc = "0 = Windows' SDR content brightness."},
    {ROW("hdr", "highlight_boost", Real), REAL(extras.hdr.highlight_boost), .lo = 0.0, .hi = 2.0, .ranged = true, .exact = true,
     .nxrt_key = "hdr.highlight_boost", .doc = "0 = the game's SDR look up to its own white, 1 = highlights reach the peak."},

    {ROW("upscale", "mode", Custom), CUSTOM(read_upscale_mode, write_upscale_mode), .words = kUpscaleWords, .syntax = "choice",
     .nxrt_key = "upscale.mode",
     .doc = "Upscaler; spatial is read as fsr1, and a mode this build does not have reads as off."},
    {ROW("upscale", "aa", Choice), ENUM(extras.upscale.aa, extras::AaMode), .words = kAaWords, .nxrt_key = "upscale.aa",
     .doc = "Anti-aliasing of the 3D scene: SMAA on the HDR scene before the tone map (the HUD and the menus are not filtered)."},
    {ROW("upscale", "quality", Choice), ENUM(extras.upscale.quality, extras::UpscaleQuality), .words = kUpscaleQualityWords,
     .nxrt_key = "upscale.quality", .doc = "The temporal modes' render scale preset."},
    {ROW("upscale", "sharpness", Real), REAL(extras.upscale.sharpness), .lo = 0.0, .hi = 1.0, .ranged = true, .exact = true,
     .nxrt_key = "upscale.sharpness", .doc = "Sharpening (1 = sharpest)."},
    {ROW("upscale", "scene_scale", Int), WHOLE(extras.upscale.scene_scale), .lo = 25, .hi = 100, .nxrt_key = "upscale.scene_scale",
     .doc = "Percent of the output size the 3D scene is rendered at; HUD and post stay native."},
    {ROW("upscale", "fsr_dll", Text), TEXT(extras.upscale.fsr_dll), .nxrt_key = "upscale.fsr_dll",
     .doc = "Path of the FidelityFX API DLL for FSR 3, kept as written (empty: next to the exe)."},
    {ROW("upscale", "dlss_dll", Text), TEXT(extras.upscale.dlss_dll), .nxrt_key = "upscale.dlss_dll",
     .doc = "Folder of the Streamline and DLSS DLLs, kept as written (empty: next to the exe)."},

    {ROW("latency", "mode", Choice), ENUM(extras.latency.mode, extras::LatencyMode), .words = kLatencyWords, .nxrt_key = "latency.mode",
     .doc = "Low latency mode (auto: whichever of Reflex and Anti-Lag 2 the GPU has; boost: Reflex boost)."},
    {ROW("latency", "frames_ahead", Int), WHOLE(extras.latency.frames_ahead), .lo = -1, .hi = 3,
     .doc = "With the mode on: frames simulated ahead of the last presented one; -1 = no wait."},
    {ROW("latency", "log_timings", Bool), FLAG(extras.latency.log_timings), .nxrt_key = "latency.log_timings",
     .doc = "Log one line of timings per second."},

    {ROW("picture", "preset", Custom), CUSTOM(read_picture_preset, nullptr), .words = kPicturePresetWords, .syntax = "choice",
     .nxrt_key = "picture.preset", .doc = "Fills in the four values below; the keys after it in the file win."},
    {ROW("picture", "contrast", Real), REAL(extras.picture.contrast), .lo = 0.5, .hi = 1.5, .ranged = true, .exact = true,
     .nxrt_key = "picture.contrast", .doc = "Contrast around mid grey."},
    {ROW("picture", "saturation", Real), REAL(extras.picture.saturation), .lo = 0.0, .hi = 2.0, .ranged = true, .exact = true,
     .nxrt_key = "picture.saturation", .doc = "Saturation (0 = grey)."},
    {ROW("picture", "black", Real), REAL(extras.picture.black), .lo = -0.5, .hi = 0.25, .ranged = true, .exact = true,
     .nxrt_key = "picture.black", .doc = "Black level: below 0 pulls the blacks down, above 0 lifts them."},
    {ROW("picture", "gamma", Real), REAL(extras.picture.gamma), .lo = 0.5, .hi = 2.0, .ranged = true, .exact = true,
     .nxrt_key = "picture.gamma", .doc = "Mid-tone brightness; above 1 brightens."},

    {ROW("prompts", "style", Custom), CUSTOM(read_prompt_style, write_prompt_style), .word_fn = prompt_style_words,
     .details_fn = prompt_style_details, .syntax = "choice",
     .doc = "Which button icons the game's prompts show; auto follows the last used input device."},

    {ROW("fps", "show", Bool), FLAG(fps.show), .doc = "The frame rate counter."},
    {ROW("fps", "key", Custom), CUSTOM(read_fps_key, write_fps_key), .syntax = "key",
     .doc = "The keyboard key that shows and hides the counter (never reaches the game)."},
    {ROW("fps", "corner", Custom), CUSTOM(read_corner, write_corner), .word_fn = corner_words, .syntax = "choice", .doc = "Where the counter sits."},
    {ROW("fps", "detail", Int), WHOLE(fps.detail), .lo = 0, .hi = 2,
     .doc = "0 frame rate, 1 also the frame time, 2 also the 1% low and the worst frame."},
    {ROW("fps", "in_screenshots", Bool), FLAG(fps.in_screenshots), .doc = "Draw the counter into screenshots too."},

    {ROW("quit", "pad", Custom), CUSTOM(read_quit_pad, write_quit_pad), .syntax = "pad_combo",
     .doc = "Pad buttons held together (back+start) that open the Close the game prompt; none = off."},
    {ROW("quit", "key", Custom), CUSTOM(read_quit_key, write_quit_key), .syntax = "key",
     .doc = "A keyboard key that opens the Close the game prompt; none = off."},

    {ROW("overlay", "pad", Custom), CUSTOM(read_overlay_pad, write_overlay_pad), .syntax = "pad_combo",
     .doc = "Pad buttons held together (left_stick+right_stick) that open and close the F1 menu; none = off. One equal to [quit] pad is ignored."},

    // Keys older builds wrote that nothing reads any more.
    {ROW("tweaks", "glow_color", Retired)}, {ROW("tweaks", "suit_glow", Retired)}, {ROW("tweaks", "ball_glow", Retired)},
    {ROW("tweaks", "gun_glow", Retired)}, {ROW("tweaks", "suit_color", Retired)}, {ROW("tweaks", "gun_color", Retired)},
    {ROW("tweaks", "skip_intro", Retired)}, {ROW("tweaks", "skip_boot_logos", Retired)}, {ROW("tweaks", "fast_menus", Retired)},
    {ROW("tweaks", "menu_speed", Retired)},
    {ROW("mouse", "alt_camera", Retired)}, {ROW("mouse", "alt_sensitivity", Retired)}, {ROW("mouse", "alt_yaw", Retired)},
    {ROW("mouse", "alt_pitch", Retired)}, {ROW("mouse", "menu_touch", Retired)},
    {ROW("pad", "alt_orbit", Retired)},
    {ROW("upscale", "xess_dll", Retired)}, {ROW("upscale", "frame_generation", Retired)},
    {ROW("display", "view_distance", Retired)},   // #249: the view distance is the console's
};

#undef ROW
#undef FLAG
#undef WHOLE
#undef REAL
#undef TEXT
#undef ENUM
#undef CUSTOM

// The file's sections in the order they are written.
constexpr SettingSection kSections[] = {
    {"meta", SectionKind::Rows},    {"mouse", SectionKind::Rows},   {"tweaks", SectionKind::Rows},  {"pad", SectionKind::Rows},
    {"padmap", SectionKind::PadMap}, {"timing", SectionKind::Rows}, {"display", SectionKind::Rows}, {"audio", SectionKind::Rows},
    {"hdr", SectionKind::Rows},     {"upscale", SectionKind::Rows}, {"latency", SectionKind::Rows}, {"picture", SectionKind::Rows},
    {"prompts", SectionKind::Rows}, {"fps", SectionKind::Rows},     {"quit", SectionKind::Rows},    {"overlay", SectionKind::Rows},
    {"switch", SectionKind::Switch},
};
constexpr const char* kRetiredSections[] = {"aim", "camera"};   // [aim] lock_mode free_lock_margin, [camera] gun_drives_view

// Actions and contexts older files may name. replaced_by: the action a binding gets instead (nullptr: the binding is dropped).
struct RetiredName {
    const char* name;
    const char* replaced_by;
};
constexpr RetiredName kRetiredActs[] = {
    {"primary", nullptr}, {"target", nullptr},   {"secondary", nullptr}, {"jump", nullptr},     {"alt_form", nullptr},
    {"look_hold", nullptr}, {"map", nullptr},    {"pause", nullptr},     {"confirm", "btn_a"},  {"back", "btn_b"},
    {"set_a_1", nullptr}, {"set_a_2", nullptr},  {"set_a_3", nullptr},   {"set_a_4", nullptr},  {"set_b_1", nullptr},
    {"set_b_2", nullptr}, {"set_b_3", nullptr},  {"set_b_4", nullptr},   {"set_a_next", nullptr}, {"set_a_prev", nullptr},
};
constexpr const char* kRetiredCtxs[] = {"alt", "inspect"};

std::string name_form(const std::string& name) {
    std::string r = lower(name);
    for (char& ch : r)
        if (ch == '-') ch = '_';
    return r;
}

const RetiredName* find_retired_act(const std::string& name) {
    const std::string k = name_form(name);
    for (const RetiredName& r : kRetiredActs)
        if (k == r.name) return &r;
    return nullptr;
}

// Writers bind through the same accessors as the reader; they only read through them.
Settings& unconst(const Settings& s) { return const_cast<Settings&>(s); }

}  // namespace

std::span<const SettingSection> setting_sections() { return kSections; }

const SettingSection* find_section(const std::string& lower_name) {
    for (const SettingSection& s : kSections)
        if (lower_name == s.name) return &s;
    return nullptr;
}

bool retired_section(const std::string& lower_name) {
    for (const char* n : kRetiredSections)
        if (lower_name == n) return true;
    return false;
}

std::span<const SettingDef> settings_table() { return kTable; }

const char* setting_key(const SettingDef& d) { return d.key ? d.key : d.key_fn(); }

bool setting_written(const SettingDef& d) { return d.type != SettingType::Retired && !(d.type == SettingType::Custom && !d.write); }

int find_setting(const std::string& section, const std::string& key) {
    for (size_t i = 0; i < std::size(kTable); ++i) {
        const SettingDef& d = kTable[i];
        if (section == d.section && (key == setting_key(d) || (d.renamed_from && key == d.renamed_from))) return static_cast<int>(i);
    }
    return -1;
}

bool setting_read(const SettingDef& d, const std::string& k, const std::string& v, Settings* s, std::string* msg) {
    switch (d.type) {
        case SettingType::Bool:
            if (!to_bool(v, &d.flag(*s))) {
                *msg = k + " must be 1/0 (or true/false, yes/no, on/off), got '" + v + "'";
                return false;
            }
            return true;
        case SettingType::Int: {
            if (d.auto_word && lower(v) == d.auto_word) {
                d.whole(*s) = d.auto_at;
                return true;
            }
            long long x = 0;
            if (!to_int(v, &x)) {
                *msg = k + " must be a whole number, got '" + v + "'";
                return false;
            }
            if (!d.allowed.empty()) {
                bool ok = false;
                std::string list;
                for (size_t i = 0; i < d.allowed.size(); ++i) {
                    ok = ok || x == d.allowed[i];
                    if (i > 0) list += i + 1 == d.allowed.size() ? " or " : ", ";
                    list += std::to_string(d.allowed[i]);
                }
                if (!ok) {
                    *msg = k + " must be " + list + ", got " + v;
                    return false;
                }
            } else if (!(d.zero_ok && x == 0) && (x < static_cast<long long>(d.lo) || x > static_cast<long long>(d.hi))) {
                *msg = k + " must be " + (d.zero_ok ? "0 or " : "") + std::to_string(static_cast<long long>(d.lo)) + ".." +
                       std::to_string(static_cast<long long>(d.hi)) + ", got " + v;
                return false;
            }
            d.whole(*s) = static_cast<int>(x);
            return true;
        }
        case SettingType::Real: {
            double x = 0;
            if (!to_double(v, &x)) return bad_number(k, v, msg);
            if (d.ranged && !(d.zero_ok && x == 0) && (x < d.lo || x > d.hi)) {
                *msg = k + " must be " + (d.zero_ok ? "0 (auto) or " : "") + num(d.lo) + ".." + num(d.hi) + ", got " + v;
                return false;
            }
            d.real(*s) = x;
            return true;
        }
        case SettingType::Text:
            if (d.auto_word && lower(v) == d.auto_word) d.text(*s).clear();
            else if (d.empty_as && v.empty()) d.text(*s) = d.empty_as;
            else d.text(*s) = v;
            return true;
        case SettingType::Choice: {
            int i = 0;
            if (!pick_word(k, v, d.words, &i, msg)) return false;
            if (d.text) d.text(*s) = d.words[i];
            else d.set(*s, i);
            return true;
        }
        case SettingType::Custom: return d.read(k, v, s, msg);
        case SettingType::Retired: return true;
    }
    return true;
}

std::string setting_write(const SettingDef& d, const Settings& cs) {
    Settings& s = unconst(cs);
    switch (d.type) {
        case SettingType::Bool: return d.flag(s) ? "1" : "0";
        case SettingType::Int: return d.auto_word && d.whole(s) <= d.auto_at ? std::string(d.auto_word) : std::to_string(d.whole(s));
        case SettingType::Real: return d.exact ? num_exact(d.real(s)) : num(d.real(s));
        case SettingType::Text: {
            const std::string& t = d.text(s);
            if (t.empty() && d.auto_word) return d.auto_word;
            if (t.empty() && d.empty_as) return d.empty_as;
            return t;
        }
        case SettingType::Choice: return d.text ? d.text(s) : std::string(d.words[d.get(s)]);
        case SettingType::Custom: return d.write ? d.write(cs) : std::string();
        case SettingType::Retired: return std::string();
    }
    return std::string();
}

void setting_copy(const SettingDef& from, const SettingDef& to, Settings* s) {
    switch (from.type) {
        case SettingType::Bool: to.flag(*s) = from.flag(*s); break;
        case SettingType::Int: to.whole(*s) = from.whole(*s); break;
        case SettingType::Real: to.real(*s) = from.real(*s); break;
        case SettingType::Text: to.text(*s) = from.text(*s); break;
        default: break;
    }
}

bool retired_act(const std::string& name) { return find_retired_act(name) != nullptr; }

const char* replaced_act(const std::string& name) {
    const RetiredName* r = find_retired_act(name);
    return r ? r->replaced_by : nullptr;
}

bool retired_ctx(const std::string& name) {
    const std::string k = name_form(name);
    for (const char* n : kRetiredCtxs)
        if (k == n) return true;
    return false;
}

std::vector<setting_doc::Row> settings_doc_rows() {
    const Settings defaults = default_settings();
    std::vector<setting_doc::Row> rows;
    // Sections in the file's order, rows in the table's order within each.
    for (const SettingSection& sec : kSections) {
        for (const SettingDef& d : kTable) {
            if (std::string(d.section) != sec.name) continue;
            setting_doc::Row r;
            r.section = d.section;
            r.key = setting_key(d);
            r.doc = d.doc ? d.doc : "";
            r.retired = d.type == SettingType::Retired;
            r.read_only = !r.retired && !setting_written(d);
            if (setting_written(d)) r.def = setting_write(d, defaults);
            switch (d.type) {
                case SettingType::Bool: r.type = "bool"; break;
                case SettingType::Int: r.type = "int"; break;
                case SettingType::Real: r.type = "real"; break;
                case SettingType::Text: r.type = "text"; break;
                case SettingType::Choice: r.type = "choice"; break;
                case SettingType::Custom: r.type = d.syntax ? d.syntax : "text"; break;
                case SettingType::Retired: r.type = "retired"; break;
            }
            if (d.type == SettingType::Int && d.allowed.empty()) r.has_min = r.has_max = true;
            if (d.ranged) r.has_min = r.has_max = true;
            r.min = d.lo;
            r.max = d.hi;
            r.zero_ok = d.zero_ok;
            for (const char* w : d.words) r.choices.push_back(w);
            for (int a : d.allowed) r.choices.push_back(std::to_string(a));
            if (d.word_fn) r.choices = d.word_fn();
            if (d.details_fn) r.choice_details = d.details_fn(defaults);
            if (d.auto_word) r.auto_word = d.auto_word;
            if (d.renamed_from) r.aliases.push_back(d.renamed_from);
            if (d.fallback) r.fallback = d.fallback;
            if (d.nxrt_key) r.nxrt_key = d.nxrt_key;
            rows.push_back(r);
        }
    }
    return rows;
}

}  // namespace nxrt::play
