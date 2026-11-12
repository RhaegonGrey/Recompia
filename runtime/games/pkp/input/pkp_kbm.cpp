// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, keyboard and mouse button prompts (pkp_kbm.h). Offsets in `main` of the 2.0.1 update, found in IDA (work/re/pc_features.md).
//
// The game writes button prompts as text tags: "^30~153~" is the glyph of whatever pad button does input action 153 ("pick up"). The text
// layout (0x1f90ca0) parses the tag and calls the tag handler stored in its text system (0xbfbe08 -> 0xe922dc). For type 0x30 the handler asks
// the input manager ([0x3561718]) which pad button does the action (0x1126e18: the remappable action table, with the game's L/R swap applied),
// looks up that button's sprite (0xa8332c: sheet and sprite id; 0x9125e0 the sheet; 0x24ec2c0 the 88-byte sprite record: name, size at
// +66/+68, the quad at +80 with its UVs at +24) and adds the sprite as an inline image. The button codes are the input manager's (its per-pad
// state at +1704 + code): 0 left 1 right 2 up 3 down (d-pad), 4 Y 5 A 6 X 7 B, 8 minus 9 plus, 10 L 11 R 12 ZL 13 ZR, 14 / 15 stick clicks;
// the stick glyphs have higher codes (105 is the right stick, sprite "0000_key_1_rs"); they are told apart by sprite name (..._ls, ..._rs).
// All codes of the action table are looked up at the first tag, because the HUD (bottom left) draws the same atlas sprites without tags.
//
// The mid hook sits on the layout's call of the handler (0x1f90fec: x0 the layout, w1 the tag type, x2 the arguments as UTF-16, x8 the
// handler). While a prompt style other than switch shows ([prompts] style keyboard/xbox/playstation, or auto following the last
// device used) and [pkp] kbm_icons is on:
//   * keyboard and mouse: the button's sprite in the game's UI atlas (a BC7 texture) is overwritten with the icon of the key bound
//     to that button (assets/kbm_icons/<name>.png next to the exe, Kenney's Input Prompts names: keyboard_e.png, mouse_left.png ...),
//     through nxrt/tex_patch.h. The game then draws its own glyph, which now shows the key, in every text that uses it. When the pads
//     are used again (or the binding changes) the saved pixels go back. A button with a bound key but no icon file gets the key's
//     name in the game's own font instead ("[E]"), through the layout's add-character call (vtable +16); the handler call is then
//     pointed at an empty function.
//   * xbox / playstation: the same atlas sprites are overwritten with the pad icons of the prompt icon pack
//     (assets/prompts/prompt_icons.pak: play::prompts_pack(); ps/cross for the button that presses Switch A, following [padmap]
//     and [pad] label_abxy), so the prompts show the controller's own buttons instead of the game's Switch glyphs. The four d-pad
//     glyphs (left, right, up, down) get the d-pad with that arm lit (xbox/dpad_up ...).
// The sprites are also looked up from the frame hook (pkp_play.cpp) once the sprite sheets are loaded, so the title screen and the HUD,
// which draw them without a text tag, show the icons before the first tag.
// The title screen's "PRESS (A) BUTTON" is not text: it is part of the title logo texture (a 1024x1024 BC7 texture loaded with the title
// layout, the English one: the A disc at (262..309, 631..678)). That texture is found by the content of the disc's blocks and the disc is
// drawn over with whatever A shows in the current prompt style (the Enter key's icon in keyboard style, the pad's button icon in a pad
// style; nxrt/tex_patch.h texture_patch_bc7_matching), in the art's style: a white key with its symbol cut out and the art's soft dark shadow.
#include "input/pkp_kbm.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "nxrt/dispatch.h"
#include "nxrt/gfx.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/tex_patch.h"
#include "nxrt/text.h"
#include "prompts.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kTagCall = 0x1f90fec;          // mid: text layout, BLR to the tag handler (w1 type, x2 args, x0 layout)
using layout::kInputMgr;
using layout::kActionButtons;
constexpr uint64_t kActionButton = 0x1126e18;     // u8 (mgr, u16 action, bool remap, bool): the button code of an action
constexpr uint64_t kTagIcon = 0xa8332c;           // (type, id, u32* sprite, u16* sheet, u16* extra): the sprite of an icon tag
constexpr uint64_t kSheet = 0x9125e0;             // (u16* sheet) -> the sprite sheet
constexpr uint64_t kSheetSprite = 0x24ec2c0;      // (sheet, sprite id) -> the 88-byte sprite record
constexpr uint64_t kEmpty = 0x124750;             // a function that only returns
using layout::kTables;
using layout::kUiMgr;
constexpr uint32_t kGuestMagic = 0x4e585433;      // "NXT3": the runtime's header in the game's NVN objects (+8)

// Glyph slots: the 16 button codes, then the two stick glyphs.
constexpr int kCodes = 16, kSlotLS = 16, kSlotRS = 17, kSlots = 18;   // the sticks are found by their sprite names (..._ls, ..._rs)
constexpr int kMaxCode = 0x7f;                                          // the game's glyph codes are 0..0x7e
constexpr uint64_t kCodeBit[kCodes] = {1ull << 12, 1ull << 14, 1ull << 13, 1ull << 15, 1ull << 3, 1ull << 0, 1ull << 2, 1ull << 1,
                                       1ull << 11, 1ull << 10, 1ull << 6, 1ull << 7, 1ull << 8, 1ull << 9, 1ull << 4, 1ull << 5};
constexpr const char* kSlotName[kSlots] = {"left", "right", "up", "down", "Y", "A", "X", "B", "minus", "plus", "L", "R", "ZL", "ZR",
                                           "LS click", "RS click", "LS", "RS"};

struct Want {
    std::string label;    // "E", "Space", "LMB" ... "" = nothing bound: the game's glyph stays
    std::string icon;     // the icon file's name without .png ("keyboard_e"), "" = none
};

struct Glyph {
    bool tried = false;        // the sprite was looked up (found or not)
    uint64_t tex = 0;          // the game's NVNtexture of the atlas, 0 = not found
    uint32_t x = 0, y = 0, w = 0, h = 0;
    std::string shown;         // the icon written there now, "" = the game's own pixels
    std::vector<uint8_t> saved;
};

struct Image {
    bool tried = false;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
};

constexpr std::array<int8_t, kMaxCode> unknown_codes() {
    std::array<int8_t, kMaxCode> a{};
    for (auto& v : a) v = -2;
    return a;
}

struct State {
    uint64_t base = 0x7100000000ull;
    bool enabled = true;                             // [pkp] kbm_icons
    std::string dir;                                 // <exe folder>/assets/kbm_icons/
    play::Settings settings;                         // the last applied settings: bindings for the keyboard style, the pad remap for pad styles
    play::PromptStyle eff = play::PromptStyle::Switch;   // the style that shows now (play::prompts_effective_style)
    std::mutex mu;
    std::array<Want, kSlots> want;
    std::array<Glyph, kSlots> glyph;
    std::array<int8_t, kMaxCode> code_slot = unknown_codes();   // glyph code -> slot, -1 = none, -2 = not looked up yet
    bool all_resolved = false;
    std::map<std::string, Image> images;
    uint32_t frames = 0, frame_tries = 0;            // kbm_frame: game frames seen, lookups tried before the first tag
    uint64_t types_logged[4] = {};                   // text tag types logged once (diagnostics)
#if NXRT_TESTTOOLS
    std::vector<uint64_t> dumped;                    // atlases written to NXRT_PKP_KBM_DUMP
#endif
    std::string title_icon;                          // the title art's A: the Enter key bound to A (else A's icon)
    std::vector<uint8_t> title_px;                   // ... drawn (kTitleArt), for title_px_icon
    std::string title_px_icon;
    std::vector<uint32_t> title_checked;             // textures looked at for the title art
    uint32_t scan_every = 60, scan_last = 0, scan_empty = 0;   // the title-art scan backs off (60 .. 600 game frames) while nothing changes (#280)
    size_t scan_registry = ~size_t{0};               // registry size at the last scan
    play::PromptStyle scan_style = play::PromptStyle::Switch;
    uint32_t scan_slow_logged = 0;
    std::atomic<uint64_t> tags{0}, texts{0}, patches{0};
};

State& st() {
    static State s;
    return s;
}

using guest::rd;
using guest::call;

// ---- what each glyph should show ----
std::string label_of(const play::InputId& in) {
    if (in.kind == play::InKind::Mouse) {
        static const char* const kMouse[] = {"", "LMB", "MMB", "RMB", "Mouse4", "Mouse5"};
        return in.code >= 1 && in.code <= 5 ? kMouse[in.code] : play::input_name(in);
    }
    if (in.kind == play::InKind::Wheel) return in.code > 0 ? "Wheel Up" : "Wheel Down";
    return play::input_name(in);
}

// Kenney Input Prompts file name (Keyboard & Mouse/Default) of an input, "" when the pack has none.
std::string icon_of(const play::InputId& in) {
    if (in.kind == play::InKind::Mouse) return in.code == 1 ? "mouse_left" : in.code == 2 ? "mouse_scroll" : in.code == 3 ? "mouse_right" : "";
    if (in.kind == play::InKind::Wheel) return in.code > 0 ? "mouse_scroll_up" : "mouse_scroll_down";
    if (in.kind != play::InKind::Key) return "";
    std::string n = play::input_name(in);
    n = nxrt::text::lower(std::move(n));
    if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0]))) return "keyboard_" + n;
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) return "keyboard_" + n;   // f1..f12
    static const std::pair<const char*, const char*> kNames[] = {
        {"space", "space"}, {"return", "return"}, {"keypad enter", "numpad_enter"}, {"left shift", "shift"}, {"right shift", "shift"},
        {"left ctrl", "ctrl"}, {"right ctrl", "ctrl"}, {"left alt", "alt"}, {"right alt", "alt"}, {"escape", "escape"},
        {"backspace", "backspace"}, {"tab", "tab"}, {"up", "arrow_up"}, {"down", "arrow_down"}, {"left", "arrow_left"},
        {"right", "arrow_right"}, {"capslock", "capslock"}, {"delete", "delete"}, {"end", "end"}, {"home", "home"}, {"insert", "insert"},
        {"pageup", "page_up"}, {"pagedown", "page_down"}, {"-", "minus"}, {"=", "equals"}, {",", "comma"}, {".", "period"},
        {"/", "slash_forward"}, {"\\", "slash_back"}, {";", "semicolon"}, {"'", "apostrophe"}, {"`", "tilde"}, {"[", "bracket_open"},
        {"]", "bracket_close"}, {"keypad +", "numpad_plus"},
    };
    for (const auto& [k, v] : kNames)
        if (n == k) return std::string("keyboard_") + v;
    return "";
}

// The input bound to an action (play bindings before the `all` layer and the menu), press bindings only.
bool input_for_act(const play::Settings& s, play::Act a, play::InputId* out) {
    for (play::Ctx want : {play::Ctx::Play, play::Ctx::All, play::Ctx::Menu})
        for (const play::Binding& b : s.bindings)
            if (b.act == a && b.ctx == want && b.trig == play::Trig::Press) {
                *out = b.in;
                return true;
            }
    return false;
}

// The input bound to exactly these Switch buttons: the named game actions before the raw btn_ ones.
bool input_for_bits(const play::Settings& s, uint64_t bit, play::InputId* out) {
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 1; a < play::kActCount; ++a) {
            const bool raw = a >= static_cast<int>(play::Act::BtnA);
            if ((pass == 0) == raw) continue;
            const play::Output& o = s.out[a];
            if (o.buttons != bit || o.lx != 0 || o.ly != 0 || o.rx != 0 || o.ry != 0) continue;
            if (input_for_act(s, static_cast<play::Act>(a), out)) return true;
        }
    return false;
}

// ---- icon files ----
// The game's button glyphs (atlas #493, "0000_key_1_a" ...) are a white cap with a dark rim and a dark letter, so they read on the white
// dialogue boxes, the blue tutorial boxes and the 3D scene alike. Kenney's keys are a white cap with the letter cut out (transparent), which
// vanishes on a white box. The icon is put on the game's look: a plate of the game's rim colour under it (the icon's shape grown by a rim of
// 3/64 of its size, and every hole inside the shape, the letter, filled) and the icon itself on top.
constexpr uint8_t kPlate[3] = {51, 51, 51};   // the game's rim and letter: grey 51 at alpha 191
constexpr float kPlateAlpha = 191.f / 255.f;

// The title logo's A disc: the 72 x 72 rectangle around it, the FNV-1a hash of its BC7 blocks (the English title art of 2.0.1), and the
// disc in that rectangle (48 x 48 at 10, 11).
constexpr gfx::Bc7Match kTitleArt = {1024, 1024, 252, 620, 72, 72, 0xc400174b48564fa1ull};
constexpr uint32_t kTitleGlyph = 48, kTitleGlyphX = 10, kTitleGlyphY = 11;

// 1 for the clear texels reachable from the border through clear texels; the other clear texels are holes in the shape (a key's letter).
std::vector<uint8_t> outside_of(const uint8_t* rgba, int w, int h) {
    auto a = [&](int x, int y) { return rgba[(static_cast<size_t>(y) * w + x) * 4 + 3]; };
    std::vector<uint8_t> outside(static_cast<size_t>(w) * h, 0);
    std::vector<int> todo;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((x == 0 || y == 0 || x == w - 1 || y == h - 1) && a(x, y) < 128) outside[static_cast<size_t>(y) * w + x] = 1, todo.push_back(y * w + x);
    while (!todo.empty()) {
        const int i = todo.back(), x = i % w, y = i / w;
        todo.pop_back();
        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& n : nb) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
            const size_t j = static_cast<size_t>(n[1]) * w + n[0];
            if (!outside[j] && a(n[0], n[1]) < 128) outside[j] = 1, todo.push_back(static_cast<int>(j));
        }
    }
    return outside;
}

void plate(Image& im) {
    const int w = static_cast<int>(im.w), h = static_cast<int>(im.h);
    auto a = [&](int x, int y) { return im.rgba[(static_cast<size_t>(y) * w + x) * 4 + 3]; };
    const std::vector<uint8_t> outside = outside_of(im.rgba.data(), w, h);
    const int r = std::max(1, static_cast<int>(std::lround(w * 3.0 / 64)));
    std::vector<uint8_t> out(im.rgba.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float pa = outside[static_cast<size_t>(y) * w + x] ? 0.f : 255.f;   // plate alpha: holes full, the rest grown from the shape
            for (int dy = -r; dy <= r && pa < 255.f; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                    if (d > r + 0.5f) continue;
                    pa = std::max(pa, a(sx, sy) * std::clamp(r + 0.5f - d, 0.f, 1.f));   // soft edge on the rim
                }
            const uint8_t* s = &im.rgba[(static_cast<size_t>(y) * w + x) * 4];
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
            const float sa = s[3] / 255.f, da = pa / 255.f * kPlateAlpha, oa = sa + da * (1 - sa);
            for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(oa > 0 ? std::lround((s[ch] * sa + kPlate[ch] * da * (1 - sa)) / oa) : 0);
            o[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
        }
    im.rgba = std::move(out);
}

// A pack icon without colour of its own: the d-pad, the triggers and shoulders and the d-pad's direction icons are white or grey, which
// vanishes on the pale move wheel and the dialogue boxes (#230). The face buttons (A, B, cross, circle...) are saturated and read as they are.
bool colourless(const Image& im) {
    size_t opaque = 0, coloured = 0;
    for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
        if (im.rgba[i + 3] <= 128) continue;
        ++opaque;
        const uint8_t hi = std::max({im.rgba[i], im.rgba[i + 1], im.rgba[i + 2]}), lo = std::min({im.rgba[i], im.rgba[i + 1], im.rgba[i + 2]});
        if (hi - lo > 60) ++coloured;
    }
    return opaque > 0 && coloured * 4 < opaque;
}

// The icon `name`: a pack entry ("xbox/a", "ps/cross"; names with a slash, straight RGBA from the prompt pack; the colourless ones are put
// on the plate like the keyboard icons) or a Kenney file `name`.png from assets/kbm_icons, on its plate (plated) or as drawn by Kenney.
const Image* image_locked(State& s, const std::string& name, bool plated = true) {
    const bool from_pack = name.find('/') != std::string::npos;
    Image& im = s.images[from_pack ? "pack:" + name : (plated ? name : "raw:" + name)];
    if (!im.tried) {
        im.tried = true;
        if (from_pack) {
            const play::IconPack* pack = play::prompts_pack();
            const uint8_t* px = pack ? pack->find(name) : nullptr;
            if (px && pack->width() >= 8 && pack->height() >= 8) {
                im.w = pack->width();
                im.h = pack->height();
                im.rgba.assign(px, px + static_cast<size_t>(im.w) * im.h * 4);
                if (colourless(im)) plate(im);
                NXLOG_I(kTag, "prompt icon %s: %ux%u from the pack", name.c_str(), im.w, im.h);
            } else im.rgba.clear();
        } else if (!gfx::load_png_rgba(s.dir + name + ".png", &im.rgba, &im.w, &im.h) || im.w < 8 || im.h < 8) im.rgba.clear();
        else {
            if (plated) plate(im);
            NXLOG_I(kTag, "kbm icon %s.png: %ux%u", name.c_str(), im.w, im.h);
        }
    }
    return im.rgba.empty() ? nullptr : &im;
}

// The icon scaled (bilinear) to w x h.
std::vector<uint8_t> scaled(const Image& im, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = std::clamp((x + 0.5f) * im.w / w - 0.5f, 0.f, im.w - 1.f), fy = std::clamp((y + 0.5f) * im.h / h - 0.5f, 0.f, im.h - 1.f);
            const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
            const uint32_t x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
            const float ax = fx - x0, ay = fy - y0;
            // premultiplied, so clear texels (black) do not darken the edges
            auto p = [&](uint32_t px, uint32_t py, int ch) {
                const uint8_t* q = &im.rgba[(static_cast<size_t>(py) * im.w + px) * 4];
                return ch == 3 ? static_cast<float>(q[3]) : q[ch] * (q[3] / 255.f);
            };
            auto lerp = [&](int ch) { return (p(x0, y0, ch) * (1 - ax) + p(x1, y0, ch) * ax) * (1 - ay) + (p(x0, y1, ch) * (1 - ax) + p(x1, y1, ch) * ax) * ay; };
            const float alpha = lerp(3);
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
            for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(alpha > 0 ? std::lround(std::clamp(lerp(ch) * 255.f / alpha, 0.f, 255.f)) : 0);
            o[3] = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.f, 255.f)));
        }
    return out;
}

// The icon's drawn part (its bounding box, without the file's clear margin) scaled to fill 0.96 of w x h, aspect kept, centred: the game's
// glyphs fill their sprite, Kenney's keys sit in a margin (and wide keys are half as tall), so they would show smaller than the game's.
std::vector<uint8_t> fitted(const Image& im, uint32_t w, uint32_t h) {
    uint32_t x0 = im.w, y0 = im.h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x)
            if (im.rgba[(static_cast<size_t>(y) * im.w + x) * 4 + 3] > 8) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    if (x1 <= x0 || y1 <= y0) return scaled(im, w, h);
    Image crop;
    crop.w = x1 - x0;
    crop.h = y1 - y0;
    crop.rgba.resize(static_cast<size_t>(crop.w) * crop.h * 4);
    for (uint32_t y = 0; y < crop.h; ++y)
        std::memcpy(&crop.rgba[static_cast<size_t>(y) * crop.w * 4], &im.rgba[(static_cast<size_t>(y0 + y) * im.w + x0) * 4], static_cast<size_t>(crop.w) * 4);
    const float k = std::min(w * 0.96f / crop.w, h * 0.96f / crop.h);
    const uint32_t sw = std::max(1u, static_cast<uint32_t>(std::lround(crop.w * k))), sh = std::max(1u, static_cast<uint32_t>(std::lround(crop.h * k)));
    const std::vector<uint8_t> small = scaled(crop, sw, sh);
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    const uint32_t ox = (w - std::min(w, sw)) / 2, oy = (h - std::min(h, sh)) / 2;
    for (uint32_t y = 0; y < sh && oy + y < h; ++y)
        std::memcpy(&out[(static_cast<size_t>(oy + y) * w + ox) * 4], &small[static_cast<size_t>(y) * sw * 4], static_cast<size_t>(std::min(sw, w - ox)) * 4);
    return out;
}

// A wanted icon: a file name, or "@move:" and four file names (the move keys, forward left back right) drawn as the WASD cluster.
std::vector<std::string> parts_of(const std::string& target) {
    if (target.compare(0, 6, "@move:") != 0) return {target};
    std::vector<std::string> v;
    size_t p = 6;
    for (size_t q; (q = target.find(',', p)) != std::string::npos; p = q + 1) v.push_back(target.substr(p, q - p));
    v.push_back(target.substr(p));
    return v.size() == 4 ? v : std::vector<std::string>{};
}

bool have_icon_locked(State& s, const std::string& target) {
    const std::vector<std::string> parts = parts_of(target);
    if (parts.empty()) return false;
    for (const std::string& n : parts)
        if (n.empty() || !image_locked(s, n)) return false;
    return true;
}

std::vector<uint8_t> pixels_locked(State& s, const std::string& target, uint32_t w, uint32_t h) {
    const std::vector<std::string> parts = parts_of(target);
    if (parts.size() == 1) return fitted(*image_locked(s, parts[0]), w, h);
    // forward on top in the middle, left / back / right below it. The icons carry a transparent margin (the key is about 3/4 of the
    // icon), so they are drawn at 0.45 of the glyph and 0.31 apart: the keys nearly touch and fill the glyph (the margins overlap or are cut).
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    const int k = static_cast<int>(w * 0.45f), step = static_cast<int>(w * 0.31f), cx = static_cast<int>(w) / 2, cy = static_cast<int>(h) / 2;
    const int ctr[4][2] = {{cx, cy - step / 2}, {cx - step, cy + step / 2}, {cx, cy + step / 2}, {cx + step, cy + step / 2}};
    for (int i = 0; i < 4; ++i) {
        const std::vector<uint8_t> key = scaled(*image_locked(s, parts[i]), static_cast<uint32_t>(k), static_cast<uint32_t>(k));
        for (int y = 0; y < k; ++y)
            for (int x = 0; x < k; ++x) {
                const int ox = ctr[i][0] - k / 2 + x, oy = ctr[i][1] - k / 2 + y;
                if (ox < 0 || oy < 0 || ox >= static_cast<int>(w) || oy >= static_cast<int>(h)) continue;
                const uint8_t* src = &key[(static_cast<size_t>(y) * k + x) * 4];
                uint8_t* dst = &out[(static_cast<size_t>(oy) * w + ox) * 4];
                const float sa = src[3] / 255.f, da = dst[3] / 255.f, oa = sa + da * (1 - sa);
                for (int ch = 0; ch < 3; ++ch)
                    dst[ch] = static_cast<uint8_t>(oa > 0 ? std::lround((src[ch] * sa + dst[ch] * da * (1 - sa)) / oa) : 0);
                dst[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
            }
    }
    return out;
}

// The title art's rectangle (kTitleArt, 72 x 72) with the icon in the art's style: white with its symbol cut out, over a soft shadow of the
// whole shape (the cut-out shows the shadow, like the art's A). Empty when the icon is missing.
std::vector<uint8_t> title_pixels_locked(State& s, const std::string& icon) {
    const Image* im = icon.empty() ? nullptr : image_locked(s, icon, false);
    if (!im) return {};
    const int W = static_cast<int>(kTitleArt.w), H = static_cast<int>(kTitleArt.h), g = static_cast<int>(kTitleGlyph);
    const std::vector<uint8_t> glyph = fitted(*im, kTitleGlyph, kTitleGlyph);
    const std::vector<uint8_t> outside = outside_of(glyph.data(), g, g);
    std::vector<float> shadow(static_cast<size_t>(W) * H, 0.f), tmp(shadow.size());
    for (int y = 0; y < g; ++y)
        for (int x = 0; x < g; ++x)
            shadow[static_cast<size_t>(y + kTitleGlyphY) * W + x + kTitleGlyphX] =
                outside[static_cast<size_t>(y) * g + x] ? glyph[(static_cast<size_t>(y) * g + x) * 4 + 3] / 255.f : 1.f;
    for (int pass = 0; pass < 2; ++pass)   // two box blurs of radius 3 in x and y: about the art's shadow (6 texels)
        for (int dir = 0; dir < 2; ++dir) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float sum = 0;
                    for (int k = -3; k <= 3; ++k) {
                        const int sx = dir ? x : x + k, sy = dir ? y + k : y;
                        if (sx >= 0 && sy >= 0 && sx < W && sy < H) sum += shadow[static_cast<size_t>(sy) * W + sx];
                    }
                    tmp[static_cast<size_t>(y) * W + x] = sum / 7.f;
                }
            shadow.swap(tmp);
        }
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float da = std::min(1.f, shadow[static_cast<size_t>(y) * W + x] * 0.4f);   // the art's shadow: near black, up to ~0.3 alpha
            const int gx = x - static_cast<int>(kTitleGlyphX), gy = y - static_cast<int>(kTitleGlyphY);
            const uint8_t* sp = gx >= 0 && gy >= 0 && gx < g && gy < g ? &glyph[(static_cast<size_t>(gy) * g + gx) * 4] : nullptr;
            const float sa = sp ? sp[3] / 255.f : 0.f, oa = sa + da * (1 - sa);
            uint8_t* o = &out[(static_cast<size_t>(y) * W + x) * 4];
            for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(oa > 0 ? std::lround(((sp ? sp[ch] : 0) * sa + 12.f * da * (1 - sa)) / oa) : 0);
            o[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
        }
    return out;
}

// ---- the sprite and its atlas ----
// The game's NVNtexture reachable from the sprite record: a breadth-first walk over the pointers it holds (the runtime's header names it).
uint64_t find_texture(uint64_t root) {
    std::vector<uint64_t> todo{root}, seen;
    for (size_t i = 0; i < todo.size() && i < 256; ++i) {
        const uint64_t node = todo[i];
        if (std::find(seen.begin(), seen.end(), node) != seen.end()) continue;
        seen.push_back(node);
        if (!mem::is_mapped(node, 0x110)) continue;
        for (uint64_t off = 0; off < 0x100; off += 8) {
            if (rd<uint32_t>(node + off + 8) == kGuestMagic) {
                gfx::GuestTexInfo ti;
                if (gfx::guest_texture_info(reinterpret_cast<const void*>(node + off), &ti) && ti.bc7) return node + off;
            }
            const uint64_t p = rd<uint64_t>(node + off);
            if (p >= 0x7000000000ull && p < 0x8000000000ull && (p & 7) == 0 && todo.size() < 512) todo.push_back(p);
        }
    }
    return 0;
}

#if NXRT_TESTTOOLS   // a dev aid: release packages carry no env-var dumps
// NXRT_PKP_KBM_DUMP=<folder>: the atlas's level 0 as raw BC7 blocks (<folder>/atlas_<id>_<w>x<h>.bc7), once per texture. Caller holds s.mu.
void dump_atlas(State& s, uint64_t tex) {
    const char* dir = std::getenv("NXRT_PKP_KBM_DUMP");
    if (!dir || !*dir || std::find(s.dumped.begin(), s.dumped.end(), tex) != s.dumped.end()) return;
    s.dumped.push_back(tex);
    gfx::GuestTexInfo ti;
    std::vector<uint8_t> blocks;
    uint32_t w = 0, h = 0;
    if (!gfx::guest_texture_info(reinterpret_cast<const void*>(tex), &ti) || !gfx::texture_read_bc7_level(reinterpret_cast<const void*>(tex), 0, &blocks, &w, &h))
        return;
    char path[512];
    std::snprintf(path, sizeof path, "%s/atlas_%u_%ux%u.bc7", dir, ti.id, w, h);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(blocks.data(), 1, blocks.size(), f);
        std::fclose(f);
        NXLOG_I(kTag, "kbm: atlas #%u (%ux%u, %u levels, %s) written to %s", ti.id, w, h, ti.levels, ti.srgb ? "sRGB" : "linear", path);
    }
}
#endif

// Looks up the sprite of a glyph code (through an action that shows it) and, for a code with a slot, where it is in the atlas. Caller holds s.mu.
// False: the sprite sheet is not loaded yet (the code stays unknown, to be looked up again).
bool resolve_code(::Ctx* c, State& s, uint32_t code, uint32_t action) {
    if (code >= static_cast<uint32_t>(kMaxCode)) return true;
    const uint64_t scratch = (c->x[31] - 0x400) & ~0xfull, sp = scratch - 0x100;
    std::memset(reinterpret_cast<void*>(scratch), 0, 16);
    call(c, s.base + kTagIcon, 0x30, action, scratch, scratch + 4, scratch + 6, sp);
    const uint32_t sprite = rd<uint32_t>(scratch);
    const uint64_t sheet = call(c, s.base + kSheet, scratch + 4, 0, 0, 0, 0, sp);
    const uint64_t rec = sheet ? call(c, s.base + kSheetSprite, sheet, sprite, 0, 0, 0, sp) : 0;
    if (!rec || !mem::is_mapped(rec, 88)) return false;
    s.code_slot[code] = -1;
    char name[65] = {};
    std::memcpy(name, reinterpret_cast<const void*>(rec), 64);
    const std::string nm(name);
    auto ends = [&](const char* suf) { const size_t n = std::strlen(suf); return nm.size() >= n && nm.compare(nm.size() - n, n, suf) == 0; };
    const int slot = code < static_cast<uint32_t>(kCodes) ? static_cast<int>(code) : ends("_ls") ? kSlotLS : ends("_rs") ? kSlotRS : -1;
    if (slot < 0) {
        NXLOG_I(kTag, "kbm prompt: code %u is sprite %s (left alone)", code, name);
        return true;
    }
    s.code_slot[code] = static_cast<int8_t>(slot);
    Glyph& g = s.glyph[slot];
    if (g.tried) return true;
    g.tried = true;
    const uint64_t quad = rd<uint64_t>(rec + 80);
    g.tex = find_texture(rec);
    gfx::GuestTexInfo ti;
    if (!g.tex || !quad || !mem::is_mapped(quad + 24, 32) || !gfx::guest_texture_info(reinterpret_cast<const void*>(g.tex), &ti)) {
        NXLOG_W(kTag, "kbm prompt %s: sprite %s, atlas texture not found", kSlotName[slot], name);
        g.tex = 0;
        return true;
    }
#if NXRT_TESTTOOLS
    dump_atlas(s, g.tex);
#endif
    float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    for (int k = 0; k < 4; ++k) {
        const float u = rd<float>(quad + 24 + 8 * k), v = rd<float>(quad + 28 + 8 * k);
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
    g.x = static_cast<uint32_t>(std::lround(u0 * ti.width));
    g.y = static_cast<uint32_t>(std::lround(v0 * ti.height));
    g.w = static_cast<uint32_t>(std::lround(u1 * ti.width)) - g.x;
    g.h = static_cast<uint32_t>(std::lround(v1 * ti.height)) - g.y;
    const bool ok = ((g.x | g.y | g.w | g.h) & 3u) == 0 && g.w && g.h && g.w <= 256 && g.h <= 256;
    NXLOG_I(kTag, "kbm prompt %s (code %u): sprite %s %ux%u at (%u, %u) of texture #%u (%ux%u, %u levels)%s", kSlotName[slot], code, name, g.w,
            g.h, g.x, g.y, ti.id, ti.width, ti.height, ti.levels, ok ? "" : ", not 4x4 aligned: text only");
    if (!ok) g.tex = 0;
    return true;
}

// Every code of the input manager's action table, once: the HUD draws the same atlas sprites without text tags. Caller holds s.mu.
// Stays unresolved (to be tried again) while the sprite sheet is not loaded.
void resolve_all(::Ctx* c, State& s, uint64_t mgr) {
    const uint64_t tab = rd<uint64_t>(mgr + kActionButtons), end = rd<uint64_t>(mgr + kActionButtons + 8);
    if (!tab || end <= tab || end - tab > 4 * 0x400 || !mem::is_mapped(tab, end - tab)) return;
    bool ready = true;
    for (uint32_t action = 1; action < (end - tab) / 4 && action <= 0x1ff && ready; ++action) {
        if (action == 255) continue;
        const uint32_t code = rd<uint32_t>(tab + 4ull * action) & 0xff;
        if (code < static_cast<uint32_t>(kMaxCode) && s.code_slot[code] == -2) ready = resolve_code(c, s, code, action);
    }
    s.all_resolved = ready;
}

// What every glyph should show now, from the current settings and the prompt style that shows. Caller holds s.mu.
void compute_want_locked(State& s) {
    const play::PromptStyle eff = play::prompts_effective_style();
    s.eff = eff;
    std::array<Want, kSlots> want;
    std::string title;
    if (eff == play::PromptStyle::Keyboard) {
        for (int i = 0; i < kCodes; ++i) {
            play::InputId in;
            if (input_for_bits(s.settings, kCodeBit[i], &in)) want[i] = {label_of(in), icon_of(in)};
        }
        // the sticks: the left one moves (the four move keys, "WASD"), the right one turns the camera (the mouse)
        std::string move, move_icon = "@move:";
        for (play::Act a : {play::Act::MoveForward, play::Act::MoveLeft, play::Act::MoveBack, play::Act::MoveRight}) {
            play::InputId in;
            const bool bound = input_for_act(s.settings, a, &in);
            const std::string n = bound ? play::input_name(in) : std::string(), ic = bound ? icon_of(in) : std::string();
            move = n.size() == 1 && (!move.empty() || a == play::Act::MoveForward) ? move + n : std::string();
            move_icon = ic.empty() || move_icon.empty() ? std::string() : move_icon + (a == play::Act::MoveForward ? "" : ",") + ic;
        }
        want[kSlotLS] = {move.size() == 4 ? move : "Move", move_icon};
        want[kSlotRS] = {"Mouse", "mouse_move"};
        // the title art's A: the Enter key when one is bound to A (the menu key, though E is listed first), else whatever A shows
        title = want[5].icon;
        for (const play::Binding& b : s.settings.bindings) {
            const play::Output& o = s.settings.out[static_cast<int>(b.act)];
            if (b.trig != play::Trig::Press || b.in.kind != play::InKind::Key || o.buttons != kCodeBit[5] || o.lx || o.ly || o.rx || o.ry) continue;
            const std::string n = play::input_name(b.in);
            if ((n == "Return" || n == "Keypad Enter") && !icon_of(b.in).empty()) {
                title = icon_of(b.in);
                break;
            }
        }
    } else if (play::prompt_style_is_pad(eff)) {
        // the pad's own button that presses each Switch button, following [padmap] and [pad] label_abxy; the stick clicks and the
        // stick glyphs have no pad icon and keep the game's own
        for (int i = 0; i < kCodes; ++i)
            want[i].icon = play::pad_pack_name(eff, s.settings.pad, s.settings.pad.label_abxy, kCodeBit[i]);
        title = want[5].icon;
    }
    s.want = want;
    s.title_icon = title;
}

// Puts the wanted icon (or the game's pixels) into every glyph that was found. Caller holds s.mu.
void sync_glyphs_locked(State& s) {
    compute_want_locked(s);
    const bool patch = s.enabled && s.eff != play::PromptStyle::Switch;
    for (int i = 0; i < kSlots; ++i) {
        Glyph& g = s.glyph[i];
        if (!g.tex) continue;
        std::string target = patch ? s.want[i].icon : std::string();
        if (!target.empty() && !have_icon_locked(s, target)) {
            const std::string fb = play::pad_pack_fallback(target);   // an older pack without the d-pad directions: the whole d-pad
            target = !fb.empty() && have_icon_locked(s, fb) ? fb : std::string();
        }
        if (target == g.shown) continue;
        const void* tex = reinterpret_cast<const void*>(g.tex);
        if (!g.shown.empty()) {
            gfx::texture_put_bc7_blocks(tex, g.x, g.y, g.w, g.h, g.saved.data(), g.saved.size());
            g.shown.clear();
        }
        if (!target.empty()) {
            const std::vector<uint8_t> px = pixels_locked(s, target, g.w, g.h);
            if (gfx::texture_patch_bc7(tex, g.x, g.y, g.w, g.h, px.data(), &g.saved)) {
                g.shown = target;
                s.patches.fetch_add(1, std::memory_order_relaxed);
                NXLOG_I(kTag, "kbm prompt %s: icon %s", kSlotName[i], target.c_str());
            } else {
                gfx::GuestTexInfo fi;
                if (gfx::guest_texture_info(tex, &fi))
                    NXLOG_W(kTag, "kbm prompt %s: the atlas patch failed (texture #%u, NVN format %u, %ux%u, %u levels, %s%s)", kSlotName[i], fi.id,
                            fi.format, fi.width, fi.height, fi.levels, fi.bc7 ? "BC7" : "not BC7", fi.srgb ? ", sRGB" : "");
                else
                    NXLOG_W(kTag, "kbm prompt %s: the atlas patch failed (texture is gone)", kSlotName[i]);
                g.tex = 0;
            }
        }
    }
}

int hook_tag(::Ctx* c, uint64_t, void*) {
    State& s = st();
    {
        // every tag type once, with its arguments (which texts carry glyphs other than the action tag 0x30)
        const uint32_t type = static_cast<uint32_t>(c->x[1] & 0xff);
        std::lock_guard<std::mutex> lk(s.mu);
        if (!(s.types_logged[type / 64] & (1ull << (type % 64)))) {
            s.types_logged[type / 64] |= 1ull << (type % 64);
            char arg[40] = {};
            for (int i = 0; i < 39 && c->x[2] && mem::is_mapped(c->x[2] + 2 * i, 2); ++i) {
                const uint16_t ch = rd<uint16_t>(c->x[2] + 2 * i);
                if (!ch) break;
                arg[i] = ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : '?';
            }
            NXLOG_I(kTag, "kbm: text tag type 0x%02x first seen, arguments \"%s\"", type, arg);
        }
    }
    if ((c->x[1] & 0xff) != 0x30) return 0;
    if (!s.enabled) return 0;
    s.tags.fetch_add(1, std::memory_order_relaxed);
    uint32_t action = 0;
    if (!c->x[2]) return 0;   // no string: no digits, and the hook would read address 0
    for (uint64_t p = c->x[2]; p < c->x[2] + 16; p += 2) {
        const uint16_t ch = rd<uint16_t>(p);
        if (ch < '0' || ch > '9') break;
        action = action * 10 + (ch - '0');
    }
    if (action == 0 || action > 0x1ff || action == 255) return 0;
    const uint64_t mgr = rd<uint64_t>(s.base + kInputMgr);
    if (!mgr) return 0;
    const uint32_t code = static_cast<uint32_t>(call(c, s.base + kActionButton, mgr, action, 1, 0)) & 0xff;
    std::string label;
    bool icon = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.all_resolved) resolve_all(c, s, mgr);
        if (code < static_cast<uint32_t>(kMaxCode) && s.code_slot[code] == -2) resolve_code(c, s, code, action);
        const int slot = code < static_cast<uint32_t>(kMaxCode) ? s.code_slot[code] : -1;
        sync_glyphs_locked(s);
        if (slot >= 0 && s.eff == play::PromptStyle::Keyboard) {   // the text label is only for the keyboard style
            label = s.want[slot].label;
            icon = !s.glyph[slot].shown.empty();
        }
    }
    if (label.empty() || icon) return 0;   // the game's glyph (patched with the key's icon, or a pad button no key is bound to)
    // the key's name in the game's font: the layout's add-character call, then the handler call does nothing
    const uint64_t layout = c->x[0];
    const uint64_t add_char = rd<uint64_t>(rd<uint64_t>(layout) + 16);
    for (char ch : "[" + label + "]") call(c, add_char, layout, static_cast<uint8_t>(ch), 0xffffffffull);
    c->x[8] = s.base + kEmpty;
    s.texts.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// The glyph sprites are looked up (and patched) once the game's sprite sheets are loaded, without waiting for the first text tag: the title
// screen and the HUD draw the same atlas sprites with no tag. Also follows a change of the shown prompt style between tags.
void frame(::Ctx* c) {
    State& s = st();
    if (!s.enabled || (++s.frames % 30) != 0) return;
    const play::PromptStyle eff = play::prompts_effective_style();
    std::vector<uint8_t> title;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        bool ready = s.all_resolved;
        if (!ready && eff != play::PromptStyle::Switch && s.frame_tries < 400) {
            const uint64_t mgr = rd<uint64_t>(s.base + kInputMgr);
            if (mgr && rd<uint64_t>(s.base + kTables) && rd<uint64_t>(s.base + kUiMgr)) {
                ++s.frame_tries;
                resolve_all(c, s, mgr);
                ready = s.all_resolved;
                if (ready) NXLOG_I(kTag, "kbm: glyph sprites found at game frame %u", s.frames);
            }
        }
        // not only once everything is found: the title draws (-) Settings while other sprites (sticks) are not loaded yet, so every
        // glyph already found gets its icon now. Skipped while none was found (nothing to patch, no per-frame work)
        if (ready || std::any_of(s.glyph.begin(), s.glyph.end(), [](const Glyph& g) { return g.tex != 0; })) sync_glyphs_locked(s);
        if (eff != play::PromptStyle::Switch && s.frames % 60 == 0) {
            // #280: the scan below walks every registered texture under the registry lock. It is meant for the title screen, but ran once a
            // second for the whole session. It now backs off while nothing changes: the interval doubles (60 .. 600 frames) after two scans in
            // a row that found nothing new, and goes back to 60 as soon as the registry changed (a title screen loads textures) or the prompt
            // style did.
            const size_t reg = gfx::texture_registry_count();
            if (reg != s.scan_registry || eff != s.scan_style) {
                s.scan_registry = reg;
                s.scan_style = eff;
                s.scan_every = 60;
                s.scan_empty = 0;
                s.scan_last = 0;
            }
            if (s.scan_last != 0 && s.frames - s.scan_last < s.scan_every) return;
            if (s.title_px_icon != s.title_icon) {
                s.title_px = title_pixels_locked(s, s.title_icon);
                s.title_px_icon = s.title_icon;
            }
            title = s.title_px;
        }
    }
    if (title.empty()) return;
    // a new title art texture (each time the title screen loads) is drawn over once; textures looked at before are skipped
    const size_t checked_before = st().title_checked.size();
    const auto t0 = std::chrono::steady_clock::now();
    const int n = gfx::texture_patch_bc7_matching(kTitleArt, title.data(), &st().title_checked);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.scan_last = s.frames;
        if (n > 0 || st().title_checked.size() != checked_before) {
            s.scan_empty = 0;
            s.scan_every = 60;
        } else if (++s.scan_empty >= 2) {
            s.scan_every = std::min<uint32_t>(s.scan_every * 2, 600);
        }
        if (ms > 2.0 && s.scan_slow_logged < 20) {
            ++s.scan_slow_logged;
            NXLOG_I(kTag, "kbm: title art scan took %.2f ms over %zu textures (next in %u game frames)", ms, s.scan_registry, s.scan_every);
        }
    }
    if (n)
        NXLOG_I(kTag, "kbm: title screen art: PRESS (A) BUTTON now shows %s (%d texture%s)", s.title_px_icon.c_str(), n, n == 1 ? "" : "s");
}

std::string exe_dir() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t cut = p.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : p.substr(0, cut + 1);
}

// main thread: the key names for every pad button
void settings_changed(const play::Settings& set) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.settings = set;
    compute_want_locked(s);
}

void install(const FeatureInstall& in) {
    State& s = st();
    s.base = in.base;
    s.enabled = settings::get_bool(settings::Key::KbmIcons);
    s.dir = exe_dir() + "assets/kbm_icons/";
    in.hooks.add_mid(in.base + kTagCall, &hook_tag);
    NXLOG_I(kTag, "button prompts %s (keyboard icons from %s, pad icons from the prompt icon pack; the style is [prompts] style)",
            s.enabled ? "on" : "off ([pkp] kbm_icons = 0)", s.dir.c_str());
    settings_changed(in.settings);
}

void status_text(char* buf, size_t n) {
    std::snprintf(buf, n, " kbm_tags=%llu kbm_texts=%llu kbm_patches=%llu", static_cast<unsigned long long>(st().tags.load()),
                  static_cast<unsigned long long>(st().texts.load()), static_cast<unsigned long long>(st().patches.load()));
}

}  // namespace

const Feature kKbmFeature = {
    .name = "kbm",
    .install = &install,
    .game_frame = &frame,
    .settings_changed = &settings_changed,
    .status_text = &status_text,
};

}  // namespace nxrt::games::pkp
