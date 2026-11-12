// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode (S8): the data model shared by the settings file, the binding engine and the settings page.
//
// Vocabulary:
//   input    a physical thing on the keyboard or mouse: a key (SDL scancode), a mouse button or a wheel notch.
//   action   what the player wants the game to do (Pokopia: "move_forward", "btn_a", "dpad_up"). The game only knows the Switch pad,
//            so every action is tied to a Switch output (buttons and stick directions) by the [switch] table of the settings file.
//   context  where the player is: in play, on the map, in a menu.
//            `all` is a layer under every context.
// The action and context slots are generic; the registered game may name them and give their factory outputs and the launcher's labels
// (GameVocabulary below). Pokopia uses the generic names.
//   trigger  how an input turns into an action: press (while held), tap (a short pulse after a quick release), hold (after a delay,
//            while held), toggle (each press flips it), doubletap (a second quick press, while held).
// A binding says "in this context, this input drives this action with this trigger".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/extras.h"
#include "nxrt/gfx_tuning.h"

namespace nxrt::play {

// ---- contexts ----------------------------------------------------------------------------------------------------------------
// Play: the game in play.
enum class Ctx : uint8_t { All = 0, Play, Map, Menu, kCount };
constexpr int kCtxCount = static_cast<int>(Ctx::kCount);
const char* ctx_name(Ctx c);                           // the game's names (a game may use others); generic: "all" "play" "map" "menu"
bool parse_ctx(const std::string& s, Ctx* out);        // case-insensitive
inline bool ctx_in_play(Ctx c) { return c == Ctx::Play; }

// ---- triggers ----------------------------------------------------------------------------------------------------------------
enum class Trig : uint8_t { Press = 0, Tap, Hold, Toggle, DoubleTap };
const char* trig_name(Trig t);                         // "press" "tap" "hold" "toggle" "doubletap"
bool parse_trig(const std::string& s, Trig* out);      // case-insensitive; also accepts "double_tap"

// ---- actions -----------------------------------------------------------------------------------------------------------------
enum class Act : uint8_t {
    None = 0,
    // left stick
    MoveForward, MoveBack, MoveLeft, MoveRight,
    // right stick (keyboard look; the mouse does not use these)
    LookUp, LookDown, LookLeft, LookRight,
    // d-pad
    DpadUp, DpadDown, DpadLeft, DpadRight,
    // raw Switch buttons
    BtnA, BtnB, BtnX, BtnY, BtnL, BtnR, BtnZL, BtnZR, BtnPlus, BtnMinus, BtnLStick, BtnRStick,
    // a game-side switch with no Switch output (pkp: the Joy-Con 2 mouse mode, toggled by each activation)
    MouseMode,
    kCount
};
constexpr int kActCount = static_cast<int>(Act::kCount);

struct ActInfo {
    Act act;
    const char* name;           // the name used in the settings file: "move_forward" "btn_zl"; the game's for its slots
    const char* default_out;    // the default [switch] entry in output syntax (see parse_output): "ZR", "X+UP", "LS_UP" ... "" = none
};
const ActInfo& act_info(Act a);                         // the registered game's row (GameVocabulary::acts), else the generic one
bool parse_act(const std::string& name, Act* out);     // case-insensitive, '-' and '_' equal

// ---- Switch outputs ----------------------------------------------------------------------------------------------------------
// What an action does on the Switch pad: buttons pressed together plus stick deflection (each axis -1..1, summed over all active
// actions and clamped).
struct Output {
    uint64_t buttons = 0;       // nn::hid::NpadButton bits (nxrt/input_source.h: btn(kBtnA) ...)
    float lx = 0, ly = 0;       // left stick, y up = +
    float rx = 0, ry = 0;       // right stick, y up = +
    bool empty() const { return buttons == 0 && lx == 0 && ly == 0 && rx == 0 && ry == 0; }
    bool operator==(const Output& o) const { return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry; }
};
// Syntax: terms joined by '+', case-insensitive: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT (d-pad) LSTICK RSTICK (clicks)
// and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT. Example "X+UP" = X and d-pad up together.
// "" or "none" parses to the empty output. Unknown terms: returns false and *err names the term.
bool parse_output(const std::string& text, Output* out, std::string* err = nullptr);
std::string output_text(const Output& o);              // canonical text, parse_output(output_text(o)) == o

// ---- inputs ------------------------------------------------------------------------------------------------------------------
enum class InKind : uint8_t { None = 0, Key, Mouse, Wheel };
struct InputId {
    InKind kind = InKind::None;
    int32_t code = 0;           // Key: SDL_Scancode. Mouse: 1 left, 2 middle, 3 right, 4 x1, 5 x2 (the numbering of input.h). Wheel: +1 up, -1 down.
    bool operator==(const InputId& o) const { return kind == o.kind && code == o.code; }
    bool operator<(const InputId& o) const { return kind != o.kind ? kind < o.kind : code < o.code; }
};
// Names: SDL's scancode names for keys ("W", "Space", "Left Ctrl", "Tab", "1", "Keypad 5", "F"), "Mouse1".."Mouse5", "WheelUp", "WheelDown".
std::string input_name(const InputId& id);
bool parse_input(const std::string& name, InputId* out);   // case-insensitive; false for an unknown name

struct Binding {
    InputId in;
    Ctx ctx = Ctx::Play;    // All = every context that has no binding of its own for the same input
    Act act = Act::None;
    Trig trig = Trig::Press;
    int ms = 0;                 // Tap/Hold/DoubleTap window in ms; 0 = the default of [timing]
};

// ---- settings ----------------------------------------------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
// Source engine yaw scale: one mouse count at sensitivity 1.0 turns 0.022 degrees. The settings file stores radians per count; the
// settings page shows (and edits) the Source-scale value, source = rad_per_count / kSourceRadPerCount.
constexpr double kSourceRadPerCount = 0.022 * kPi / 180.0;
inline double source_sens(double rad_per_count) { return rad_per_count / kSourceRadPerCount; }
inline double rad_per_count(double source) { return source * kSourceRadPerCount; }

struct MouseSettings {
    double yaw_rad = 0.000767945;   // radians per count, horizontal
    double pitch_rad = 0.000767945;   // radians per count, vertical (separate)
    bool invert_y = false;                         // false: mouse up looks up
    bool invert_x = false;
    bool menu_cursor = true;                       // in menus the mouse is a pointer: cursor, hover, click, right click, wheel
    bool wheel_cycles = true;                       // [mouse] wheel_cycles: kept in the file; no action reads it since the cycle actions went
};
// ---- pad button remapping ------------------------------------------------------------------------------------------------------------
// The physical buttons of an SDL gamepad, named by position (the face buttons by where they sit, so the names do not depend on the printed
// letters). Each one presses a Switch button; `default` (kPadDefault) keeps the built-in mapping of pad_map.h.
enum class PadSrc : uint8_t {
    South = 0, East, West, North,                         // face buttons: bottom, right, left, top
    LeftShoulder, RightShoulder, LeftTrigger, RightTrigger,
    Start, Back, LeftStick, RightStick,                   // the two sticks are clicks
    DpadUp, DpadDown, DpadLeft, DpadRight,
    RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2,   // rear grips: Steam Controller R4, L4, R5, L5 (Xbox Elite, DualSense Edge and Joy-Con paddles too)
    Touchpad, TouchpadRight,                                // trackpad clicks: the touchpad button (a PlayStation touchpad, the Steam Controller's left trackpad), the Steam Controller's right trackpad
    Guide, Misc1,                                           // the Steam / Xbox / PlayStation button (SDL GUIDE); SDL's first extra button (the Steam Controller's and Deck's quick-access "...", an Xbox Share, a PS5 mic, a Switch Pro capture button)
    kCount
};
constexpr int kPadSrcCount = static_cast<int>(PadSrc::kCount);
const char* pad_src_name(PadSrc s);                      // the file's word: "south" "east" "west" "north" "left_shoulder" ... "dpad_right"
bool parse_pad_src(const std::string& name, PadSrc* out);   // case-insensitive, '-' and ' ' equal '_'
constexpr uint64_t kPadDefault = ~0ull;                  // PadSettings::remap value: the built-in mapping
constexpr uint32_t pad_src_bit(PadSrc s) { return 1u << static_cast<int>(s); }
// A set of pad buttons held together (QuitSettings::pad): PadSrc names joined by '+' ("back+start"), "none" (or empty) = no buttons.
// Case-insensitive like parse_pad_src; false for an unknown name. pad_combo_text writes them in PadSrc order, "none" for 0.
bool parse_pad_combo(const std::string& text, uint32_t* out);
std::string pad_combo_text(uint32_t bits);

struct PadSettings {
    uint64_t remap[kPadSrcCount] = {kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault,
                                    kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault,
                                    kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault, kPadDefault};
    bool label_abxy = false;       // false (default): Switch layout, by position (right = Switch A, bottom = B); true: Xbox layout, by label (A = A)
    double stick_deadzone = 0.10;  // left stick, radial, 0..0.5, applied before the game's own dead zone
    double right_deadzone = 0.10;  // right stick (looking), same rule
    double stick_max = 0.92;       // left stick, 0.5..1: the fraction of the travel that counts as full deflection (1 = off). A pad whose stick tops out below 1 (a worn pad, a DualSense near 0.96) reaches full speed with this lowered. Default 0.92 as in pokopia-native
    double right_stick_max = 0.92; // right stick (looking), same rule
    bool invert_y = false;         // right stick up looks down (in play only: menus and the map keep the stick as it is). Stacks with the game's own option.
    double look_sens = 1.0;        // 0.25..3: the right stick's deflection is multiplied by this in play (below 1 the turn speed tops out lower, above 1 it tops out sooner)
    double trigger_threshold = 0.35;   // analog trigger travel (0..1) that counts as ZL / ZR pressed
    bool gyro = true;              // pass the gyro to the six-axis sensor
    double gyro_scale = 1.0;
    bool rumble = true;
    double rumble_scale = 1.0;
};
struct TimingSettings {
    int tap_ms = 200;              // a press shorter than this is a tap
    int hold_ms = 250;             // a hold starts after this
    int double_ms = 300;           // the second press of a double tap must come within this of the first release
    int pulse_ms = 80;             // how long a tap / wheel notch stays pressed on the Switch side
};
struct DisplaySettings {
    std::string window_mode = "windowed";   // "windowed" "borderless" "exclusive"
    bool vsync = false;            // false: no forced vsync (immediate / mailbox, VRR friendly) with the frame cap below
    // [display] fps_limit (older name fps_cap): 0 = unlimited (with vsync on: mailbox presents), -1 = auto (the display's refresh rate),
    // else the cap; with vsync on, a cap only applies when it is below the refresh rate
    int fps_cap = 60;
    // [display] fps_snap: "auto" (on for a detected Steam Deck only), "true" (on anywhere), "false" (off). With vsync on, a limit below the
    // refresh rate then runs at an even divisor of it (40 -> 30 on 90 Hz). F1 "Cap FPS to 30 for Steam Deck" writes true / false.
    std::string fps_snap = "auto";
    int width = 1280, height = 720;    // windowed client size, remembered
    bool keep_aspect = true;       // letterbox instead of stretching
    bool vram_cap = true;          // [display] vram_cap: an unset render size is kept to 2560x1440 on a GPU with 4 GB of video memory or less
    // ---- S10 display options (play_display.h). The first four only apply at the next start: the game builds its render targets once.
    // Internal resolution in percent (25..400), 0 = auto; ignored with a fixed size. What 100 means is the game's: the window's client size
    // where the game module resizes the targets itself (auto = 100), the console's own size where the NVN back end scales the game's
    // targets (Pokopia: 100 = 1920x1080, auto = the window height / 1080, at least 1).
    int render_scale = 100;
    int render_width = 0, render_height = 0;   // fixed internal resolution (both > 0 and both within 320..16384), 0 = follow the window
    double aspect = 0;             // 0 = the ratio of the internal resolution; otherwise the forced ratio of the 3D view and the HUD (1.0 .. 5.0)
    // live:
    double fov_offset = 0;         // degrees added to the vertical field of view of every game camera (-30 .. 40), 0 = the game's own
    bool dynamic_resolution = false;   // the game's own dynamic resolution (it lowers the render size when the GPU is late)
    int anisotropy = 0;            // 0 = what the game's samplers ask for; 2, 4, 8 or 16 = at least that on every filtering sampler
    double lod_bias = 0;           // texture LOD bias added to every sampler (-2.0 .. 2.0; negative is sharper)
    gfx::ShadowQuality shadow_quality = gfx::ShadowQuality::Default;   // the game's shadow maps made 2x, 3x, 4x or 8x larger per side (live)
    bool bloom = true;
    bool chromatic_aberration = true;
    // Games with quality hooks (play_game.h kDisplayOptQuality, live): the preset names the set of values last picked on the F1 page
    // (switch2, high, ultra, steamdeck, legion_go_s, laptop, desktop; custom after a single change, quality_presets() below).
    std::string quality_preset = "switch2";
    // ---- S38 GPU paths, at the next start; [nvn] pool_mirror and pass_barriers in the game's ini or the environment win over these.
    bool fast_gpu_memory = true;   // device-local copies of the pools draws read ([nvn] pool_mirror = on); costs up to 2 GiB of video memory
    bool fewer_gpu_waits = true;   // no full barrier after passes that only wrote their attachments ([nvn] pass_barriers = tracked)
    bool shader_prewarm = true;    // build the pipelines recorded by earlier runs ahead of the draws ([nvn] prewarm); at the next start, [nvn] prewarm in the ini wins
    std::string astc_quality = "high";   // ASTC texture quality on this GPU: fast, high or uncompressed (RGBA8, lots of video memory); becomes [gfx] astc_quality at the next start unless the ini or the environment set it
    // The display the window lives on, 1-based in the system's order; 0 = automatic (wherever the window opens). Applied live (the window moves).
    int monitor = 0;
};

// ---- quality presets ([display] quality_preset, the F1 Graphics page) ------------------------------------------------------------
// The presets in the combo's order, "custom" last. The game's GamePlay::quality_preset hook writes their values. The picture presets
// (switch2, high, ultra) set the texture LOD bias, anisotropy, shadow quality and anti-aliasing; the device presets (sets_frame) also set the render
// scale and the frame limit. Changing one of the values the current preset set makes it custom (quality_preset_edited).
struct QualityPresetInfo {
    const char* id;       // the ini value, lowercase
    const char* name;     // the combo's label
    const char* about;    // one line for the tooltip: what it is for
    bool sets_frame;      // also sets render_scale and fps_cap
};
const QualityPresetInfo* quality_presets(int* count);              // all of them, custom last
const QualityPresetInfo* find_quality_preset(const std::string& id);   // case-insensitive; nullptr for an unknown id
enum class QualityField : uint8_t { LodBias, Anisotropy, Shadows, AntiAliasing, RenderScale, FrameLimit };
// The player changed `f` by hand: d.quality_preset becomes "custom" when the current preset set that value. True when it changed.
bool quality_preset_edited(DisplaySettings& d, QualityField f);

// ---- audio -------------------------------------------------------------------------------------------------------------------------
struct AudioSettings {
    double volume = 1.0;           // [audio] volume, master gain 0..1, applied live; [audio] volume in the ini or the environment wins at the start
    bool mute_unfocused = false;   // [audio] mute_unfocused: silent while the window has no focus (live)
    std::string device = "default";   // [audio] device: "default" or part of an output device's name; used at the next start ([audio] device in the ini wins)
};

// ---- button prompts ----------------------------------------------------------------------------------------------------------------
// Which button icons the game's prompts show: the game's own Switch icons, or those of another pad or the keyboard. Auto follows the
// last input device used. Every pad style is a set of the icon pack (prompt_resolve.cpp, kPadSets). Settings files store the name, so a
// new style may go anywhere before Keyboard.
enum class PromptStyle : uint8_t {
    Switch = 0, Xbox, PlayStation, Steam, SteamDeck, NintendoSwitch, Switch2, WiiU, SteamFrame, ValveIndex, Touch, Keyboard, Auto
};
constexpr int kPromptStyleCount = 13;
// "switch" "xbox" "playstation" "steam" "steamdeck" "switch1" "switch2" "wiiu" "steamframe" "index" "touch" "keyboard" "auto"
const char* prompt_style_name(PromptStyle s);
// Case-insensitive; also accepts ps, ps4, ps5, dualsense, steamcontroller, deck, steam_deck, nintendo_switch, ns1, ns2, nintendo_switch_2,
// wii_u, steam_frame, frame, valve_index, touchscreen, kbm, keyboard_mouse, mouse.
bool parse_prompt_style(const std::string& s, PromptStyle* out);
struct PromptSettings {
    PromptStyle style = PromptStyle::Auto;     // follows the last used device: keyboard icons until a pad is used (or when none is connected)
};

// ---- frame rate counter ------------------------------------------------------------------------------------------------------------
// The small corner overlay with the frame rate and the frame time. Off by default; a hotkey (and the F1 page and the launcher) switches it.
enum class Corner : uint8_t { TopLeft = 0, TopRight, BottomLeft, BottomRight };
const char* corner_name(Corner c);                       // "top_left" "top_right" "bottom_left" "bottom_right"
bool parse_corner(const std::string& s, Corner* out);    // case-insensitive, '-' and ' ' equal '_'
constexpr int32_t kScancodeF2 = 59;                      // SDL_SCANCODE_F2
struct FpsSettings {
    bool show = false;                                   // [fps] show
    InputId key = {InKind::Key, kScancodeF2};            // [fps] key: the hotkey that flips `show` (a keyboard key; never reaches the game)
    Corner corner = Corner::TopLeft;                     // [fps] corner
    int detail = 1;                                      // [fps] detail: 0 frame rate only, 1 frame rate and frame time, 2 also the 1% low and the worst frame
    bool in_screenshots = false;                         // [fps] in_screenshots: the counter is drawn into screenshots too (test tooling asks for this)
};

// ---- quit shortcut ------------------------------------------------------------------------------------------------------------
// A pad combination and a keyboard key that open the overlay's "Close the game?" prompt (the same prompt as the F1 page's Quit button).
// Neither reaches the game. The pad buttons are physical ones (before [padmap]), all held on one pad.
struct QuitSettings {
    uint32_t pad = pad_src_bit(PadSrc::Back) | pad_src_bit(PadSrc::Start);   // [quit] pad: bits of PadSrc, 0 = off (Select + Start)
    InputId key = {};                                                        // [quit] key: a keyboard key, kind None = off
};

// ---- overlay shortcut ---------------------------------------------------------------------------------------------------------
// A pad combination that opens or closes the F1 overlay menu, like the F1 key. Based on pokopia-native (MIT) windows/src/nxrt/front/menu.c @e5d9ac6
// (Minus + Plus there; here it is off by default, since Minus + Plus is this project's quit combination). Same rules as QuitSettings::pad.
struct OverlaySettings {
    uint32_t pad = pad_src_bit(PadSrc::LeftStick) | pad_src_bit(PadSrc::RightStick);   // [overlay] pad: bits of PadSrc, 0 = off (L3 + R3: L + R is the game's Dive chord, #364); one equal to [quit] pad is ignored with a warning
};

struct Settings {
    MouseSettings mouse;
    PadSettings pad;
    TimingSettings timing;
    DisplaySettings display;
    AudioSettings audio;
    extras::Settings extras;       // the [hdr] [upscale] [latency] [picture] sections (nxrt/extras.h): HDR output, upscaling, low latency
    PromptSettings prompts;
    FpsSettings fps;
    QuitSettings quit;
    OverlaySettings overlay;
    std::string language;          // [tweaks] language: a language name (AmericanEnglish, French ...) or "" = auto (the Windows language). Read by the
                                   // settings service at start-up (services/common.cpp); kept here so a save writes it back. Applies at the next start.
    std::string clock_format = "auto";   // [tweaks] clock_format: "auto" (the game's language decides), "12h" or "24h". The game's clock text hook reads it.
    Output out[kActCount];         // the [switch] table; defaulted from ActInfo::default_out
    std::vector<Binding> bindings;
    // [meta] defaults_rev: the factory-defaults revision the file was written with (0 when the file has no [meta]). When a file with
    // bindings sections is read at an older revision, the newer factory bindings are added to it (play_config.cpp defaults_rev(): the game's GameDefaults::rev and additions).
    int defaults_rev = 0;
    // [meta] display_rev: the display defaults a file has been brought to (0 when the file has no [meta]). macOS turns vsync on once for
    // a file below 1 (a Windows controls.ini has it off, and without it MoltenVK presents mid-refresh); a player who turns it off keeps it off.
    int display_rev = 0;
    // Transient, never written to the file: set by load_settings when it upgraded an older file, so the caller can keep a .bak and save.
    bool migrated = false;
    std::string migration_added;   // transient: the bindings the upgrade added, for the log line
};

// The factory settings: the table above and the default bindings (default_settings_text(), which is the same text the settings file would
// contain). The text, its revision and the bindings later revisions add are the registered game's (nxrt/game.h Info::play_defaults); without one the text is empty and the revision 0.
Settings default_settings();
const char* default_settings_text();

// A factory binding that a later revision of the defaults added: a controls.ini that has [bindings.*] sections and a defaults_rev below
// `rev` gets it (only where the input is still unbound in that context, so the user's own choice wins).
struct GameDefaultAddition {
    int rev;
    Ctx ctx;
    const char* input;
    Act act;
};
// A factory setting that a later revision changed: a controls.ini below `rev` that still has the old factory value `from` takes `to` once
// (a value the player picked themselves stays).
struct GameDefaultChange {
    int rev;
    const char* section;   // "display"
    const char* key;       // "render_scale"
    const char* from;
    const char* to;
};
struct GameVocabulary;
struct GameDefaults {
    const char* settings_text = "";                  // the factory settings file
    int defaults_rev = 0;                            // the [meta] defaults_rev this build writes; bump it together with a new addition
    const GameDefaultAddition* additions = nullptr;
    unsigned addition_count = 0;
    const GameVocabulary* vocabulary = nullptr;      // the names of the action and context slots (below)
    const struct GameDefaultChange* changes = nullptr;
    unsigned change_count = 0;
};
const GameDefaults* game_defaults();                 // the registered game's, or nullptr

// The game's words for the generic slots (Info::play_defaults->vocabulary). Every part is optional: a null table keeps the generic names.
struct GameActLabel {
    Act act;
    const char* label;          // the launcher's row label (UTF-8): "Fire", "Power beam"
    const char* group;          // the launcher's group (UTF-8): "Combat", "Beams"
};
struct GameVocabulary {
    const ActInfo* acts = nullptr;                   // kActCount rows in enum order: the file names and factory outputs of every action
    const char* const* ctx_names = nullptr;          // kCtxCount names in enum order
    const GameActLabel* labels = nullptr;            // the launcher's Keyboard rows, in display order
    unsigned label_count = 0;
    // settings-file key and launcher label of the wheel setting (nullptr: the generic key)
    const char* key_wheel_cycles = nullptr;          // [mouse] (a game may name it differently; generic "wheel_cycles")
    const char* label_wheel_cycles = nullptr;        // launcher check box (the game's own wording)
};
const GameVocabulary* game_vocabulary();             // the registered game's, or nullptr
// The settings-file key of the wheel setting: the game's, else the generic one.
const char* key_wheel_cycles();

// ---- the settings file ---------------------------------------------------------------------------------------------------------
// Every key of the fixed sections, with its range and default, is a row of the settings table (play_config_table.cpp), which the reader,
// the writer, docs/CONFIG.md and the launcher's schema follow; the summary below is the overview.
// Plain ini, '#' or ';' starts a comment. Sections and keys are case-insensitive, whitespace around names and values is ignored.
//   [meta]     defaults_rev = <n>   (written by the game; the factory-defaults revision of the file, a file without it counts as 0)
//   [mouse]    yaw = 0.000768   pitch = 0.000768   invert_y = 0   invert_x = 0   menu_cursor = 1   wheel_beams = 1     (radians per count)
//              yaw_source / pitch_source = 2.0 are accepted as well (Source scale); when both forms appear the last one wins
//   [pad]      label_abxy (0 = Switch layout, the default; 1 = Xbox layout, by label) deadzone stick_max right_stick_max trigger_threshold gyro gyro_scale rumble rumble_scale
//   [timing]   tap_ms hold_ms double_ms pulse_ms
//   [display]  window_mode vsync fps_limit (number, 0 = unlimited, auto = refresh rate; fps_cap is read too) width height keep_aspect render_scale render_width render_height aspect fov_offset
//              dynamic_resolution anisotropy lod_bias bloom chromatic_aberration fast_gpu_memory fewer_gpu_waits shader_prewarm astc_quality (fast high uncompressed)
//              monitor (auto or the display's number, 1 = the first)
//   [hdr]      mode (off auto on) format (auto hdr10 scrgb) peak_nits (0 = auto, else 200..10000) paper_white_nits (0 = auto, else
//              80..500) highlight_boost (0..2)
//   [upscale]  mode (off fsr1 nis, and fsr3 dlss xess kept but not used; spatial = fsr1) sharpness (0..1) fsr_dll dlss_dll (paths, kept verbatim)
//   [latency]  mode (off auto on boost) log_timings (1/0)
//   [picture]  contrast (0.5..1.5) saturation (0..2) black (-0.5..0.25) gamma (0.5..2), neutral 1 1 0 1; preset (neutral vivid) is read, not written
//   [audio]    volume (0..1) mute_unfocused (1/0) device (default or part of an output device's name)
//   [prompts]  style = switch|xbox|playstation|keyboard|auto   (which button icons the game's prompts show; auto, the default, follows the last used input device)
//   [tweaks]   language (auto or a language name such as French; next start)
//   [fps]      show (1/0) key (a keyboard key, F2) corner (top_left top_right bottom_left bottom_right) detail (0..2) in_screenshots (1/0)
//   [quit]     pad (pad buttons held together, e.g. back+start; none = off) key (a keyboard key; none = off)   (open the "Close the game?" prompt)
//   [overlay]  pad (pad buttons held together, left_stick+right_stick by default; none = off; not the [quit] pad)   (open and close the F1 menu)
//   [padmap]   <south east west north left_shoulder right_shoulder left_trigger right_trigger start back left_stick right_stick dpad_up dpad_down
//              dpad_left dpad_right> = default | none | <Switch buttons joined by '+', e.g. A or ZL+L>   (the pad button presses those Switch buttons)
//   [switch]   <action> = <output>      e.g.  btn_zr = ZR    dpad_up = UP    (missing actions keep the default)
//   [bindings.<ctx>]   <input> = <action> [trigger [ms]]     e.g.   Mouse1 = btn_zr    Left Ctrl = btn_lstick hold 250    Space = btn_r toggle
//                      ctx is a context name (all play map menu). The same input may appear several times in a section.
//   A game with a GameVocabulary may rename the actions and contexts.
// Unknown sections/keys are ignored with a warning in *warnings (not an error); a malformed value of a known key is an error.
// parse_settings starts from default_settings() and applies the text on top. If the text has any [bindings.*] section, the default
// bindings are dropped first (the file is then the whole truth); a file without bindings sections keeps the default bindings.
bool parse_settings(const std::string& text, Settings* out, std::string* err, std::vector<std::string>* warnings = nullptr);
std::string format_settings(const Settings& s);        // canonical text, parse_settings(format_settings(s)) reproduces s
// false + defaults in *out when the file is missing (err empty) or invalid (err set). *warnings (optional) gets the skipped lines: unknown
// sections and keys, bad values of fixed keys (those keep their default; only a broken line or binding makes the file invalid).
bool load_settings(const std::string& path, Settings* out, std::string* err, std::vector<std::string>* warnings = nullptr);
bool save_settings(const std::string& path, const Settings& s, std::string* err);   // writes atomically (temp file + rename)

}  // namespace nxrt::play
