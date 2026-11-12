// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the rows of the PC settings tabs (pkp_options_internal.h): each binds to a field of the play-mode Settings behind
// controls.ini, the same store as the F1 page, except the Gameplay tab's, which bind to the gameplay mods' [pkp] keys of the game ini.
// Nothing reaches the save file.
#include "ui/pkp_options_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "nxrt/host_device.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::games::pkp::options_detail {
namespace {

// clang-format off
#define ROW_BOOL(p) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = v != 0; }
#define ROW_ENUM(p, T) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = static_cast<T>(v); }
#define ROW_PCT(p) [](const S& s) { return static_cast<int>(std::lround(s.p * 100.0)); }, [](S& s, int v) { s.p = v / 100.0; }
#define ROW_INT(p) [](const S& s) { return static_cast<int>(s.p); }, [](S& s, int v) { s.p = v; }
#define ROW_STEP(p, base, step) [](const S& s) { return static_cast<int>(std::lround((s.p - (base)) / (step))); }, [](S& s, int v) { s.p = (base) + v * (step); }

int index_of(const int* values, int n, int v, int otherwise) {
    for (int i = 0; i < n; ++i) if (values[i] == v) return i;
    return otherwise;
}
int index_of(const double* values, int n, double v, int otherwise) {
    for (int i = 0; i < n; ++i) if (std::fabs(values[i] - v) < 1e-6) return i;
    return otherwise;
}

constexpr int kFpsValues[] = {-1, 30, 40, 45, 60, 90, 120, 144, 165, 240, 0};
constexpr int kFpsCount = static_cast<int>(std::size(kFpsValues));
constexpr const char* kFpsNames[kFpsCount + 1] = {"Match Display", "30", "40", "45", "60", "90", "120", "144", "165", "240", "Unlimited", "Custom"};
constexpr int kWidths[] = {1280, 1600, 1920, 2560, 3840}, kHeights[] = {720, 900, 1080, 1440, 2160};
constexpr int kSizeCount = static_cast<int>(std::size(kWidths));
constexpr const char* kSizeNames[kSizeCount + 1] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160", "Custom"};
// [display] aspect: 0 is auto (the window's shape); read once at start (pkp_play.cpp install_render), so the row applies after a restart.
constexpr double kAspectValues[] = {0.0, 16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0, 32.0 / 9.0};
constexpr int kAspectCount = static_cast<int>(std::size(kAspectValues));
constexpr const char* kAspectNames[kAspectCount + 1] = {"Auto", "16:9", "16:10", "21:9", "32:9", "Custom"};
constexpr int kAniso[] = {0, 2, 4, 8, 16};
constexpr const char* kAnisoNames[] = {"Game Default", "2x", "4x", "8x", "16x"};
// In play::PromptStyle order (the row stores the enum's number).
constexpr const char* kPromptNames[] = {"Switch",      "Xbox",        "PlayStation", "Steam Controller", "Steam Deck", "Nintendo Switch", "Switch 2",
                                        "Wii U",       "Steam Frame", "Valve Index", "Touch",            "Keyboard",   "Automatic"};
static_assert(std::size(kPromptNames) == play::kPromptStyleCount, "one name per PromptStyle");
constexpr const char* kAheadNames[] = {"No Limit", "0", "1", "2", "3"};
constexpr const char* kAaNames[] = {"Off", "SMAA Low", "SMAA Medium", "SMAA High", "SMAA Ultra"};      // indexed by extras::AaMode
constexpr const char* kShadowNames[] = {"Game Default", "High (2x)", "Very High (3x)", "Ultra (4x)", "Extreme (8x)"};      // indexed by extras::ShadowQuality
constexpr const char* kAstcKeys[] = {"fast", "high", "uncompressed"};
constexpr const char* kWindowKeys[] = {"windowed", "borderless", "exclusive"};
constexpr extras::UpscaleMode kUpscalers[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Nearest};

std::vector<std::string> g_devices;   // audio output names, refreshed when the screen opens

int key_index(const char* const* keys, int n, const std::string& v) {
    for (int i = 0; i < n; ++i) if (v == keys[i]) return i;
    return 0;
}

const char* preset_text(int v, char*, size_t) {
    int n = 0;
    const play::QualityPresetInfo* q = play::quality_presets(&n);
    return q[std::clamp(v, 0, n - 1)].name;
}
int preset_max() { int n = 0; play::quality_presets(&n); return n - 1; }

const char* monitor_text(int v, char* out, size_t n) {
    if (v <= 0) return "Automatic";
    if (!platform::display_name(v, out, static_cast<int>(n))) std::snprintf(out, n, "Display %d", v);
    return out;
}
int monitor_max() { return std::max(platform::display_count(), 1); }

const char* device_text(int v, char*, size_t) {
    return v <= 0 || v > static_cast<int>(g_devices.size()) ? "System Default" : g_devices[static_cast<size_t>(v - 1)].c_str();
}
int device_max() { return std::clamp(static_cast<int>(g_devices.size()), 1, 250); }

const Row kSound[] = {
    number("Master Volume", "The volume of everything in the game, on top of the sliders above.", 0, 100, "%.0f%%", 0, 1, ROW_PCT(audio.volume)),
    choice("Mute in Background", "Silence the game while its window is not in focus.", kOffOn, 2, ROW_BOOL(audio.mute_unfocused)),
    dynamic("Output Device", "The sound device the game plays through.", device_text, device_max,
            [](const S& s) { for (size_t i = 0; i < g_devices.size() && i < 250; ++i) if (g_devices[i] == s.audio.device) return static_cast<int>(i) + 1; return 0; },
            [](S& s, int v) { s.audio.device = v <= 0 || v > static_cast<int>(g_devices.size()) ? "default" : g_devices[static_cast<size_t>(v - 1)]; }, true),
};

const Row kDisplay[] = {
    choice("Window Mode", "Windowed, borderless fullscreen or exclusive fullscreen.", {"Windowed", "Borderless", "Fullscreen"}, 3,
           [](const S& s) { return key_index(kWindowKeys, 3, s.display.window_mode); }, [](S& s, int v) { s.display.window_mode = kWindowKeys[v]; }),
    dynamic("Monitor", "The display the window moves to. Fullscreen modes use this display.", monitor_text, monitor_max, ROW_INT(display.monitor)),
    names("Window Size", "The size of the window in windowed mode.", kSizeNames, kSizeCount + 1,
          [](const S& s) { for (int i = 0; i < kSizeCount; ++i) if (s.display.width == kWidths[i] && s.display.height == kHeights[i]) return i; return kSizeCount; },
          [](S& s, int v) { if (v < kSizeCount) { s.display.width = kWidths[v]; s.display.height = kHeights[v]; } }),
    names("Aspect Ratio", "The shape of the picture. Auto follows the window; 16:9 is the game's own.", kAspectNames, kAspectCount + 1,
          [](const S& s) { return index_of(kAspectValues, kAspectCount, s.display.aspect, kAspectCount); },
          [](S& s, int v) { if (v < kAspectCount) s.display.aspect = kAspectValues[v]; }, true),
    choice("Letterbox", "Black bars keep the picture's shape when it does not fit the window, instead of stretching it.", kOffOn, 2, ROW_BOOL(display.keep_aspect)),
    choice("VSync", "Wait for the display between frames: no tearing, a little more delay.", kOffOn, 2, ROW_BOOL(display.vsync)),
    names("Frame Rate Limit", "The highest frame rate. Match Display follows the refresh rate.", kFpsNames, kFpsCount + 1,
          [](const S& s) { return index_of(kFpsValues, kFpsCount, s.display.fps_cap, kFpsCount); },
          [](S& s, int v) { if (v < kFpsCount) { s.display.fps_cap = kFpsValues[v]; play::quality_preset_edited(s.display, play::QualityField::FrameLimit); } }),
    choice("Even Frame Pacing", "With VSync, run at an even divisor of the refresh rate (Steam Deck: 30 instead of 40).", kOffOn, 2,
           [](const S& s) { return static_cast<int>(s.display.fps_snap == "true" || (s.display.fps_snap == "auto" && nxrt::host_is_steam_deck())); },
           [](S& s, int v) { s.display.fps_snap = v ? "true" : "false"; }),
    autonum("Render Scale", "The game's resolution; 100% is 1920 x 1080. Auto follows the window.", 76, "%.0f%%", 20, 5,
            [](const S& s) { return s.display.render_scale <= 0 ? 0 : std::clamp((s.display.render_scale - 20) / 5, 1, 76); },
            [](S& s, int v) { s.display.render_scale = v <= 0 ? 0 : 20 + v * 5; play::quality_preset_edited(s.display, play::QualityField::RenderScale); }, true),
};

const Row kPerformance[] = {
    choice("Upscaler", "How the picture is enlarged from a lower resolution.", {"Off", "FSR 1", "Nearest"}, 3,
           [](const S& s) { for (int i = 0; i < 3; ++i) if (s.extras.upscale.mode == kUpscalers[i]) return i; return 0; },
           [](S& s, int v) { s.extras.upscale.mode = kUpscalers[v]; }),
    number("Sharpness", "How much FSR 1 sharpens the picture.", 0, 20, "%.2f", 0, 0.05, ROW_STEP(extras.upscale.sharpness, 0.0, 0.05)),
    number("Scene Resolution", "Renders the 3D scene smaller and keeps the menus sharp. 100% turns it off.", 25, 100, "%.0f%%", 0, 1, ROW_INT(extras.upscale.scene_scale)),
    names("Anti-Aliasing", "Smooths jagged edges of the 3D scene. The menus are not filtered.", kAaNames, 5,
          [](const S& s) { return static_cast<int>(s.extras.upscale.aa); },
          [](S& s, int v) { s.extras.upscale.aa = static_cast<extras::AaMode>(v); play::quality_preset_edited(s.display, play::QualityField::AntiAliasing); }),
    choice("Low Latency", "Reduces input delay on GPUs that support it.", {"Off", "Auto", "On", "Boost"}, 4, ROW_ENUM(extras.latency.mode, extras::LatencyMode), true),
    names("Frames Ahead", "How many frames may wait for the screen. Fewer is more responsive.", kAheadNames, 5,
          [](const S& s) { return s.extras.latency.frames_ahead + 1; }, [](S& s, int v) { s.extras.latency.frames_ahead = v - 1; }, true),
    choice("Frame Rate Counter", "Show the frame rate on screen.", kOffOn, 2, ROW_BOOL(fps.show)),
    choice("Counter Corner", "Where the frame rate counter sits.", {"Top Left", "Top Right", "Bottom Left", "Bottom Right"}, 4, ROW_ENUM(fps.corner, play::Corner)),
    choice("Counter Detail", "What the frame rate counter shows.", {"FPS", "FPS and Time", "Full"}, 3, ROW_INT(fps.detail)),
};

const Row kGraphics[] = {
    dynamic("Quality Preset", "Sets the texture options together.", preset_text, preset_max,
            [](const S& s) { int n = 0; const play::QualityPresetInfo* q = play::quality_presets(&n); for (int i = 0; i < n; ++i) if (s.display.quality_preset == q[i].id) return i; return n - 1; },
            [](S& s, int v) { int n = 0; const play::QualityPresetInfo* q = play::quality_presets(&n); const play::GamePlay* gp = play::game_play(); if (v < n - 1 && gp && gp->quality_preset) gp->quality_preset(q[v].id, s); }),
    choice("Dynamic Resolution", "Let the game lower its resolution when the GPU falls behind.", kOffOn, 2, ROW_BOOL(display.dynamic_resolution)),
    names("Texture Filtering", "Sharper textures at an angle.", kAnisoNames, 5,
          [](const S& s) { return index_of(kAniso, 5, s.display.anisotropy, 0); },
          [](S& s, int v) { s.display.anisotropy = kAniso[v]; play::quality_preset_edited(s.display, play::QualityField::Anisotropy); }),
    names("Shadow Quality", "Makes the game's shadow maps 2x, 3x, 4x or 8x sharper per side. Ultra and Extreme cost a lot of video memory.", kShadowNames, 5,
          [](const S& s) { return static_cast<int>(s.display.shadow_quality); },
          [](S& s, int v) { s.display.shadow_quality = static_cast<gfx::ShadowQuality>(v); play::quality_preset_edited(s.display, play::QualityField::Shadows); }),
    number("Texture Sharpness", "Texture level of detail. Negative is sharper.", 0, 80, "%+.2f", -2, 0.05,
           [](const S& s) { return static_cast<int>(std::lround((s.display.lod_bias + 2.0) / 0.05)); },
           [](S& s, int v) { s.display.lod_bias = -2.0 + v * 0.05; play::quality_preset_edited(s.display, play::QualityField::LodBias); }),
    choice("Texture Compression", "Fast loads quickest; Uncompressed uses a lot of video memory.", {"Fast", "High", "Uncompressed"}, 3,
           [](const S& s) { return key_index(kAstcKeys, 3, s.display.astc_quality); }, [](S& s, int v) { s.display.astc_quality = kAstcKeys[v]; }, true),
    choice("Faster GPU Memory", "Uses more video memory for smoother drawing.", kOffOn, 2, ROW_BOOL(display.fast_gpu_memory), true),
    choice("Fewer GPU Waits", "Skips pipeline waits that are not needed.", kOffOn, 2, ROW_BOOL(display.fewer_gpu_waits), true),
    choice("Prepare Shaders", "Builds known shaders early to reduce stutter.", kOffOn, 2, ROW_BOOL(display.shader_prewarm), true),
};

const Row kPicture[] = {
    choice("HDR", "High dynamic range output on a display that supports it.", {"Off", "Auto", "On", "Native"}, 4, ROW_ENUM(extras.hdr.mode, extras::HdrMode), true),
    choice("HDR Format", "The HDR signal sent to the display.", {"Auto", "HDR10", "scRGB"}, 3, ROW_ENUM(extras.hdr.format, extras::HdrFormat), true),
    autonum("Peak Brightness", "The brightest the display can go in HDR.", 197, "%.0f nits", 150, 50,
            [](const S& s) { return s.extras.hdr.peak_nits <= 0 ? 0 : std::clamp(static_cast<int>(std::lround((s.extras.hdr.peak_nits - 150) / 50.0)), 1, 197); },
            [](S& s, int v) { s.extras.hdr.peak_nits = v <= 0 ? 0 : 150 + v * 50; }),
    autonum("Paper White", "The brightness of white in HDR.", 43, "%.0f nits", 70, 10,
            [](const S& s) { return s.extras.hdr.paper_white_nits <= 0 ? 0 : std::clamp(static_cast<int>(std::lround((s.extras.hdr.paper_white_nits - 70) / 10.0)), 1, 43); },
            [](S& s, int v) { s.extras.hdr.paper_white_nits = v <= 0 ? 0 : 70 + v * 10; }),
    number("Highlight Boost", "How far highlights reach toward peak brightness.", 0, 40, "%.2f", 0, 0.05, ROW_STEP(extras.hdr.highlight_boost, 0.0, 0.05)),
    Row{"Color Preset", "Neutral is the game's own picture.", {"Neutral", "Vivid", "Custom"}, 3, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr,
        [](const S& s) { extras::PictureSettings v; extras::picture_preset("vivid", &v); const auto& p = s.extras.picture; if (p.neutral()) return 0; return p.contrast == v.contrast && p.saturation == v.saturation && p.black == v.black && p.gamma == v.gamma ? 1 : 2; },
        [](S& s, int v) { if (v < 2) extras::picture_preset(v ? "vivid" : "neutral", &s.extras.picture); }, false, true},
    number("Contrast", "Spreads the tones away from mid grey. 1.00 is the game's own.", 0, 100, "%.2f", 0.5, 0.01, ROW_STEP(extras.picture.contrast, 0.5, 0.01), true),
    number("Saturation", "Color intensity. 1.00 is the game's own.", 0, 100, "%.2f", 0, 0.02, ROW_STEP(extras.picture.saturation, 0.0, 0.02), true),
    number("Black Level", "Below 0 deepens the blacks, above 0 lifts them.", 0, 150, "%+.3f", -0.5, 0.005, ROW_STEP(extras.picture.black, -0.5, 0.005), true),
    number("Gamma", "Mid-tone brightness. 1.00 is the game's own.", 0, 150, "%.2f", 0.5, 0.01, ROW_STEP(extras.picture.gamma, 0.5, 0.01), true),
};

// The Mouse mode key (the Joy-Con 2 mouse mode, pkp_play.cpp): its binding in play, any key, mouse button or wheel notch, saved at once. The
// new key replaces any other play binding of that input; an all-context binding of it still works elsewhere.
class MouseModeKey final : public CaptureClient {
public:
    const char* name() const override { return "Mouse Mode Key"; }
    std::string bound() const override {
        std::string out;
        for (const play::Binding& b : play::current_settings().bindings)
            if (b.act == play::Act::MouseMode && (b.ctx == play::Ctx::Play || b.ctx == play::Ctx::All))
                out += (out.empty() ? "" : ", ") + play::input_name(b.in);
        return out.empty() ? "None" : out;
    }
    bool take(const play::InputId& in) override {
        S s = play::current_settings();
        std::erase_if(s.bindings, [&](const play::Binding& b) { return b.act == play::Act::MouseMode || (b.in == in && b.ctx == play::Ctx::Play); });
        s.bindings.push_back(play::Binding{in, play::Ctx::Play, play::Act::MouseMode, play::Trig::Press, 0});
        play::apply_settings(s);   // main thread, next frame; also writes controls.ini
        return true;
    }
} g_mouse_mode_key;

#define SENS(p) [](const S& s) { return static_cast<int>(std::lround(play::source_sens(s.mouse.p) * 10.0)); }, [](S& s, int v) { s.mouse.p = play::rad_per_count(v / 10.0); }
const Row kMouse[] = {
    number("Horizontal Sensitivity", "How fast the mouse turns the camera sideways.", 1, 200, "%.1f", 0, 0.1, SENS(yaw_rad)),
    number("Vertical Sensitivity", "How fast the mouse tilts the camera.", 1, 200, "%.1f", 0, 0.1, SENS(pitch_rad)),
    choice("Invert Vertical", "Moving the mouse up looks down.", kOffOn, 2, ROW_BOOL(mouse.invert_y)),
    choice("Invert Horizontal", "Moving the mouse left looks right.", kOffOn, 2, ROW_BOOL(mouse.invert_x)),
    choice("Mouse Pointer in Menus", "Use the mouse as a pointer: hover, click, right click, wheel.", kOffOn, 2, ROW_BOOL(mouse.menu_cursor)),
    names("Button Icons", "The button icons in prompts. Automatic follows the last device used.", kPromptNames, static_cast<uint8_t>(std::size(kPromptNames)), ROW_ENUM(prompts.style, play::PromptStyle)),
    capture_row("Mouse Mode Key", "Turns the Joy-Con 2 mouse mode on and off (point and click to place and break). To change it, press left or right on this row (or click it), then press the new key or mouse button. Esc cancels.",
                g_mouse_mode_key),
};

const Row kController[] = {
    choice("Face Buttons", "Switch keeps the buttons by position, Xbox by their letters.", {"Switch", "Xbox"}, 2, ROW_BOOL(pad.label_abxy)),
    number("Left Stick Dead Zone", "Ignores small movements of the left stick.", 0, 50, "%.0f%%", 0, 1, ROW_PCT(pad.stick_deadzone)),
    number("Right Stick Dead Zone", "Ignores small movements of the right stick.", 0, 50, "%.0f%%", 0, 1, ROW_PCT(pad.right_deadzone)),
    number("Left Stick Range", "How much of the stick's travel counts as full. Lower it for a worn stick.", 50, 100, "%.0f%%", 0, 1, ROW_PCT(pad.stick_max)),
    number("Right Stick Range", "How much of the stick's travel counts as full. Lower it for a worn stick.", 50, 100, "%.0f%%", 0, 1, ROW_PCT(pad.right_stick_max)),
    choice("Invert Look", "Stick up looks down. Adds to the game's own invert option.", kOffOn, 2, ROW_BOOL(pad.invert_y)),
    number("Look Sensitivity", "Multiplies the right stick while playing.", 0, 55, "%.2fx", 0.25, 0.05, ROW_STEP(pad.look_sens, 0.25, 0.05)),
    number("Trigger Threshold", "How far a trigger must be pressed to count.", 5, 100, "%.0f%%", 0, 1, ROW_PCT(pad.trigger_threshold)),
    choice("Motion Sensor", "Pass the controller's gyro to the game.", kOffOn, 2, ROW_BOOL(pad.gyro)),
    number("Motion Strength", "Scales the gyro.", 0, 78, "%.2fx", 0.1, 0.05, ROW_STEP(pad.gyro_scale, 0.1, 0.05)),
    number("Rumble Strength", "Scales the vibration.", 0, 200, "%.0f%%", 0, 1, ROW_PCT(pad.rumble_scale)),
    number("Controller Button", "Pick a controller button, then set what it presses on the next row.",
            0, 254, "%.0f", 0, 1, [](const S&) { return 127; }, [](S&, int) {}),
    number("Presses", "What the controller button above presses on the Switch pad. Default keeps the built-in layout.",
            0, 254, "%.0f", 0, 1, [](const S&) { return 127; }, [](S&, int) {}),
};

// The gameplay mods (the gameplay() rows of pkp_settings_table.h), labelled as in the launcher's Settings > Gameplay (settings-schema.js
// GAMEPLAY). title_quit has no row (hidden by the repo owner). Every mod reads its keys when it installs, so these rows apply after a restart;
// the shoulder camera's two distances are live keys (no switch: 0 / 0 is the game's own camera), in force when the screen commits, and so
// is camera aim's debug view (a plain key, not a mod).
using settings::Key;
constexpr Row kGameplay[] = {
    key_row("Running jump keeps speed", "A running jump keeps the running speed in the air.", Key::JumpMomentum),
    key_row("Running jump speed kept", "With the running jump on: how much of the extra running speed is kept.", Key::JumpMomentumStrength, 0.01, "%.0f%%", 100),
    key_row("Camera aim", "Abilities and tools aim where the camera looks instead of where your character faces, and your character turns to the target when the ability starts.", Key::CameraAim),
    key_row("Extra reach in blocks", "With camera aim: extra blocks of reach. Off is the game's own range.", Key::CameraAimReach, 1, "%.0f", 1, "Off"),
    key_row("Camera aim: show the ray", "Draws camera aim's ray over the picture: the blocks it tested, the block it hit and the block it selected, with the reason in a panel at the top left. For finding aiming bugs.", Key::CameraAimDebug),
    key_row("Sort button in the bag and containers", "A Sort button in the key guide of the bag and of the container screens.", Key::InventorySort),
    key_row("Sort each time the bag opens", "Sorts the bag each time the bag or a container screen opens.", Key::InventoryAutosort),
    key_row("Search in the crafting screen", "A Search button in the key guide of the crafting screen.", Key::CraftSearch),
    key_row("Item editor (experimental)", "Adds an Items tab to this screen that changes how many of an item you hold. Not during Link Play.", Key::ItemEditor),
    key_row("More Pokémon in town", "Experimental: more Pokémon can live in your town than the game's 25.", Key::PokemonCap),
    key_row("Pokémon in town", "With More Pokémon in town: how many can live there, 25 to 64. Visitors and spawns share the game's 64 slots, so a high number leaves them less room.", Key::PokemonCapLimit, 1, "%.0f"),
    key_row("Shoulder camera: side", "Moves the camera to the side so your character no longer blocks the view, in blocks. Right is positive, left negative; 0 is the game's own camera.", Key::CameraShoulderSide, 0.1, "%+.1f"),
    key_row("Shoulder camera: height", "Moves the camera up, in blocks. 0 is the game's own camera.", Key::CameraShoulderHeight, 0.1, "%+.1f"),
};

// The launcher's online presence (launcher/src-tauri/src/online.rs, which reads the key every few seconds while the game runs): its own
// tab so it is easy to find. The online name and the reset stay in the launcher.
constexpr Row kOnline[] = {
    key_row("Share that I am online", "While the game runs, the launcher tells the Recompia online server that you are playing: your online name from the launcher (empty: anonymous), the version, Windows or Linux, and how many Pokemon have a home in your save. Never your save or your user ID. Off unless you switch it on.", Key::OnlinePresence),
};
// clang-format on

}  // namespace

constinit const RowList kSoundRows = rows_of(kSound), kDisplayRows = rows_of(kDisplay), kPerformanceRows = rows_of(kPerformance),
                        kGraphicsRows = rows_of(kGraphics), kPictureRows = rows_of(kPicture), kMouseRows = rows_of(kMouse),
                        kControllerRows = rows_of(kController), kGameplayRows = rows_of(kGameplay),
                        kOnlineRows = rows_of(kOnline);

void refresh_audio_devices() {
    g_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) g_devices.push_back(d.name);
}

}  // namespace nxrt::games::pkp::options_detail
