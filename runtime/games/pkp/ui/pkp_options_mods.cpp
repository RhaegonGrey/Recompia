// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the Mods tab (pkp_options_model.h): the Modtopia mods of the mods host (nxrt/mods.h), through the core's own list and
// settings API (modtopia/modtopia_host.h) only. The rows are those of one mod at a time:
//   0  Mod       a slider over the mods in load order (then the rest); moving it shows that mod's rows (Use::Rows)
//   1  On/Off    the player's switch, saved when the screen commits (mt_loader_set_enabled + mt_loader_save_list), in force at the next
//                start. A code mod whose code the player has not allowed (MT_MOD_NEW_CODE) is held by the host and cannot be switched on
//                here: the row only shows its state; code is allowed in the launcher's Mods page only. A mod the launcher keeps on
//                (mods::locked, the launcher's [mods] locked) shows On read only while it is on: it is unlocked in the launcher's Mods page.
//   2  Status    read only: its state this run, or the saved switch when it differs ("Off from next start", "On from next start"); the
//                description: the core's message for it (why it is off, broken or held) or its description, what it holds (code, files or
//                LayeredFS files, patches), whether it changes gameplay or looks only, the hosts it talks to and its package checksum
//   3  Page      only when it has more than 10 settings: which 9 the rows below show (Use::Rows)
//   3.. its settings: bool and choice (2..4 names) as choices, longer choices and numbers as sliders (a number's range coarsened to at
//      most 255 positions), a key through the key capture (keyboard keys only: the names the mods' key settings store), text read only.
//      A setting reaches the mod at once (mt_setting_set_*, the core saves config/<id>.toml and tells the mod); a discard puts back the
//      value it had when the screen opened or last committed. A setting the launcher keeps on (mods::setting_locked) is read only.
// No mods folder or no mods: one read-only row that says so. Strings from the core are copied: they live until the core's next change.
// The hooks run on the game's thread, as does the core's frame; a captured key reaches the core on play mode's main thread, which HOST.md
// allows ("settings changes made in a menu reach the mod on the menu's thread").
#include "ui/pkp_options_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "modtopia/modtopia_host.h"
#include "nxrt/mods.h"
#include "nxrt/text.h"

namespace nxrt::games::pkp::options_detail {
namespace {

constexpr int kModRow = 0, kSwitchRow = 1, kFirstSetting = 3;   // row 2: Status
constexpr int kSettingRows = kDirtySlot - kFirstSetting;   // 10 settings, or a Page row and 9
constexpr int kPageSize = kSettingRows - 1;
constexpr int kMaxMods = 256;                              // the Mod row's slider positions
constexpr int kMaxPositions = 255;                         // a slider's (record bytes)
constexpr size_t kShortText = 40;                          // a text setting's value as a choice name

enum class Kind : uint8_t { NoMods, Mod, Switch, Status, Page, Setting };

// One mod as the tab knows it since the screen opened (or last committed).
struct Mod {
    std::string id, title, state, about;    // title: name and version; about: the core's message, else the description
    std::string facts;                      // what it holds and does, the network hosts and the checksum (tags())
    bool held = false;                      // code the player has not allowed: the switch cannot turn it on
    bool locked = false;                    // the launcher keeps it on: the switch cannot turn it off
    bool on_at_start = false;               // the switch this run started with
    int on_cur = 0, on_orig = 0;            // the switch: as shown, as saved
    std::vector<double> num;                // each setting's value in force (bool, number, choice index)
    std::vector<std::string> text;          // ... and of a text or key setting
};

// A row on screen.
struct Line {
    Kind kind = Kind::NoMods;
    Shape shape;
    int setting = -1;                       // Kind::Setting: the mod's setting index
    int type = 0;                           // ... its MT_SET_*
    bool restart = false;
    double lo = 0, step = 1, hi = 0;        // a number: the value of each position, lo + pos * step, at most hi
    std::string label, desc;
    std::vector<std::string> names;         // a choice's names, a long choice's slider texts, a read-only row's value
};

std::string str(const char* s) { return s ? s : ""; }

// Its state this run, or the saved switch when that differs from it (in force at the next start).
const char* state_word(const MtModInfo& m, bool on_at_start, bool on_saved) {
    if ((m.flags & MT_MOD_HAS_CODE) && (m.flags & MT_MOD_NEW_CODE)) return "Not allowed";
    if (m.state == MT_STATE_ERROR) return "Error";
    if (m.state == MT_STATE_OK) return on_saved ? "On" : "Off from next start";
    if (!on_at_start) return on_saved ? "On from next start" : "Off";
    return "Not loaded";
}

// What a mod holds and does, for its Status row: "Has code, LayeredFS files. Changes gameplay (guessed from its files). Talks to
// a.example. Checksum (sha256): ..." (the checksum is the one players compare with the author's).
std::string tags(const MtModInfo& m) {
    constexpr size_t kHosts = 120;   // the hosts' share of the description
    const int f = m.flags;
    std::string has;
    const auto add = [&has](const char* s) { has += (has.empty() ? "" : ", ") + std::string(s); };
    if (f & MT_MOD_HAS_CODE) add(f & MT_MOD_NEW_CODE ? "new code" : "code");
    if (f & MT_MOD_LAYEREDFS) add("LayeredFS files");
    else if (f & MT_MOD_HAS_DATA) add("files");
    if (f & MT_MOD_HAS_PATCHES) add("patches");
    std::string out = has.empty() ? "" : "Has " + has + ". ";
    out += f & MT_MOD_GAMEPLAY ? "Changes gameplay: every player of a Link Play session needs it" : "Looks only";
    if (f & MT_MOD_KIND_GUESSED) out += " (guessed from its files)";
    out += ".";
    if ((f & MT_MOD_NETWORK) && m.network && *m.network) {
        std::string hosts = m.network;
        if (hosts.size() > kHosts) hosts = hosts.substr(0, nxrt::text::utf8_cut(hosts, kHosts - 3)) + "...";
        out += " Talks to " + hosts + ".";
    }
    if (m.sha256 && *m.sha256) out += std::string(" Checksum (sha256): ") + m.sha256;
    return out;
}

// The Status row's description: the message or description, cut so the facts after it fit the screen's description (512 bytes).
std::string status_desc(const Mod& m) {
    constexpr size_t kDesc = 500;
    std::string about = m.about.empty() ? m.title : m.about;
    const size_t room = m.facts.size() + 2 < kDesc ? kDesc - m.facts.size() - 2 : 0;
    if (about.size() > room) about = room > 3 ? about.substr(0, nxrt::text::utf8_cut(about, room - 3)) + "..." : std::string();
    if (about.empty()) return m.facts;
    const char end = about.back();
    return about + (end == '.' || end == '!' || end == '?' ? " " : ". ") + m.facts;
}

// A key setting's capture: the key name as the core stores it ("F3", "PageUp": SDL's scancode names, play::input_name).
class ModKey final : public CaptureClient {
public:
    MtLoader* L = nullptr;
    int mod = -1, setting = -1;
    std::string label;
    const char* name() const override { return label.c_str(); }
    std::string bound() const override {
        char buf[128] = {};
        return L && mt_setting_get_text(L, mod, setting, buf, sizeof buf) == MT_OK ? buf : "";
    }
    bool take(const play::InputId& in) override {
        if (!L || in.kind != play::InKind::Key) return false;   // a mod's key setting holds a keyboard key
        return mt_setting_set_text(L, mod, setting, play::input_name(in).c_str()) == MT_OK;
    }
};

class ModsTab final : public TabModel {
public:
    int count() const override { return static_cast<int>(lines_.size()); }
    Shape shape(int i) const override { return lines_[i].shape; }
    const char* label(int i) const override { return lines_[i].label.c_str(); }
    const char* desc(int i) const override { return lines_[i].desc.c_str(); }
    bool restart(int i) const override { return lines_[i].restart; }
    int def(int i) const override { return orig[i]; }   // the core keeps the defaults: a reset leaves the row as it is
    CaptureClient* capture(int i) const override {
        const Line& l = lines_[i];
        return l.kind == Kind::Setting && l.shape.use == Use::Capture ? &keys_[i - first_setting_row()] : nullptr;
    }
    const char* value_text(int i, int pos, char* out, size_t n) const override;

    void open() override;
    void commit(Apply&) override;
    void accept() override;
    void revert() override;
    void preview(int i, int pos) override;
    void pick(int i, int pos) override;
    bool hidden_changes() const override;

private:
    int first_setting_row() const { return paged() ? kFirstSetting + 1 : kFirstSetting; }
    bool paged() const { return settings_of(shown_) > kSettingRows; }
    int settings_of(int m) const { return L_ && m >= 0 ? mt_setting_count(L_, m) : 0; }
    int pages() const { return (settings_of(shown_) + kPageSize - 1) / kPageSize; }
    bool on_screen(int m, int s) const;
    bool setting_changed(int m, int s) const;
    void keep_switch();
    void build();
    Line setting_line(int s, int row);
    int position(const Line& l, double v) const;
    double value(const Line& l, int pos) const;

    MtLoader* L_ = nullptr;
    std::vector<Mod> mods_;
    int shown_ = 0, page_ = 0;
    std::vector<Line> lines_;
    mutable ModKey keys_[kSettingRows];
    std::unordered_map<std::string, bool> start_on_;   // id -> the switch this run started with (Mod::on_at_start)
};

const char* ModsTab::value_text(int i, int pos, char* out, size_t n) const {
    const Line& l = lines_[i];
    switch (l.kind) {
        case Kind::Mod: {
            const Mod& m = mods_[std::clamp(pos, 0, static_cast<int>(mods_.size()) - 1)];
            std::snprintf(out, n, "%s (%s)", m.title.c_str(), m.state.c_str());
            return out;
        }
        case Kind::Page:
            std::snprintf(out, n, "Page %d of %d", std::clamp(pos, 0, pages() - 1) + 1, pages());
            return out;
        case Kind::Setting:
            if (l.type == MT_SET_KEY) return capture::text(keys_[i - first_setting_row()], out, n);
            if (l.type == MT_SET_NUMBER) {
                std::snprintf(out, n, "%g", value(l, pos));
                return out;
            }
            break;
        default: break;
    }
    if (l.names.empty()) return "";
    return l.names[std::clamp(pos - l.shape.lo, 0, static_cast<int>(l.names.size()) - 1)].c_str();
}

// A number's position for a value, and the value of a position.
int ModsTab::position(const Line& l, double v) const {
    if (l.type != MT_SET_NUMBER) return static_cast<int>(v);
    return std::clamp(static_cast<int>(std::lround((v - l.lo) / l.step)), l.shape.lo, l.shape.hi);
}
double ModsTab::value(const Line& l, int pos) const { return l.type == MT_SET_NUMBER ? std::min(l.lo + pos * l.step, l.hi) : pos; }

Line ModsTab::setting_line(int s, int row) {
    const MtSettingInfo* in = mt_setting_info(L_, shown_, s);
    Line l;
    l.kind = Kind::Setting;
    l.setting = s;
    l.type = in->type;
    l.restart = in->restart != 0;
    l.label = in->label && *in->label ? in->label : str(in->key);
    l.desc = str(in->description);
    switch (in->type) {
        case MT_SET_BOOL:
            l.shape = Shape{2, 0, 1, Use::Live};
            l.names = {"Off", "On"};
            break;
        case MT_SET_CHOICE: {
            const int n = std::min(in->nchoices, kMaxPositions + 1);
            for (int c = 0; c < n; ++c) l.names.push_back(str(in->choices[c]));
            if (n == 1) l.shape = Shape{2, 0, 1, Use::ReadOnly};
            else if (n <= 4) l.shape = Shape{static_cast<uint8_t>(n), 0, n - 1, Use::Live};
            else l.shape = Shape{0, 0, n - 1, Use::Live};
            break;
        }
        case MT_SET_NUMBER: {
            // Positions of `step` from min (a step of 0 is any value: 255 positions); more than 255 take every k-th, the last is max.
            const double span = in->max - in->min;
            double step = in->step > 0 ? in->step : span / kMaxPositions;
            int positions = step > 0 ? static_cast<int>(std::lround(span / step)) : 0;
            if (positions > kMaxPositions) {
                const int k = (positions + kMaxPositions - 1) / kMaxPositions;
                step *= k;
                positions = (positions + k - 1) / k;
            }
            l.lo = in->min;
            l.hi = in->max;
            l.step = step > 0 ? step : 1;
            l.shape = Shape{0, 0, std::max(positions, 1), Use::Live};
            break;
        }
        case MT_SET_KEY: {
            ModKey& k = keys_[row - first_setting_row()];
            k.L = L_;
            k.mod = shown_;
            k.setting = s;
            k.label = "Mod " + mods_[shown_].id + " " + str(in->key);
            l.shape = Shape{0, 0, 2 * capture::kMid, Use::Capture};
            l.desc += (l.desc.empty() ? "" : " ") + std::string("To change it, press left or right on this row (or click it), then press the new key. Esc cancels.");
            break;
        }
        default: {   // text: edited in the launcher
            std::string t = mods_[shown_].text[static_cast<size_t>(s)];
            if (t.size() > kShortText) t = t.substr(0, kShortText - 3) + "...";
            l.names = {t, t};
            l.shape = Shape{2, 0, 1, Use::ReadOnly};
            l.desc += (l.desc.empty() ? "" : " ") + std::string("Edit it in the launcher's Mods page.");
            break;
        }
    }
    if (mods::setting_locked(mods_[shown_].id.c_str(), in->key)) {
        // shown as the core words its value (On, the choice, the number, the key), not changeable here
        char buf[128] = {};
        mt_setting_get_text(L_, shown_, s, buf, sizeof buf);
        l.names = {buf, buf};
        l.shape = Shape{2, 0, 1, Use::ReadOnly};
        l.desc += (l.desc.empty() ? "" : " ") + std::string("Locked in the launcher, which keeps it as it is. Unlock it in the launcher's Mods page to change it.");
    }
    return l;
}

// The rows of the shown mod (and page), and their values in the slots.
void ModsTab::build() {
    lines_.clear();
    for (int i = 0; i < kSlots; ++i) cur[i] = orig[i] = 0;
    if (mods_.empty()) {
        Line l;
        l.shape = Shape{2, 0, 1, Use::ReadOnly};
        l.label = "No mods";
        l.desc = mods::started_without() ? "This start runs without mods. They are back at the next start."
                                         : "No mods. Add them in the launcher's Mods page.";
        l.names = {"None", "None"};
        lines_.push_back(std::move(l));
        return;
    }
    const Mod& m = mods_[static_cast<size_t>(shown_)];
    const int n = static_cast<int>(mods_.size());

    Line pick;
    pick.kind = Kind::Mod;
    pick.shape = Shape{0, 0, std::max(n - 1, 1), Use::Rows};
    pick.label = "Mod";
    pick.desc = "The mod the rows below are about, in load order. Its switch, its state and its settings follow.";
    lines_.push_back(std::move(pick));
    cur[kModRow] = orig[kModRow] = shown_;

    Line sw;
    sw.kind = Kind::Switch;
    sw.label = "On/Off";
    sw.names = {"Off", "On"};
    if (m.locked && m.on_orig) {
        sw.shape = Shape{2, 0, 1, Use::ReadOnly};
        sw.names = {"On", "On"};
        sw.desc = "Locked in the launcher, which keeps this mod on. To switch it off or remove it, unlock it in the launcher's Mods page first: "
                  "it says what that changes.";
    } else if (m.held) {
        sw.shape = Shape{2, 0, 1, Use::ReadOnly};
        sw.names = {m.on_orig ? "On" : "Off", m.on_orig ? "On" : "Off"};
        sw.desc = "This mod has new or changed code that you have not allowed, so it does not run. Allow it in the launcher's Mods page; "
                  "this switch cannot turn it on.";
    } else {
        sw.shape = Shape{2, 0, 1, Use::Value};
        sw.desc = "Turns this mod on or off.";
        sw.restart = true;
    }
    lines_.push_back(std::move(sw));
    cur[kSwitchRow] = m.on_cur;
    orig[kSwitchRow] = m.on_orig;

    Line st;
    st.kind = Kind::Status;
    st.shape = Shape{2, 0, 1, Use::ReadOnly};
    st.label = "Status";
    st.names = {m.state, m.state};
    st.desc = status_desc(m);
    lines_.push_back(std::move(st));

    const int total = settings_of(shown_);
    int first = 0, shown_settings = total;
    if (paged()) {
        Line pg;
        pg.kind = Kind::Page;
        pg.shape = Shape{0, 0, std::max(pages() - 1, 1), Use::Rows};
        pg.label = "Settings page";
        pg.desc = "This mod has more settings than fit here: the page of them the rows below show.";
        lines_.push_back(std::move(pg));
        cur[kFirstSetting] = orig[kFirstSetting] = page_;
        first = page_ * kPageSize;
        shown_settings = std::min(kPageSize, total - first);
    }
    for (int k = 0; k < shown_settings; ++k) {
        const int row = static_cast<int>(lines_.size()), s = first + k;
        lines_.push_back(setting_line(s, row));
        const Line& l = lines_.back();
        if (l.shape.use == Use::Capture) {
            cur[row] = orig[row] = capture::kMid;
        } else if (l.shape.use == Use::Live) {
            cur[row] = position(l, mt_setting_get_number(L_, shown_, s));
            orig[row] = position(l, m.num[static_cast<size_t>(s)]);
        }
    }
}

void ModsTab::open() {
    L_ = mods::loader();
    mods_.clear();
    const int n = L_ ? std::min(mt_loader_count(L_), kMaxMods) : 0;
    for (int i = 0; i < n; ++i) {
        const MtModInfo* in = mt_loader_info(L_, i);
        if (!in) break;
        Mod m;
        m.id = str(in->id);
        m.title = str(in->name && *in->name ? in->name : in->id) + " " + str(in->version);
        m.about = in->message && *in->message ? str(in->message) : str(in->description);
        m.facts = tags(*in);
        m.held = (in->flags & MT_MOD_HAS_CODE) && (in->flags & MT_MOD_NEW_CODE);
        m.locked = mods::locked(in->id);
        m.on_cur = m.on_orig = in->enabled ? 1 : 0;
        // The core's switch changes only when this tab saves one, so its value at the first open is the one the run started with.
        m.on_at_start = start_on_.try_emplace(m.id, in->enabled != 0).first->second;
        m.state = state_word(*in, m.on_at_start, m.on_orig != 0);
        mods_.push_back(std::move(m));
    }
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = mods_[i];
        const int ns = settings_of(static_cast<int>(i));
        m.num.assign(static_cast<size_t>(ns), 0);
        m.text.assign(static_cast<size_t>(ns), "");
        for (int s = 0; s < ns; ++s) {
            m.num[static_cast<size_t>(s)] = mt_setting_get_number(L_, static_cast<int>(i), s);
            char buf[512] = {};
            if (mt_setting_get_text(L_, static_cast<int>(i), s, buf, sizeof buf) == MT_OK) m.text[static_cast<size_t>(s)] = buf;
        }
    }
    if (shown_ >= static_cast<int>(mods_.size())) shown_ = 0;   // the mod shown last time stays shown
    page_ = 0;
    build();
}

// The shown mod's switch, from its slot.
void ModsTab::keep_switch() {
    if (mods_.empty() || lines_.size() <= kSwitchRow || lines_[kSwitchRow].shape.use != Use::Value) return;
    mods_[static_cast<size_t>(shown_)].on_cur = cur[kSwitchRow];
}

void ModsTab::commit(Apply&) {
    keep_switch();
    bool any = false;
    for (const Mod& m : mods_)
        if (m.on_cur != m.on_orig) {
            mt_loader_set_enabled(L_, m.id.c_str(), m.on_cur);
            any = true;
        }
    if (any) mt_loader_save_list(L_);
}

// After the commit: what is in force now is what a discard goes back to.
void ModsTab::accept() {
    TabModel::accept();
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = mods_[i];
        m.on_orig = m.on_cur;
        if (const MtModInfo* in = mt_loader_info(L_, static_cast<int>(i))) m.state = state_word(*in, m.on_at_start, m.on_orig != 0);
        for (size_t s = 0; s < m.num.size(); ++s) {
            m.num[s] = mt_setting_get_number(L_, static_cast<int>(i), static_cast<int>(s));
            char buf[512] = {};
            if (mt_setting_get_text(L_, static_cast<int>(i), static_cast<int>(s), buf, sizeof buf) == MT_OK) m.text[s] = buf;
        }
    }
    for (Line& l : lines_)
        if (l.kind == Kind::Status) l.names = {mods_[static_cast<size_t>(shown_)].state, mods_[static_cast<size_t>(shown_)].state};
}

// Every switch as saved, every setting the screen changed back to its value (the mod is told again).
void ModsTab::revert() {
    for (size_t i = 0; i < mods_.size(); ++i) {
        Mod& m = mods_[i];
        m.on_cur = m.on_orig;
        for (size_t s = 0; s < m.num.size(); ++s) {
            if (!setting_changed(static_cast<int>(i), static_cast<int>(s))) continue;
            const int type = mt_setting_info(L_, static_cast<int>(i), static_cast<int>(s))->type;
            if (type == MT_SET_TEXT || type == MT_SET_KEY) mt_setting_set_text(L_, static_cast<int>(i), static_cast<int>(s), m.text[s].c_str());
            else mt_setting_set_number(L_, static_cast<int>(i), static_cast<int>(s), m.num[s]);
        }
    }
    if (!mods_.empty()) build();
    else TabModel::revert();
}

// A live setting row moved: the mod gets the value now.
void ModsTab::preview(int i, int pos) {
    const Line& l = lines_[i];
    if (l.kind == Kind::Setting) mt_setting_set_number(L_, shown_, l.setting, value(l, pos));
}

// The Mod or the Page row moved.
void ModsTab::pick(int i, int pos) {
    if (mods_.empty()) return;
    keep_switch();
    if (lines_[i].kind == Kind::Mod) {
        shown_ = std::clamp(pos, 0, static_cast<int>(mods_.size()) - 1);
        page_ = 0;
    } else if (lines_[i].kind == Kind::Page) {
        page_ = std::clamp(pos, 0, pages() - 1);
    }
    build();
}

bool ModsTab::on_screen(int m, int s) const {
    if (m != shown_) return false;
    const int first = paged() ? page_ * kPageSize : 0;
    return s >= first && s < first + (paged() ? kPageSize : kSettingRows);
}

bool ModsTab::setting_changed(int m, int s) const {
    const Mod& mod = mods_[static_cast<size_t>(m)];
    const int type = mt_setting_info(L_, m, s)->type;
    if (type == MT_SET_TEXT || type == MT_SET_KEY) {
        char buf[512] = {};
        mt_setting_get_text(L_, m, s, buf, sizeof buf);
        return mod.text[static_cast<size_t>(s)] != buf;
    }
    return mt_setting_get_number(L_, m, s) != mod.num[static_cast<size_t>(s)];
}

// Switches of the other mods and settings not on the rows now (a key set by a capture counts as hidden: its row's value means nothing).
bool ModsTab::hidden_changes() const {
    for (size_t i = 0; i < mods_.size(); ++i) {
        const int m = static_cast<int>(i);
        if (m != shown_ && mods_[i].on_cur != mods_[i].on_orig) return true;
        for (size_t s = 0; s < mods_[i].num.size(); ++s) {
            const int type = mt_setting_info(L_, m, static_cast<int>(s))->type;
            if ((type == MT_SET_KEY || !on_screen(m, static_cast<int>(s))) && setting_changed(m, static_cast<int>(s))) return true;
        }
    }
    return false;
}

ModsTab g_mods;

}  // namespace

TabModel& mods_tab() { return g_mods; }

}  // namespace nxrt::games::pkp::options_detail
