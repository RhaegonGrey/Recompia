// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (#323): in the bag screen (X) and the container screens a "(Y) Sort" button sits in the screen's own key guide; Y
// (keyboard F through the default btn_y binding; R (Space) where the screen uses Y itself) or a left click on the button sorts the items of the
// panel under the cursor: part-full stacks of the same item are merged up to the game's max stack, then the stacks are ordered by the
// item table's kind byte, then by item id; empty slots go last. [pkp] inventory_sort (on by default: the button only acts when
// pressed), [pkp] inventory_autosort (off by default: sorts the bag once each time the screen opens). Both do nothing during Link Play.
//
// How the game keeps the bag (main 2.0.1a, measured with page traps and snapshot diffs in scripted runs):
//  * A slot is 16 bytes: a vtable qword (main+0x322c1d8), a packed u32 (bits 0..12 item id, 0 = empty; 13..20, 21..25, 26..30 and
//    bit 31 per-slot data) and a u8 count at +0xc. Only the 8 bytes from +8 move; every slot keeps its vtable.
//  * Inventory kind 0 is the player's bag: 120 slots, then kind 4 (10 slots at bag + 0x780, the separate sub-area) that is never
//    touched. Offline the bag is save common + 0x830 (m_useItemInventoryOffline; save common = [main+0x3561930] + 0x13d4920). The
//    other kinds are containers: count slots from slot `first` of a store (save common + 0x1050, or the stage's store when the
//    container type's table entry main+0x3484c48 + type * 12 is nonzero); 0x7100e6dc2c (player, kind, type, id, use_temp) returns
//    the bag or the store, 0x71002ee4bc (type, id, &first, &count) the container's first slot and slot count.
//  * The screen works on a temp copy only while an item is held (per-player flag main+0x36bbfd8; copies at 0x3684340 / 0x36853b0)
//    and copies it back on placement / close (0x7100ff3be8 -> 0x7100e707c0). Sorting is done only while nothing is held (screen
//    state 1, flag 0), so it writes the live array and no copy is pending.
//  * Merging follows the game's own rule for dropping a stack onto a slot (0x7100ff41cc): the packed words are equal and the count stays
//    within 0x71002a7f0c() (the max stack, one value for every item: data table [0x35618f0] +0x10 +0x660, row 0, u16 at +0). Here all
//    8 bytes but the count must be equal.
//  * The bag / container screen is UI class 0x8d (vtable main+0x333ba60, the object holds +0x10; size 0x1a8). Its per-frame virtual
//    0x7100feee78 (x0 = the screen) is the entry hook. Fields: +0x30 player helper (0x7100a6fd58 -> player index), +0x78 the screen's
//    action list ({_, entries, count, cap}, 0x30 bytes each, u16 action at +0x18), +0x148 container type (u8), +0x154 container id
//    (u16), +0x160 state (1 browsing, 3 holding), +0x168 the screen's container kind (0 = bag only), +0x169 the kind of the panel under
//    the cursor. 0x7100ff12e8 (screen) makes the container and bag panels re-read their arrays (the game calls it after its own
//    commit), so the sorted order shows at once.
//  * In the bag's browsing state Y has no action (X = pick up / hold to rearrange, A = take out, B = back, - = info, + = drop), so it is
//    free for the sort.
//  * The button: the screen's prompts are the game's key guide widget fed a key guide set (pkp_key_guide.h; this is a client); the browsing
//    sets are 10 (the bag: {214 Take Out, 2 Back, 46 Swap, 126 Hold to Sort, 69 Drop, 63 Info}), 16 (the bag on a placeable item: 213
//    Spit Out) and the container screens' (11, 12, 174, 175, ...: 215 / 216 Add / Remove on A, 154 Add Stack / 155 Remove All on Y).
//    The widget lays out at most six entries (six part slots at +0xa4), so before its layout pass places them the "Hold to Sort" entry (126, X held, the second X prompt next to "Swap"; in every browsing set, in
//    no holding set) becomes key guide 92 ("Sort", the game's own text in every language). When its part is filled the icon is set to the
//    sort button and the part's layout is handed to the menu pointer (the key guide's click routing, pkp_menu.cpp): a left click on it
//    sorts. The sort button is the first of Y, R that no other prompt of the shown set uses (never L: it pages the screen's tabs
//    without being in the set or the action list, so a Sort on L also turned the page; with Y and R taken the prompt is click only)
//    (input manager table, below): Y in the bag, R in the container screens (set 186: Add (A), Swap (X), Add Stack (Y), Store All (+),
//    Info (-); registered there also B, ZL Transfer Half). Its button sorts only while no registered action of the screen is on it.
//  * m_currentUseItemSlot (save common + 0xc070, u16) holds a bag slot index: after a bag sort it is moved to the new slot of the
//    same item.
//  * Link Play: online the bag is m_useItemInventoryOnline and container moves go through the game's network requests
//    (0x7100e6e4fc), so nothing is sorted and no button is shown while 0x71000376c4 (online) is true.
#include "ui/pkp_sort.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_tables.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kScreenFrame = 0xfeee78;      // bag / container screen virtual +0x48 (per frame), x0 = the screen
constexpr uint64_t kScreenVtable = 0x333ba70;     // the vtable pointer the object holds (vtable 0x333ba60 + 0x10)
constexpr uint64_t kRefresh = 0xff12e8;          // (screen): panels re-read their arrays
constexpr uint64_t kPlayerOf = 0xa6fd58;         // (screen + 0x30) -> player index
constexpr uint64_t kInventory = 0xe6dc2c;        // (player, kind, type, id, use_temp) -> slot array
constexpr uint64_t kContainerSlots = 0x2ee4bc;   // (type, id, u32* first, u32* count) -> valid
using layout::kOnline;
using layout::kMaxStack;
constexpr uint64_t kTempFlag = layout::kItemTempFlag;
using layout::kSaveRoot;
constexpr uint64_t kCommon = layout::kSaveCommon, kUseSlot = 0xc070;
using layout::kBagOffline;
using layout::kItemTable;                        // the item table (the table at +0 while the manager holds < 200 tables, pkp_tables.h)
constexpr uint64_t kItemKind = 0x3a;
constexpr uint64_t kPlayerHelper = 0x30, kActions = 0x78, kType = 0x148, kId = 0x154, kState = 0x160, kScreenKind = 0x168, kCursorKind = 0x169;
using layout::kBagSlots;
constexpr uint32_t kMaxSlots = 0x800, kStoreSlots = 0x77a9;   // the stage store is the larger one
using items::kIdMask;
// The game's input manager [main+0x3561718]: its vector at +0xcd8 maps an action to its button (u32 per action, low byte: 0..3 the d-pad,
// 4 Y, 5 A, 6 X, 7 B, 8 (-), 9 (+), 0xa L, 0xb R, 0xc ZL, 0xd ZR, 0xe / 0xf the stick clicks; 0x7101126e18 reads it for the key guide icons).
using layout::kInputMgr;
using layout::kActionButtons;
// The sort button: the first of these whose button no prompt of the shown key guide set and no registered action of the screen uses.
// Y in the bag; R in the container screens (Y is Add Stack / Remove All, (+) Store All / Retrieve All, ZL Transfer Half there). Never L: the screen pages
// its tabs with L outside the shown set and the action list, so L would sort and page at once.
struct SortButton { uint16_t act; uint8_t button; hid::NpadButtonBit pad; const char* name; };   // an action shown as that button (the icon), the pad bit
constexpr SortButton kSortButtons[] = {{0x97, 4, hid::kBtnY, "Y"}, {0x03, 0xb, hid::kBtnR, "R"}};
constexpr int kNoButton = -1;   // click only
constexpr auto kReopenGap = std::chrono::milliseconds(300);   // no screen frame for this long = the screen was closed
// the key guide button
constexpr uint16_t kSortGuide = 92;              // key guide "Sort" (action 0xbc)
constexpr uint16_t kHoldToSortGuide = 126;       // key guide "Hold to Sort" (X held)

using guest::rd;
using guest::wr;
using guest::call;

struct State {
    uint64_t base = 0;
    bool button = true, autosort = false, log_lists = false;
    uint64_t screen = 0, prev_buttons = 0;
    std::chrono::steady_clock::time_point last_frame{};
    bool auto_pending = false;
    std::array<uint8_t, kIdMask + 1> kind_of{};   // item kind + 1, 0 = not looked up yet
    uint64_t sorts = 0;
    // the key guide button
    int client = -1;                              // the key guide client
    std::atomic<uint64_t> widget{0};              // the key guide widget showing a browsing set
    uint64_t row = 0;                             // key guide 92's table row
    std::atomic<bool> online{false};
    std::atomic<int> shown{0};                    // kSortButtons index shown on the Sort prompt (chosen at its fill), kNoButton = click only
    int pressable = 0;                            // the same while no registered action of the screen uses its button (logged on change)
    std::array<uint16_t, 512> guide_action{};     // key guide id -> its action + 1 (0 = not looked up yet)
    std::atomic<bool> click{false};               // a click on the button (the key guide's click routing, main thread)
    uint64_t logged_layout = 0;
};
State g;

// The item table's kind byte (record +0x3a), 0xff when the table cannot be read. Looked up once per id.
uint8_t item_kind(::Ctx* c, uint32_t id) {
    id &= kIdMask;
    if (g.kind_of[id]) return static_cast<uint8_t>(g.kind_of[id] - 1);
    uint8_t kind = 0xff;
    const uint64_t entry = tables::row_or_first(c, g.base, kItemTable, id);
    const uint64_t rec = entry ? rd<uint64_t>(entry + 8) : 0;
    if (rec && mem::is_mapped(rec + kItemKind, 1)) kind = rd<uint8_t>(rec + kItemKind);
    if (kind != 0xff) g.kind_of[id] = static_cast<uint8_t>(kind + 1);
    return kind;
}

struct Slot {
    uint64_t payload;   // packed u32 + count byte + 3 bytes, as stored at slot + 8
    uint32_t key;       // kind << 16 | id (empty slots are not keyed)
    uint32_t from;
};
using items::kCountMask;
using items::count_of;
using items::with_count;

void log_list(const char* what, uint64_t arr, uint32_t n) {
    char line[1600];
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 16 < sizeof line; ++i) {
        const uint32_t v = rd<uint32_t>(arr + i * 16 + 8);
        if (!(v & kIdMask)) continue;
        k += static_cast<size_t>(std::snprintf(line + k, sizeof line - k, " %u:%x*%u", i, v & kIdMask, rd<uint8_t>(arr + i * 16 + 0xc)));
    }
    line[k] = 0;
    NXLOG_I(kTag, "inventory sort: %s%s", what, line);
}

// Merges and sorts the n slots at arr. Returns false when nothing could be read.
bool sort_slots(::Ctx* c, uint64_t arr, uint32_t n, bool bag) {
    if (!arr || !n || n > kMaxSlots || !mem::is_mapped(arr, uint64_t{n} * 16)) return false;
    const uint64_t vt = rd<uint64_t>(arr);
    std::vector<Slot> items, empty;
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (rd<uint64_t>(arr + i * 16) != vt) return false;   // not one array of slots
        const uint64_t p = rd<uint64_t>(arr + i * 16 + 8);
        const uint32_t id = static_cast<uint32_t>(p) & kIdMask;
        if (!id) { empty.push_back({p, 0, i}); continue; }
        items.push_back({p, static_cast<uint32_t>(item_kind(c, id)) << 16 | id, i});
        total += count_of(p);
    }
    if (g.log_lists) log_list("before", arr, n);
    const size_t stacks_before = items.size();
    // Merge: a stack whose 8 bytes equal an earlier stack's but for the count tops the earlier ones up to the max stack (slot order);
    // what is left stays a stack of its own, an emptied slot joins the empty ones.
    std::vector<uint32_t> merged_into(n, ~0u);   // an emptied slot -> the slot its last items went to
    const uint32_t max = std::min<uint32_t>(static_cast<uint32_t>(call(c, g.base + kMaxStack, 0) & 0xffff), 0xff);
    if (max > 1) {
        std::vector<Slot> kept;
        kept.reserve(items.size());
        for (const Slot& s : items) {
            uint32_t left = count_of(s.payload), last = s.from;
            for (Slot& k : kept) {
                if (!left) break;
                const uint32_t have = count_of(k.payload);
                if ((k.payload & ~kCountMask) != (s.payload & ~kCountMask) || have >= max) continue;
                const uint32_t take = std::min(left, max - have);
                k.payload = with_count(k.payload, have + take);
                left -= take;
                last = k.from;
            }
            if (left) {
                kept.push_back({with_count(s.payload, left), s.key, s.from});
            } else {
                merged_into[s.from] = last;
                empty.push_back({0, 0, s.from});
            }
        }
        items.swap(kept);
    }
    std::stable_sort(items.begin(), items.end(), [](const Slot& a, const Slot& b) { return a.key < b.key; });
    std::vector<uint32_t> moved_to(n, ~0u);
    uint32_t at = 0;
    for (const Slot& s : items) { moved_to[s.from] = at; wr<uint64_t>(arr + at++ * 16 + 8, s.payload); }
    for (const Slot& s : empty) wr<uint64_t>(arr + at++ * 16 + 8, s.payload);
    if (bag) {   // the in-use slot index follows its item
        const uint64_t save = rd<uint64_t>(g.base + kSaveRoot);
        if (save && arr == save + kCommon + kBagOffline) {
            const uint64_t a = save + kCommon + kUseSlot;
            uint16_t old = rd<uint16_t>(a);
            if (old < n && merged_into[old] != ~0u) old = static_cast<uint16_t>(merged_into[old]);
            if (old < n && moved_to[old] != ~0u) wr<uint16_t>(a, static_cast<uint16_t>(moved_to[old]));
        }
    }
    uint64_t after = 0;
    for (uint32_t i = 0; i < n; ++i) after += rd<uint8_t>(arr + i * 16 + 0xc) * ((rd<uint32_t>(arr + i * 16 + 8) & kIdMask) != 0);
    if (g.log_lists) log_list("after ", arr, n);
    if (++g.sorts <= 64)
        NXLOG_I(kTag, "inventory sort: %s, %u slots, %zu stacks (%zu after merging, max stack %u), %llu items (%llu after)", bag ? "bag" : "container",
                n, stacks_before, items.size(), max, static_cast<unsigned long long>(total), static_cast<unsigned long long>(after));
    return true;
}

// Sorts the array of kind `kind` shown by the screen and refreshes its panels.
void sort_screen(::Ctx* c, uint64_t screen, uint8_t player, uint8_t kind) {
    if (kind == 4 || kind == 9) return;   // the bag's sub-area and the special kind 9 are left alone
    const uint8_t type = rd<uint8_t>(screen + kType);
    const uint16_t id = rd<uint16_t>(screen + kId);
    uint64_t arr = call(c, g.base + kInventory, player, kind, type, id, 0);
    uint32_t n = kBagSlots;
    if (kind != 0) {   // containers: the call returns the whole store; the container is count slots from its first one
        const uint64_t buf = (c->x[31] - 0x80) & ~uint64_t{0xf};
        std::memset(reinterpret_cast<void*>(buf), 0, 16);
        if (!(call(c, g.base + kContainerSlots, type, id, buf, buf + 4, 0, buf - 0x40) & 1)) return;
        const uint32_t first = rd<uint32_t>(buf);
        n = rd<uint32_t>(buf + 4);
        if (!arr || first + uint64_t{n} > kStoreSlots) return;
        arr += uint64_t{first} * 16;
        if (g.log_lists) NXLOG_I(kTag, "inventory sort: container type %u id %u, store slots %u..%u", type, id, first, first + n);
    }
    if (sort_slots(c, arr, n, kind == 0)) call(c, g.base + kRefresh, screen);
}

// The button of an action (input manager table), -1 if unknown.
int button_of(uint16_t act) {
    const uint64_t im = rd<uint64_t>(g.base + kInputMgr);
    if (!im || !mem::is_mapped(im + kActionButtons, 16)) return -1;
    const uint64_t b = rd<uint64_t>(im + kActionButtons), e = rd<uint64_t>(im + kActionButtons + 8);
    if (!b || e < b || act >= (e - b) / 4 || !mem::is_mapped(b + act * 4ull, 4)) return -1;
    return rd<uint8_t>(b + act * 4ull);
}

// Marks the buttons of the screen's registered actions (all modes) in `used`. False when the list cannot be read.
bool mark_registered(uint64_t screen, std::array<bool, 256>& used) {
    const uint64_t m = rd<uint64_t>(screen + kActions);
    if (!m || !mem::is_mapped(m, 0x20)) return false;
    const uint64_t d = rd<uint64_t>(m + 8), n = rd<uint64_t>(m + 0x10);
    if (n > 64 || (n && !mem::is_mapped(d, n * 0x30))) return false;
    for (uint64_t i = 0; i < n; ++i) {
        const int b = button_of(rd<uint16_t>(d + i * 0x30 + 0x18));
        if (b >= 0) used[b] = true;
    }
    return true;
}

int hook_screen_frame(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (!screen || !mem::is_mapped(screen, 0x1a8) || rd<uint64_t>(screen) != g.base + kScreenVtable) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (screen != g.screen || now - g.last_frame > kReopenGap) {   // the screen (re)opened
        g.screen = screen;
        g.auto_pending = g.autosort;
        g.prev_buttons = ~uint64_t{0};   // a button still held from before the screen opened does not count
        g.click = false;
        if (g.log_lists)
            NXLOG_I(kTag, "inventory sort: screen %llx open, state %u, kinds %u/%u, type %u id %u", static_cast<unsigned long long>(screen),
                    rd<uint8_t>(screen + kState), rd<uint8_t>(screen + kScreenKind), rd<uint8_t>(screen + kCursorKind),
                    rd<uint8_t>(screen + kType), rd<uint16_t>(screen + kId));
    }
    g.last_frame = now;

    // The prompt's button sorts only while no action of the screen is on it (the action list is complete one frame after a state change;
    // the prompt was chosen from the shown set): otherwise the prompt takes clicks only, and the game keeps the button.
    int pick = g.shown.load();
    if (pick != kNoButton && rd<uint8_t>(screen + kState) == 1) {
        std::array<bool, 256> used{};
        if (!mark_registered(screen, used) || used[kSortButtons[pick].button]) pick = kNoButton;
    }
    if (pick != g.pressable && g.log_lists && rd<uint8_t>(screen + kState) == 1)
        NXLOG_I(kTag, "inventory sort: sort button now %s", pick == kNoButton ? "none (click only)" : kSortButtons[pick].name);
    if (rd<uint8_t>(screen + kState) == 1) g.pressable = pick;
    const uint64_t mask = g.pressable == kNoButton ? 0 : hid::btn(kSortButtons[g.pressable].pad);
    uint64_t buttons = 0;
    if (g.button && mask) {
        if (auto src = hid::input_source()) {
            hid::InputSnapshot snap;
            src->poll(snap);
            buttons = (snap.npad[0].buttons | snap.npad[hid::kNpadSlotHandheld].buttons) & mask;
        }
    }
    const bool clicked = g.click.exchange(false);
    const bool pressed = (buttons && !g.prev_buttons) || clicked;
    g.prev_buttons = buttons;
    const bool online = call(c, g.base + kOnline, 0) & 1;
    g.online = online;
    if (!pressed && !g.auto_pending) return 0;

    const uint8_t state = rd<uint8_t>(screen + kState);
    const uint8_t player = static_cast<uint8_t>(call(c, g.base + kPlayerOf, screen + kPlayerHelper));
    if (pressed && g.log_lists)
        NXLOG_I(kTag, "inventory sort: %s, state %u, player %u, temp %u, online %u, cursor kind %u", clicked ? "click" : "button", state, player,
                player < 2 ? rd<uint8_t>(g.base + kTempFlag + player) : 0xff, online, rd<uint8_t>(screen + kCursorKind));
    if (state != 1) return 0;   // browsing only, never while an item is held
    if (player > 1 || rd<uint8_t>(g.base + kTempFlag + player)) return 0;
    if (online) { g.auto_pending = false; return 0; }   // Link Play: off
    if (g.auto_pending) {
        g.auto_pending = false;
        sort_screen(c, screen, player, 0);
    }
    if (pressed) {
        const uint8_t kind = rd<uint8_t>(screen + kCursorKind);
        if (kind == 0 || kind == rd<uint8_t>(screen + kScreenKind)) sort_screen(c, screen, player, kind);
    }
    return 0;
}

// ---- the key guide button ----

// The screen is open now (its per-frame hook ran within the reopen gap).
bool screen_up() { return g.screen && std::chrono::steady_clock::now() - g.last_frame < kReopenGap; }

void set_taken(::Ctx* c, uint64_t widget, uint32_t set) {
    if (!g.row) g.row = key_guide::guide_row(c, kSortGuide);
    if (g.log_lists && screen_up()) {
        char acts[200];
        size_t k = 0;
        acts[0] = 0;
        const uint64_t m = rd<uint64_t>(g.screen + kActions);
        const uint64_t d = m ? rd<uint64_t>(m + 8) : 0, n = m ? rd<uint64_t>(m + 0x10) : 0;
        for (uint64_t i = 0; d && i < n && i < 16 && k + 8 < sizeof acts; ++i)
            k += static_cast<size_t>(std::snprintf(acts + k, sizeof acts - k, " %x/%u", rd<uint16_t>(d + i * 0x30 + 0x18), rd<uint32_t>(d + i * 0x30 + 0x20)));
        NXLOG_I(kTag, "inventory sort: key guide set %u on widget %llx; screen state %u, actions (action/mode):%s", set,
                static_cast<unsigned long long>(widget), rd<uint8_t>(g.screen + kState), acts);
    }
}

// Before the widget lays out its parts: while the screen browses, the set's "Hold to Sort" entry (126, X held, the second X prompt next
// to "Swap"; every browsing set of these screens has it, the holding sets do not) becomes the Sort entry. The widget has six part slots
// (its layout pass stops at six), all in use, so one entry has to give way; X held still rearranges as before. The fill rebuilds the
// entries from the set, so this runs on every layout pass.
void laying_out(::Ctx*, uint64_t w) {
    if (!w || g.online || !screen_up() || rd<uint8_t>(g.screen + kState) != 1) return;
    key_guide::Entries e;
    if (!key_guide::entries(w, &e)) return;
    for (uint64_t i = 0; i < e.count; ++i)
        if (e.id(i) == kHoldToSortGuide) {
            e.set_id(i, kSortGuide);
            g.widget = w;
        }
}

// The Sort part is filled: the sort button's icon, and its layout for the menu pointer.
void part_filled(::Ctx* c, const key_guide::Part& p) {
    if (!g.row || p.row != g.row) return;
    const uint64_t w = g.widget.load();
    if (!w || p.widget != w) return;
    // the fill follows a state change in the same frame, after the per-frame hook ran, and maybe before the new actions are registered
    // The button: the first of kSortButtons that no other prompt of the shown set uses (the set is what the player sees; the screen's
    // action list may still be the one of the previous state here, so it is checked per frame instead).
    std::array<bool, 256> used{};
    key_guide::Entries e;
    if (!key_guide::entries(w, &e)) e.count = 0;
    for (uint64_t i = 0; i < e.count; ++i) {
        const uint16_t id = e.id(i);
        if (id == kSortGuide || id >= g.guide_action.size()) continue;
        if (!g.guide_action[id]) {   // the key guide's row: {u16 action, ...} at [row + 8]
            const uint64_t row = key_guide::guide_row(c, id);
            const uint64_t rec = row ? rd<uint64_t>(row + 8) : 0;
            if (!rec || !mem::is_mapped(rec, 2)) continue;
            g.guide_action[id] = static_cast<uint16_t>(rd<uint16_t>(rec) + 1);
        }
        const int b = button_of(static_cast<uint16_t>(g.guide_action[id] - 1));
        if (b >= 0) used[b] = true;
    }
    int pick = kNoButton;
    for (int i = 0; i < static_cast<int>(std::size(kSortButtons)) && pick == kNoButton; ++i)
        if (button_of(kSortButtons[i].act) == kSortButtons[i].button && !used[kSortButtons[i].button]) pick = i;
    g.shown = pick;
    if (pick != kNoButton) key_guide::set_icon(c, p, kSortButtons[pick].act);
    if (g.log_lists) NXLOG_I(kTag, "inventory sort: Sort prompt button %s", pick == kNoButton ? "none (click only)" : kSortButtons[pick].name);
    const uint64_t lay = key_guide::layout_of(c, p);
    key_guide::set_click_layout(g.client, lay);
    if (g.log_lists && lay != g.logged_layout) {
        g.logged_layout = lay;
        NXLOG_I(kTag, "inventory sort: Sort button part %llx (layout %llx) on widget %llx", static_cast<unsigned long long>(p.part),
                static_cast<unsigned long long>(lay), static_cast<unsigned long long>(w));
    }
}

// Main thread: a left click on the Sort part. The bag / container screen sorts at its next frame. Not taken unless the screen is up and
// its key guide shows the Sort entry (the widget reuses its part instances for other sets, whose clicks go on as before).
bool click(play::Act* act) {
    const uint64_t w = g.widget.load();
    key_guide::Entries e;
    if (!screen_up() || !key_guide::entries(w, &e)) return false;
    for (uint64_t i = 0; i < e.count; ++i)
        if (e.id(i) == kSortGuide) {   // the widget shows the Sort entry now
            g.click = true;
            *act = play::Act::None;
            return true;
        }
    return false;
}

void install(const FeatureInstall& in) {
    const uint64_t base = in.base;
    g.base = base;
    g.button = settings::get_bool(settings::Key::InventorySort);
    g.autosort = settings::get_bool(settings::Key::InventoryAutosort);
    g.log_lists = settings::is_set(settings::Key::MenuLog) && settings::get_int(settings::Key::MenuLog) > 0;   // only when set: the menu's default 1 does not log the lists
    if (!g.button && !g.autosort) return;
    in.hooks.add_entry(base + kScreenFrame, &hook_screen_frame);
    if (g.button)
        g.client = key_guide::add({.name = "inventory sort", .set_taken = &set_taken, .laying_out = &laying_out, .part_filled = &part_filled,
                                   .click = &click, .click_note = "the Sort key guide -> inventory sort"});
    NXLOG_I(kTag, "inventory sort: button %s, autosort %s (hook main+0x%llx; the button is a key guide entry)", g.button ? "on" : "off",
            g.autosort ? "on" : "off", static_cast<unsigned long long>(kScreenFrame));
}

}  // namespace

const Feature kSortFeature = {.name = "sort", .install = &install};

}  // namespace nxrt::games::pkp
