// SPDX-License-Identifier: GPL-3.0-or-later
#include "prompt_resolve.h"
#include "nxrt/host_device.h"

#include <cctype>
#include <vector>

#include "nxrt/game.h"
#include "nxrt/input_source.h"
#include "nxrt/text.h"
#include "play/pad_map.h"

namespace nxrt::play {

namespace {

using text::lower;

// The Switch button bit an icon stands for; the d-pad icon stands for "up" (any direction will do for the glyph of the whole cross).
uint64_t button_bit(Btn b) {
    using namespace hid;
    switch (b) {
        case Btn::A: return btn(kBtnA);
        case Btn::B: return btn(kBtnB);
        case Btn::X: return btn(kBtnX);
        case Btn::Y: return btn(kBtnY);
        case Btn::L: return btn(kBtnL);
        case Btn::R: return btn(kBtnR);
        case Btn::ZL: return btn(kBtnZL);
        case Btn::ZR: return btn(kBtnZR);
        case Btn::Plus: return btn(kBtnPlus);
        case Btn::Minus: return btn(kBtnMinus);
        case Btn::DPad: return btn(kBtnUp);
        default: return 0;
    }
}

// The pad source that presses Switch button `bit`, in this order: the lowest source whose remap is exactly that bit; the lowest source
// with a remap that contains it; the source whose built-in button it is, if that source is not remapped. False: nothing presses it.
bool source_for_button(const PadSettings& pad, bool label_abxy, uint64_t bit, PadSrc* out) {
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (pad.remap[i] == bit) {
            *out = static_cast<PadSrc>(i);
            return true;
        }
    }
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (pad.remap[i] != kPadDefault && (pad.remap[i] & bit)) {
            *out = static_cast<PadSrc>(i);
            return true;
        }
    }
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (pad.remap[i] == kPadDefault && pad_default_buttons(static_cast<PadSrc>(i), label_abxy) == bit) {
            *out = static_cast<PadSrc>(i);
            return true;
        }
    }
    return false;
}

// The icon sets of the pad styles: one pack entry per PadSrc, named by the physical button (South is the Xbox A, the PlayStation cross,
// the Nintendo B), so it is the same whatever the label question says; the label question only decides which source a Switch button
// comes from. "" = the controller has no icon for that source (the game keeps its own); the stick clicks and the single d-pad
// directions have none in any set (the d-pad prompt is the set's whole cross, `dpad`, and pad_pack_name adds the direction). The entries
// are built by runtime/scripts/build_prompt_pack.py (PAD_SETS: same prefixes and names). Columns: South East West North,
// LeftShoulder RightShoulder LeftTrigger RightTrigger, Start Back, LeftStick RightStick, DpadUp DpadDown DpadLeft DpadRight,
// RightPaddle1 LeftPaddle1 RightPaddle2 LeftPaddle2, Touchpad TouchpadRight, Guide Misc1. Every set lists all kPadSrcCount entries (a
// missing one would be a null name).
struct PadIconSet {
    PromptStyle style;
    const char* dpad;
    const char* names[kPadSrcCount];
};
constexpr PadIconSet kPadSets[] = {
    {PromptStyle::Xbox, "xbox/dpad",
     {"xbox/a", "xbox/b", "xbox/x", "xbox/y", "xbox/lb", "xbox/rb", "xbox/lt", "xbox/rt", "xbox/menu", "xbox/view",
      "", "", "", "", "", "", "", "", "", "", "", "", "", ""}},
    {PromptStyle::PlayStation, "ps/dpad",
     {"ps/cross", "ps/circle", "ps/square", "ps/triangle", "ps/l1", "ps/r1", "ps/l2", "ps/r2", "ps/options", "ps/create",
      "", "", "", "", "", "", "", "", "", "", "", "", "", ""}},
    // the four rear grips and the trackpad clicks (Steam Controller 2026), the Steam button and the quick-access button
    {PromptStyle::Steam, "steam/dpad",
     {"steam/a", "steam/b", "steam/x", "steam/y", "steam/lb", "steam/rb", "steam/lt", "steam/rt", "steam/menu", "steam/view",
      "", "", "", "", "", "", "steam/r4", "steam/l4", "steam/r5", "steam/l5", "steam/lp", "steam/rp", "steam/guide", "steam/qam"}},
    // the rear buttons R4 L4 R5 L5 and the trackpads
    {PromptStyle::SteamDeck, "deck/dpad",
     {"deck/a", "deck/b", "deck/x", "deck/y", "deck/l1", "deck/r1", "deck/l2", "deck/r2", "deck/menu", "deck/view",
      "", "", "", "", "", "", "deck/r4", "deck/l4", "deck/r5", "deck/l5", "deck/lp", "deck/rp", "steam/guide", "steam/qam"}},
    // Nintendo layout: B at the bottom, A at the right, Y at the left, X at the top; Plus is Start, Minus is Back
    {PromptStyle::NintendoSwitch, "ns1/dpad",
     {"ns1/b", "ns1/a", "ns1/y", "ns1/x", "ns1/l", "ns1/r", "ns1/zl", "ns1/zr", "ns1/plus", "ns1/minus",
      "", "", "", "", "", "", "", "", "", "", "", "", "", ""}},
    {PromptStyle::Switch2, "ns2/dpad",   // + the Switch 2 Pro Controller's rear GR and GL
     {"ns2/b", "ns2/a", "ns2/y", "ns2/x", "ns2/l", "ns2/r", "ns2/zl", "ns2/zr", "ns2/plus", "ns2/minus",
      "", "", "", "", "", "", "ns2/gr", "ns2/gl", "ns2/gr", "ns2/gl", "", "", "", ""}},
    {PromptStyle::WiiU, "wiiu/dpad",
     {"wiiu/b", "wiiu/a", "wiiu/y", "wiiu/x", "wiiu/l", "wiiu/r", "wiiu/zl", "wiiu/zr", "wiiu/plus", "wiiu/minus",
      "", "", "", "", "", "", "", "", "", "", "", "", "", ""}},
    {PromptStyle::SteamFrame, "frame/dpad",   // the grips like the Steam Controller's
     {"frame/a", "frame/b", "frame/x", "frame/y", "frame/l1", "frame/r1", "frame/l2", "frame/r2", "frame/menu", "frame/view",
      "", "", "", "", "", "", "frame/rg", "frame/lg", "frame/rg", "frame/lg", "", "", "", ""}},
    // A, B, the triggers and the trackpad press; no X, Y, shoulders, Start or Back. The d-pad is the left stick (SteamVR's usual binding).
    {PromptStyle::ValveIndex, "index/dpad",
     {"index/a", "index/b", "", "", "", "", "index/lt", "index/rt", "", "",
      "", "", "", "", "", "", "", "", "", "", "index/track", "index/track", "", ""}},
    // Gestures: tap confirms, a two-finger tap goes back, hold and double tap for the other two, swipes for the shoulders, pinch for the
    // triggers, a two-finger double tap and hold for Start and Back, a one-finger swipe for the d-pad.
    {PromptStyle::Touch, "touch/dpad",
     {"touch/tap", "touch/two", "touch/hold", "touch/double", "touch/swipe_left", "touch/swipe_right", "touch/zoom_out", "touch/zoom_in",
      "touch/two_double", "touch/two_hold", "", "", "", "", "", "", "", "", "", "", "", "", "", ""}},
};

const PadIconSet* pad_set(PromptStyle style) {
    for (const PadIconSet& s : kPadSets)
        if (s.style == style) return &s;
    return nullptr;
}

// Pad icons: the button of the style's controller that presses the Switch button ("" when nothing presses it or the set has no icon
// for that source: the game keeps its icon). The d-pad icon is always the whole d-pad.
const char* pad_name(const PadIconSet& set, const PadSettings& pad, bool label_abxy, Btn b) {
    if (b == Btn::DPad) return set.dpad;
    PadSrc src = PadSrc::South;
    if (!source_for_button(pad, label_abxy, button_bit(b), &src)) return "";
    return set.names[static_cast<int>(src)];
}

// Does binding b apply in context c? A binding of c applies; an `all` binding applies unless the same input has a binding of c itself.
bool binding_applies(const Settings& s, const Binding& b, Ctx c) {
    if (b.act == Act::None) return false;
    if (c == Ctx::All) return b.ctx == Ctx::All;
    if (b.ctx == c) return true;
    if (b.ctx != Ctx::All) return false;
    for (const Binding& o : s.bindings)
        if (o.ctx == c && o.in == b.in) return false;
    return true;
}

}  // namespace

const PromptGame* prompt_game() { return game::info().prompts; }

namespace {
const PromptIcon* icon_of(Icon i) {
    const PromptGame* g = prompt_game();
    return g && g->icons ? &g->icons[static_cast<int>(i)] : nullptr;
}
}  // namespace

Btn icon_button(Icon i) { const PromptIcon* p = icon_of(i); return p ? p->btn : Btn::None; }
bool icon_pressed(Icon i) { const PromptIcon* p = icon_of(i); return p && p->pressed; }
const char* icon_asset_name(Icon i) { const PromptIcon* p = icon_of(i); return p ? p->asset : ""; }
const char* icon_asset_uuid(Icon i) { const PromptIcon* p = icon_of(i); return p ? p->uuid : ""; }
const char* icon_pak_file(Icon i) { const PromptIcon* p = icon_of(i); return p ? p->pak : ""; }
const char* icon_front_uuid(Icon i) { const PromptIcon* p = icon_of(i); return p ? p->front_uuid : nullptr; }

std::string kbm_pack_name(const InputId& id) {
    const std::string n = lower(input_name(id));
    return n.empty() ? std::string() : "kbm/" + n;
}

bool input_for_button(const Settings& s, Ctx ctx, Btn b, InputId* out) {
    const uint64_t bit = button_bit(b);
    if (!bit) return false;
    // actions that do exactly this button: the named game actions first, the raw btn_ ones last
    std::vector<Act> cands;
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 1; a < kActCount; ++a) {
            const bool raw = a >= static_cast<int>(Act::BtnA);
            if ((pass == 0) == raw) continue;
            const Output& o = s.out[a];
            if (o.buttons == bit && o.lx == 0 && o.ly == 0 && o.rx == 0 && o.ry == 0) cands.push_back(static_cast<Act>(a));
        }
    // the current context first, then the others (a menu button is still worth showing while playing)
    const Ctx order[] = {ctx, Ctx::Menu, Ctx::Play, Ctx::Map, Ctx::All};
    for (Ctx c : order)
        for (Act a : cands)
            for (const Binding& bd : s.bindings)
                if (bd.act == a && binding_applies(s, bd, c)) {
                    *out = bd.in;
                    return true;
                }
    return false;
}

namespace {
// The pack entry a style shows for Switch button b: the pad set's button (pad remap of `settings`, or the built-in mapping without
// settings), or for the keyboard the key bound to it in ctx. "" = the game's own icon.
std::string button_icon(PromptStyle style, const Settings* settings, bool label_abxy, Ctx ctx, Btn b) {
    const PadSettings no_pad{};
    if (const PadIconSet* set = pad_set(style)) return pad_name(*set, settings ? settings->pad : no_pad, label_abxy, b);
    if (style != PromptStyle::Keyboard || !settings) return {};
    InputId id;
    if (!input_for_button(*settings, ctx, b, &id)) return {};
    return kbm_pack_name(id);
}
}  // namespace

std::string resolve_icon(const ResolveIn& in, Icon icon) {
    // The map's stick icons: the mouse buttons that drive the map (left drag rotates, middle drag moves). Only keyboard style on the map.
    if (icon == Icon::LStick || icon == Icon::RStick) {
        if (in.style != PromptStyle::Keyboard || in.ctx != Ctx::Map) return {};
        return icon == Icon::LStick ? "kbm/mouse1" : "kbm/mouse2";
    }
    const std::string base = button_icon(in.style, in.settings, in.label_abxy, in.ctx, icon_button(icon));
    if (base.empty()) return {};
    return icon_pressed(icon) ? base + "_pressed" : base;
}

std::vector<PromptPreviewItem> prompt_preview(PromptStyle style, const Settings& s) {
    static constexpr struct {
        const char* label;
        Btn btn;
    } kButtons[] = {{"A", Btn::A},   {"B", Btn::B},   {"X", Btn::X},       {"Y", Btn::Y},         {"L", Btn::L},           {"R", Btn::R},
                    {"ZL", Btn::ZL}, {"ZR", Btn::ZR}, {"Plus", Btn::Plus}, {"Minus", Btn::Minus}, {"D-pad", Btn::DPad}};
    std::vector<PromptPreviewItem> r;
    if (!prompt_style_is_pad(style) && style != PromptStyle::Keyboard) return r;
    for (const auto& b : kButtons) r.push_back({b.label, button_icon(style, &s, s.pad.label_abxy, Ctx::Menu, b.btn)});
    return r;
}

std::string pad_pack_name(PromptStyle style, const PadSettings& pad, bool label_abxy, uint64_t switch_bit) {
    const PadIconSet* set = pad_set(style);
    if (!set) return {};
    if (switch_bit == 0 || (switch_bit & (switch_bit - 1)) != 0) return {};   // exactly one button bit
    using namespace hid;
    // one d-pad direction: the cross with that arm lit (the prompt names a direction); pad_pack_fallback() gives the whole cross
    const char* dir = switch_bit == btn(kBtnUp) ? "_up" : switch_bit == btn(kBtnDown) ? "_down" : switch_bit == btn(kBtnLeft) ? "_left"
                    : switch_bit == btn(kBtnRight) ? "_right" : nullptr;
    if (dir) return std::string(set->dpad) + dir;
    Btn b = Btn::None;
    for (int i = 0; i < static_cast<int>(Btn::kCount); ++i)
        if (button_bit(static_cast<Btn>(i)) == switch_bit) {
            b = static_cast<Btn>(i);
            break;
        }
    if (b == Btn::None) return {};
    return pad_name(*set, pad, label_abxy, b);
}

bool prompt_style_is_pad(PromptStyle style) { return pad_set(style) != nullptr; }

std::string pad_pack_fallback(const std::string& name) {
    for (const std::string dir : {"_up", "_down", "_left", "_right"}) {
        const std::string tail = "/dpad" + dir;
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0)
            return name.substr(0, name.size() - dir.size());
    }
    return {};
}

PromptStyle auto_style(LastDevice last, int last_pad_kind, int connected_pad_kind) {
    auto from_kind = [](int k) {
        switch (k) {
            case 2: return PromptStyle::PlayStation;
            case 3: return PromptStyle::Switch;
            case 4: return host_is_handheld() ? PromptStyle::SteamDeck : PromptStyle::Steam;   // SDL calls the Deck a Steam pad; a handheld host is the Deck
            default: return PromptStyle::Xbox;     // Xbox pads and every pad that is neither a PlayStation, a Switch nor a Steam pad
        }
    };
    // A handheld is played with its own controls: its trackpad, touchscreen and on-screen keyboard reach the game as mouse and keyboard
    // input, which would switch the prompts to keyboard keys (the title screen showed Enter for A on a Legion Go S). There the built-in
    // pad (Xbox layout) decides; [prompts] style keyboard still shows keys.
    if (host_is_handheld()) return last == LastDevice::Pad ? from_kind(last_pad_kind) : connected_pad_kind >= 0 ? from_kind(connected_pad_kind) : PromptStyle::Xbox;
    switch (last) {
        case LastDevice::KeyboardMouse: return PromptStyle::Keyboard;
        case LastDevice::Pad: return from_kind(last_pad_kind);
        // Nothing used yet: a pad that is merely connected does not decide. A controller paired to a desktop sat there unused and
        // showed a keyboard player its Switch glyphs (the first pad press still switches to them, the first key press switches back).
        default: return PromptStyle::Keyboard;
    }
}

}  // namespace nxrt::play
