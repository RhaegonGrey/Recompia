// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options: the PC settings of the F1 page, as tabs and rows of the game's own Settings screen. [pkp] options_menu, on by default.
// Nothing reaches the save file: the rows read and write the Settings behind controls.ini, the same store as the F1 page, or a [pkp] key
// of the game ini (Row::key, settings::set_*).
//
// How the game's Options screen works (main 2.0.1a, static reading):
//  * Rows come from a data table of 51 records of 24 bytes (ids 0x25..0x32 are empty). A record names its tab, type, value count and text.
//  * The screen's value vector (ptr +0xa8, count +0xb0, 12-byte {id, cur, saved}) has one element per id and is filled when the screen opens.
//  * 0x7100e81b3c (player, id, mode, item) reads (mode 1) or applies (2, 3) the value at item + 0x358; unknown ids do nothing.
//  * 0x710100fce0 (widget, value count - 1, id) names the values from a table that ends at id 0x24: a new choice row crashes without a hook.
//  * Text ids above 800 are skipped by the game's guards; text 0xa8 is a bare "show this string" entry in every language.
//
// 14 free ids for about 60 rows, so the ids are slots: only one tab shows at a time, and a tab change rewrites the slot records and swaps
// the slot values in the vector. Each tab is a model (pkp_options_model.h) that holds its values (cur / orig) and its rows; changes reach
// Settings and the game ini only when the screen commits (0x10092a0), and a discard drops them.
// Long lists and decimals are sliders whose numeric text is replaced by the row's own (hook after 0x710100f0a0's text call).
// Hooks are the sites listed in recomp/games/pkp/main.sites.
// The files it is split into: pkp_options_internal.h.
#include "ui/pkp_options.h"
#include "ui/pkp_options_model.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::pkp {
namespace options_detail {

namespace {

constexpr uint64_t kOpen = 0x1006628;         // Options screen, open (vtable +0x38)
constexpr uint64_t kOption = 0xe81b3c;        // option get/set
constexpr uint64_t kValueNames = 0x100fce0;   // choice widget: value names
constexpr uint64_t kRowDrawn = 0x100b294;     // inline, after the row's draw called its value widget (x19 = the row)
constexpr uint64_t kSliderText = 0x100f1b4;   // inline, in the slider's update after its number was drawn (x19 = the row, w20 = the value)
constexpr uint64_t kTextById = 0xeba100;      // (widget, slot, text id): sets the description
constexpr uint64_t kStore = 0x100b920;        // (screen, id, value, flag): a row's value changed
constexpr uint64_t kCommit = 0x10092a0;       // (screen, commit): commit or revert the value vector
constexpr uint64_t kTabPrev = 0x100de9c, kTabNext = 0x100e084;   // the screen's tab change
constexpr uint64_t kRebuildList = 0x10070e0;  // (screen) row list for the current tab
constexpr uint64_t kRedraw = 0x1007380;       // (screen, 1, 1, 0) as the game's tab change calls it
constexpr uint64_t kCursor = 0x1008728;       // (screen, 1) as the game's tab change calls it
constexpr uint64_t kListCap = 0x1012a60;      // inline at the bl that sizes the tab list (w1 = capacity)
constexpr uint64_t kBarList = 0x10132a4;      // inline in the tab bar build: the three tabs are in the list (x0/x22 = list, w24 = mode)
constexpr uint64_t kBarState = 0x10132d0;     // inline at the bl that picks the bar's layout for its tab count (x21 = bar, x9 = the table, w1 = the entry)
constexpr uint64_t kBarIcons = 0x101334c;     // inline before the bar's icons are set (x21 = bar)
constexpr uint64_t kClockText = 0xead978;     // (hour, minute, out): the time of day as text, in the layout of the game language (out holds 0x1000 bytes)
constexpr uint64_t kBarTitle = 0xeb1b40;      // (bar, title byte): the title under the bar
constexpr uint64_t kBarTitleBuild = 0x10134e8, kBarTitleChange = 0x1013f54, kBarTitleChangeAlt = 0x1014004;   // its return addresses in the Options bar (x19 = the bar's holder)
using layout::kChildOf;
constexpr uint64_t kSetSprite = 0xa7c05c;     // (pane holder, slot, sprite, 0, 0)
using layout::kTables;
constexpr uint64_t kOptionTable = 0x10 + 0x828, kRowId = 0x420, kItemValue = 0x358;
constexpr uint64_t kVecPtr = 0xa8, kVecCount = 0xb0, kMode = 0x358, kCategory = 0x360, kGroup = 0x361, kTabIndex = 0x364, kDirty = 0x383;
constexpr uint32_t kCustomText = 801;         // past the game's guards (1..800): text id 801 + slot is that slot's description
constexpr uint32_t kFirstValueSlot = 7;       // text slots of the value names
constexpr uint32_t kSliderSlot = 0x14, kLabelSlot = 4, kTitleSlot = 0x19;
using layout::kLayoutOf;
constexpr uint64_t kPaneIndex = 0xa72644;     // the slot to pane lookup (with layout::kLayoutOf)
constexpr uint32_t kPaneWidth = 0xf0;         // int16, text pane
constexpr int16_t kSliderTextWidth = 420;     // room for the longest value name above the slider
constexpr uint8_t kFirstId = 0x25;
constexpr uint8_t kNoGroup = 0xee;            // a slot not used by the current tab
constexpr uint8_t kFirstOrder = 40;           // after the game's own rows of the tab
constexpr uint8_t kCellClass = 0x49;
constexpr uint32_t kFirstCell = 0x62b;        // the bar's cell widgets, 16 of them
// ponytail: every new tab shows the Game tab's icon until there is art; add a sprite pair per tab when it exists.
constexpr uint32_t kIconOn = 0x88, kIconOff = 0x87;

// ---- tabs ------------------------------------------------------------------------------------------------------------------------

struct Tab {
    const char* title;      // nullptr: the game's own title
    uint8_t group;          // record +0x12
    TabModel* model;        // nullptr: none of our rows
};

StaticTab g_sound{kSoundRows, &refresh_audio_devices}, g_display{kDisplayRows}, g_performance{kPerformanceRows}, g_graphics{kGraphicsRows},
    g_picture{kPictureRows}, g_mouse{kMouseRows}, g_gameplay{kGameplayRows}, g_online{kOnlineRows};

const Tab kTabs[] = {
    {nullptr, 1, nullptr},   // Game
    {nullptr, 2, nullptr},   // Camera
    {nullptr, 3, &g_sound},
    {"Online", 16, &g_online},
    {"Display", 4, &g_display},
    {"Performance", 5, &g_performance},
    {"Graphics", 6, &g_graphics},
    {"Picture", 7, &g_picture},
    {"Mouse", 8, &g_mouse},
    {"Controller", 9, &controller_tab()},
    {"Gameplay", 10, &g_gameplay},
    {"Time", 11, &time_tab()},
    {"Events", 12, &events_tab()},
    {"Mods", 14, &mods_tab()},   // group 13 is the game's own (Notes), so 14
    {"Items", 15, &items_tab()},
};
constexpr int kTabCount = static_cast<int>(std::size(kTabs));
constexpr int kGameTabs = 3;
// The bar has 16 cells (kFirstCell) and its layout table (hook_bar_state) has 16 entries: one more tab needs nothing else.
static_assert(kTabCount <= 16, "the tab bar has 16 cells");

// ---- state -------------------------------------------------------------------------------------------------------------------------

struct State {
    uint64_t base = 0;
    uint64_t child = 0;
    bool active = false, bar_extended = false;
    int tab = 0;
    uint8_t* rec[kSlots] = {};
} g;

using guest::rd;
using guest::wr;
using guest::call;

// The string is copied below the guest stack pointer: the game reads it as guest memory.
void set_text(::Ctx* c, uint64_t widget, uint32_t slot, const char* s) { text::set_string(c, g.base, widget, slot, s); }

int row_slot(uint32_t id) { return id >= kFirstId && id < kFirstId + kDirtySlot ? static_cast<int>(id - kFirstId) : -1; }
int row_count(int tab) { return kTabs[tab].model ? kTabs[tab].model->count() : 0; }
// The shown tab's model when the id is one of its rows, else nullptr; *row is the row.
TabModel* shown(uint32_t id, int* row) {
    *row = row_slot(id);
    return g.active && *row >= 0 && *row < row_count(g.tab) ? kTabs[g.tab].model : nullptr;
}

constexpr int kNoValues[kSlots] = {};
const int* cur_of(int tab) { return kTabs[tab].model ? kTabs[tab].model->cur : kNoValues; }
const int* orig_of(int tab) { return kTabs[tab].model ? kTabs[tab].model->orig : kNoValues; }

// Changes the vector does not hold: every other tab's, and the shown tab's rows that are not on screen.
bool has_changes(int shown_tab) {
    for (int t = 0; t < kTabCount; ++t)
        if (const TabModel* m = kTabs[t].model)
            if (t == shown_tab ? m->hidden_changes() : m->changed()) return true;
    return false;
}

void write_records(int tab) {
    const TabModel* m = kTabs[tab].model;
    for (int slot = 0; slot < kSlots; ++slot) {
        uint8_t* r = g.rec[slot];
        if (!r) continue;
        const bool row = slot != kDirtySlot && slot < row_count(tab);
        const Shape sh = row ? m->shape(slot) : Shape{};
        const uint16_t text = static_cast<uint16_t>(kCustomText + slot);
        std::memcpy(r, &text, 2);
        std::memcpy(r + 2, &text, 2);
        std::memset(r + 8, 1, 10);                   // shown in every screen mode
        r[0x13] = 2;                                 // general options: a slot stays in the value vector whatever the tab
        r[0x16] = 3;                                 // as every other row
        r[0x15] = static_cast<uint8_t>(kFirstOrder + slot);   // a nonzero order is what makes the row exist
        r[0x12] = row ? kTabs[tab].group : kNoGroup;
        r[4] = row && !sh.choices ? 2 : 1;
        r[6] = 1;                                    // the dirty scan skips rows with 0 here
        r[5] = r[7] = 0;
        if (!row) continue;
        r[5] = static_cast<uint8_t>(std::clamp(m->def(slot), sh.lo, sh.hi));
        r[6] = static_cast<uint8_t>(sh.hi);          // a choice: its value count - 1
        r[7] = static_cast<uint8_t>(sh.lo);
    }
}

template <class F> void each_slot_element(F&& f) {
    const uint64_t ptr = rd<uint64_t>(g.child + kVecPtr), n = rd<uint64_t>(g.child + kVecCount);
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t e = ptr + i * 12;
        const uint8_t id = rd<uint8_t>(e);
        if (id >= kFirstId && id < kFirstId + kSlots) f(id - kFirstId, e);
    }
}

void vector_to_store() {
    TabModel* m = kTabs[g.tab].model;
    if (!m) return;
    each_slot_element([m](int slot, uint64_t e) { if (slot != kDirtySlot) m->cur[slot] = rd<int>(e + 4); });
}

void store_to_vector() {
    const bool other_tabs = has_changes(g.tab);
    const int *cur = cur_of(g.tab), *orig = orig_of(g.tab);
    each_slot_element([other_tabs, cur, orig](int slot, uint64_t e) {
        wr<int>(e + 4, slot == kDirtySlot ? 0 : cur[slot]);
        wr<int>(e + 8, slot == kDirtySlot ? (other_tabs ? 1 : 0) : orig[slot]);
    });
    if (other_tabs) wr<uint8_t>(g.child + kDirty, 1);
}

// ---- hooks -------------------------------------------------------------------------------------------------------------------------

bool find_records(::Ctx* c) {
    const uint64_t mgr = rd<uint64_t>(g.base + kTables);
    uint64_t t = mgr ? rd<uint64_t>(mgr + kOptionTable) : 0;
    if (t) t = call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x38), t);
    if (!t) return false;
    for (int s = 0; s < kSlots; ++s) {
        const uint64_t entry = call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x30), t, kFirstId + s);
        const uint64_t rec = entry ? rd<uint64_t>(entry + 8) : 0;
        if (!rec || !mem::is_mapped(rec, 24)) return false;
        g.rec[s] = reinterpret_cast<uint8_t*>(rec);
    }
    return true;
}

int hook_open(::Ctx* c, uint64_t, void*) {
    const uint64_t child = c->x[0];
    if (!g.rec[0] && !find_records(c)) {
        static bool logged = false;
        if (!logged) NXLOG_W(kTag, "options: the option records were not found, the PC rows stay hidden");
        logged = true;
        return 0;
    }
    g.active = rd<uint8_t>(child + kCategory) == 2 && rd<int>(child + kMode) != 9;
    if (!g.active) {                                 // the second player's screen and the online screens keep the game's rows only
        for (uint8_t* r : g.rec) r[0x12] = kNoGroup;
        return 0;
    }
    const bool returning = rd<uint8_t>(child + 0x386) != 0;   // back from Button Assignments: the changes so far stay
    g.child = child;
    g.tab = std::clamp(rd<int>(child + kTabIndex), 0, kTabCount - 1);
    if (g.tab >= kGameTabs) wr<uint8_t>(child + kGroup, kTabs[g.tab].group);   // the game's table only knows its own three tabs
    if (!returning)
        for (const Tab& t : kTabs)
            if (t.model) t.model->open();
    write_records(g.tab);
    return 0;
}

int hook_option(::Ctx* c, uint64_t, void*) {
    const uint32_t id = c->x[1] & 0xff;
    if (!g.active || id < kFirstId || id >= kFirstId + kSlots) return 0;
    if (static_cast<uint32_t>(c->x[2]) == 1) wr<int>(c->x[3] + kItemValue, id == kFirstId + kDirtySlot ? 0 : cur_of(g.tab)[id - kFirstId]);
    return 1;                                        // mode 2 and 3: hook_commit applies every tab at once
}

int hook_commit(::Ctx* c, uint64_t, void*) {
    if (!g.active || c->x[0] != g.child) return 0;
    vector_to_store();
    if (c->x[1] & 1) {
        Apply a{play::current_settings()};
        for (const Tab& t : kTabs)
            if (t.model) t.model->commit(a);
        if (a.changed) play::apply_settings(a.s);   // main thread, next frame; also writes controls.ini
        for (const Tab& t : kTabs)
            if (t.model) t.model->accept();
    } else if (rd<int>(g.child + 0x354) != 0) {      // "discard" (the game's other call with 0 only redraws)
        for (const Tab& t : kTabs)
            if (t.model) t.model->revert();
    }
    store_to_vector();
    return 0;                                        // the game commits its own rows
}

// Rewrites the slot records for the shown tab's rows and swaps its values into the vector; the game's row list is then built again.
void show_tab(::Ctx* c) {
    write_records(g.tab);
    store_to_vector();
    call(c, g.base + kRebuildList, g.child);
    call(c, g.base + kRedraw, g.child, 1, 1, 0);
}

int hook_store(::Ctx* c, uint64_t, void*) {
    int row = -1;
    TabModel* m = c->x[0] == g.child ? shown(c->x[1] & 0xff, &row) : nullptr;
    if (!m) return 0;
    const int pos = static_cast<int>(c->x[2]);
    switch (m->shape(row).use) {
        case Use::Value: return 0;
        case Use::Live:
            m->cur[row] = pos;
            m->preview(row, pos);
            return 0;
        case Use::Capture:                           // wait for the new key (pkp_options_capture.h)
            capture::begin(*m->capture(row));
            return 0;
        case Use::Press:                             // the model set its values: the vector shows them, nothing to save
            m->press(row, pos);
            each_slot_element([m](int s, uint64_t e) {
                wr<int>(e + 4, s == kDirtySlot ? 0 : m->cur[s]);
                wr<int>(e + 8, s == kDirtySlot ? 0 : m->orig[s]);
            });
            return 1;
        case Use::ReadOnly: return 1;
        case Use::Rows:                              // the store's callers move the cursor next (0x1008728), as a tab change does last
            vector_to_store();
            m->pick(row, pos);
            show_tab(c);
            return 1;
    }
    return 0;
}

int change_tab(::Ctx* c, int dir) {
    const uint64_t child = c->x[0];
    if (!g.active || child != g.child) return 0;
    if (rd<uint32_t>(child + 0x90) - 1u > 1 || rd<int>(child + 0x350) != 0 || rd<uint8_t>(child + 0x385) != 0) {
        c->x[0] = 0;
        return 1;
    }
    vector_to_store();
    g.tab = (g.tab + dir + kTabCount) % kTabCount;
    wr<int>(child + kTabIndex, g.tab);
    wr<uint8_t>(child + kGroup, kTabs[g.tab].group);
    show_tab(c);
    call(c, g.base + kCursor, child, 1);
    c->x[0] = 1;
    return 1;
}
int hook_tab_prev(::Ctx* c, uint64_t, void*) { return change_tab(c, -1); }
int hook_tab_next(::Ctx* c, uint64_t, void*) { return change_tab(c, 1); }

void hook_list_cap(::Ctx* c, uint64_t, void*) { c->x[1] = kTabCount; }

void hook_bar_list(::Ctx* c, uint64_t, void*) {
    g.bar_extended = static_cast<uint32_t>(c->x[24]) != 9;
    wr<int>(c->x[0] + 0x78, g.bar_extended ? kTabCount : kGameTabs);   // the list's columns: L and R only move within a row of the grid
    if (!g.bar_extended) return;
    const uint64_t list = rd<uint64_t>(c->x[0] + 8);
    if (!list) return;
    for (uint64_t i = kGameTabs; i < kTabCount; ++i) {   // the list's {ptr, size, capacity} of {u64 (index << 32 | 0x2f), u32 0}
        const uint64_t size = rd<uint64_t>(list + 8);
        if (size == rd<uint64_t>(list + 0x10)) break;
        const uint64_t e = rd<uint64_t>(list) + size * 12;
        wr<uint64_t>(e, i << 32 | 0x2f);
        wr<uint32_t>(e + 8, 0);
        wr<uint64_t>(list + 8, size + 1);
    }
}

void hook_bar_state(::Ctx* c, uint64_t, void*) {
    if (!g.bar_extended) return;
    wr<uint8_t>(c->x[21] + 0xa4, kTabCount);
    c->x[1] = rd<uint32_t>(c->x[9] + (kTabCount - 1) * 4);   // the game masks this index to its three tabs; its 16 entries are 17 - the tab count
}

void hook_bar_icons(::Ctx* c, uint64_t, void*) {
    if (!g.bar_extended) return;
    const uint64_t bar = c->x[21];
    const uint64_t cls = (c->x[31] - 0x20) & ~uint64_t{0xf};
    for (uint32_t i = kGameTabs; i < kTabCount; ++i) {
        wr<uint16_t>(cls, 0);
        const uint64_t cell = call(c, g.base + kChildOf, bar + 0x48, kFirstCell + i, cls, 0, 0, cls - 0x10);
        if (!cell || rd<uint16_t>(cls) != kCellClass) continue;
        call(c, g.base + kSetSprite, cell + 0x30, 5, kIconOn, 0, 0, cls - 0x10);
        call(c, g.base + kSetSprite, cell + 0x30, 6, kIconOff, 0, 0, cls - 0x10);
    }
}

// The title function is shared by every screen with a tab bar, and the Pokédex reuses the bar's address after Options closed (#494):
// only the Options bar's own calls are ours. Each of them has just stored the tab at x19 + 0xac.
int hook_bar_title(::Ctx* c, uint64_t, void*) {
    const uint64_t ret = c->x[30] - g.base;
    if (!g.bar_extended || (ret != kBarTitleBuild && ret != kBarTitleChange && ret != kBarTitleChangeAlt)) return 0;
    const int tab = rd<int>(c->x[19] + 0xac);
    if (tab < kGameTabs || tab >= kTabCount) return 0;
    set_text(c, c->x[0], kTitleSlot, kTabs[tab].title);
    return 1;
}

int hook_value_names(::Ctx* c, uint64_t, void*) {
    int row = -1;
    const TabModel* m = shown(c->x[2] & 0xff, &row);
    if (!m) return 0;
    char buf[128];
    for (int i = 0; i < m->shape(row).choices; ++i) set_text(c, c->x[0], kFirstValueSlot + i, m->value_text(row, i, buf, sizeof buf));
    return 1;
}

uint64_t text_pane(::Ctx* c, uint64_t widget, uint32_t slot);

void hook_row_drawn(::Ctx* c, uint64_t, void*) {
    int row = -1;
    const TabModel* m = shown(rd<uint8_t>(c->x[19] + kRowId), &row);
    if (!m) return;
    set_text(c, c->x[19], kLabelSlot, m->label(row));
    if (m->shape(row).use == Use::Capture && capture::take_redraw(*m->capture(row))) {   // the capture started or ended: the key text, not the slider's number
        if (const uint64_t pane = text_pane(c, c->x[19], kSliderSlot)) {
            if (rd<int16_t>(pane + kPaneWidth) < kSliderTextWidth) wr<int16_t>(pane + kPaneWidth, kSliderTextWidth);
        }
        char buf[128];
        set_text(c, c->x[19], kSliderSlot, m->value_text(row, capture::kMid, buf, sizeof buf));
    }
}

// The widget's text pane for a slot: the game's own walk (see 0xa7abe0). 0 if there is none.
uint64_t text_pane(::Ctx* c, uint64_t widget, uint32_t slot) {
    const uint64_t handler = rd<uint64_t>(widget + 0x68);
    const uint64_t source = handler ? rd<uint64_t>(handler + 8) : 0;
    const uint64_t layout = source ? call(c, g.base + kLayoutOf, source) : 0;
    if (!layout || rd<uint16_t>(layout + 0x92) <= slot) return 0;
    const uint32_t index = static_cast<uint32_t>(call(c, g.base + kPaneIndex, handler, slot));
    const uint32_t count = static_cast<uint32_t>(call(c, rd<uint64_t>(rd<uint64_t>(layout + 0x10) + 0x20), layout + 0x10));
    uint64_t node = index < count ? rd<uint64_t>(layout + 0x20) : 0;
    for (uint32_t i = 0; node && i < index; ++i) node = rd<uint64_t>(node + 8);
    const uint64_t pane = rd<uint64_t>(node ? node + 0x10 : layout + 0x28);
    return pane && rd<uint8_t>(pane + 0x112) == 2 ? pane : 0;
}

void hook_slider_text(::Ctx* c, uint64_t, void*) {
    int row = -1;
    const TabModel* m = shown(rd<uint8_t>(c->x[19] + kRowId), &row);
    if (!m || m->shape(row).choices) return;
    // The box is sized for a short number: "Match Display" came out at half size.
    if (const uint64_t pane = text_pane(c, c->x[19], kSliderSlot)) {
        if (rd<int16_t>(pane + kPaneWidth) < kSliderTextWidth) wr<int16_t>(pane + kPaneWidth, kSliderTextWidth);
    }
    char buf[128];
    set_text(c, c->x[19], kSliderSlot, m->value_text(row, static_cast<int>(c->x[20]), buf, sizeof buf));
}

int hook_text_by_id(::Ctx* c, uint64_t, void*) {
    const uint32_t id = c->x[2] & 0xffff;
    if (id < kCustomText || id >= kCustomText + kDirtySlot) return 0;
    const int row = static_cast<int>(id - kCustomText);
    const TabModel* m = g.active && row < row_count(g.tab) ? kTabs[g.tab].model : nullptr;
    if (!m) return 1;
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s%s", m->desc(row), m->restart(row) ? " Applies after a restart." : "");
    set_text(c, c->x[0], static_cast<uint32_t>(c->x[1]), buf);
    return 1;
}

// frame boundary, main thread: the key capture of capture rows
void frame(bool, double, play::GameFrame*) { capture::frame(g.active); }

void install(const FeatureInstall& in) {
    in.hooks.add_entry(in.base + kClockText, &hook_clock_text);   // clock_format works without the Settings tabs too
    if (!settings::get_bool(settings::Key::OptionsMenu)) return;
    const uint64_t base = in.base;
    g.base = base;
    capture::set_host(in.host);
    in.hooks.add_entry(base + kOpen, &hook_open);
    in.hooks.add_entry(base + kOption, &hook_option);
    in.hooks.add_entry(base + kValueNames, &hook_value_names);
    in.hooks.add_inline(base + kRowDrawn, &hook_row_drawn);
    in.hooks.add_inline(base + kSliderText, &hook_slider_text);
    in.hooks.add_entry(base + kTextById, &hook_text_by_id);
    in.hooks.add_entry(base + kCommit, &hook_commit);
    in.hooks.add_entry(base + kStore, &hook_store);
    in.hooks.add_entry(base + kTabPrev, &hook_tab_prev);
    in.hooks.add_entry(base + kTabNext, &hook_tab_next);
    in.hooks.add_inline(base + kListCap, &hook_list_cap);
    in.hooks.add_inline(base + kBarList, &hook_bar_list);
    in.hooks.add_inline(base + kBarState, &hook_bar_state);
    in.hooks.add_inline(base + kBarIcons, &hook_bar_icons);
    in.hooks.add_entry(base + kBarTitle, &hook_bar_title);
    NXLOG_I(kTag, "options: PC settings in the game's menu are on (%d tabs)", kTabCount);
}

}  // namespace
}  // namespace options_detail

const Feature kOptionsFeature = {.name = "options", .install = &options_detail::install, .frame = &options_detail::frame};

}  // namespace nxrt::games::pkp
