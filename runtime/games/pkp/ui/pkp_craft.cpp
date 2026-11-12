// SPDX-License-Identifier: GPL-3.0-or-later
// Crafting search (#324): in the crafting screen a "(Y) Search" key guide sits in the screen's own key guide; Y (keyboard F through the default
// btn_y binding) or a left click on it asks for a word on the game's own keyboard (the one of name entry, filled with the current word), and the
// recipe list then keeps only the recipes whose item name holds that word (any case), on every tab and after the game's own Sort, until Cancel
// (the game's text check refuses an empty field) or until the screen closes. [pkp] craft_search (off by default). Nothing is shown or done
// during Link Play.
//
// How the game builds the list (main 2.0.1a, measured with snapshot diffs and page traps in scripted runs):
//  * The crafting screen (every station) is UI class 0xe3 (ctor 0x71010381ac, size 0x12f0, vtable pointer main+0x333cf28), one instance made
//    at boot and reused. Its virtuals: +0x38 0x7100eeae14 (the screen opens: builds the tabs and lists), +0x48 0x7100eec660 (per frame, only
//    while it shows; called on a UI worker thread). Fields: +0x48 its child table (0x7100a6f92c (table, element, u16* class) -> child),
//    +0xa8 state (1 = browsing the list; other values are its dialogs, the Sort popup and the amount picker).
//  * The recipe grid is child element 0x1fd (class 0xe4; 0x1fe is the other page, a tab change fills it and swaps the two). Its list model is
//    [[grid + 0x80] + 8][0]: +8 -> vector {entries, count, cap 1500} of 12-byte entries {u32 0xb, u16 recipe id, u32 0}, +0x98 the cursor.
//    0x7100ef0b80 (grid, list, tab) fills it (list 17 = the workbench's, tab = the tab's category): count, cursor and scroll 0, the ids from
//    0x7100e634e0 (u16 vector, cap 1500), one entry per id. 0x7100ef0734 (grid, list, tab, cursor, scroll) calls it, puts the cursor (clamped)
//    and gives an empty list one {0xb, 0} entry (the grid then shows the game's empty-list text); 0x7100e75028 (grid) makes the cells show the
//    vector; 0x7100eed680 (detail pane = child 0x1ff, recipe id) shows a recipe's picture, name and materials. The fill runs when the screen
//    opens, on every tab change and after the game's Sort (X).
//  * The filter sits in the fill, at 0x7100ef0c38 (right after 0x7100e634e0 returns; the ids at [sp + 8], their count at [sp + 0x10],
//    w21 list, w20 tab): ids whose name does not match are dropped, so every list the game builds is filtered.
//  * Recipe -> name: the recipe table [main+0x35618f0] +0x10 +0x330 (+0 while the manager holds < 0x67 tables), row = vf+0x38(0) then
//    vf+0x30(id), record [row + 8]: u16 +0x12 = the result item; item table +0x638 (+0 below 200 tables), record +0x22 = name id;
//    0x7100a34e1c (name id, 0) -> the singular name in the game's language (UTF-8, lower case; [main+0x36ca680] -> u32 language).
//  * The keyboard: 0x7101131700 (mode, buf, size, options) is the game's blocking text request (name entry uses it): it starts the keyboard job
//    and sleeps in 10 ms steps until the answer. Called from the screen's per-frame virtual it holds that UI worker: the list takes no input
//    while the keyboard is up, the main loop goes on presenting frames with the runtime's on-screen keyboard on top, and the runtime keeps the
//    pads and keys away from the game while it is open. Mode 6 takes the 32-byte options {+0 header, +0x10 guide text (UTF-8 char*), +0x18 u16
//    min length, +0x1a u16 max length, +0x1d u8 1}; the initial text is buf; the game's text check refuses an empty answer. Buffers are guest
//    heap blocks (0x7101121ee8 ([main+0x3483d58] -> allocator, 3, size, 0x10, &{u32 0x35, u64 0})), kept for the session.
//  * The button: the screen's prompts are the key guide widget (pkp_key_guide.h; this is a client) fed set 26 ({Confirm, Close, Favorite};
//    only the crafting screen's browsing state uses it). When the widget takes the set, key guide 158 (the game's own "Search", the
//    Pokédex's) is put in the set's first free slot (the widget lays out six), and when its part is filled its icon becomes Y (action 0x97),
//    its text "Search: <word>" while a word is active (the widget takes set 26 again after a new word), and its layout goes to the menu
//    pointer (the key guide's click routing, pkp_menu.cpp): a click on it asks too. Y is free in this screen (A Confirm, B Close, X Sort,
//    (+) Favorite, (-) Details, L / R tabs).
//  * Link Play (0x71000376c4 true): the list is local UI, but the key guide is left as the game has it and nothing is filtered.
#include "ui/pkp_craft.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "ui/pkp_key_guide.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_tables.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kScreenOpen = 0xeeae14;       // entry: crafting screen virtual +0x38 (opens), x0 = the screen
constexpr uint64_t kScreenFrame = 0xeec660;      // entry: crafting screen virtual +0x48 (per frame), x0 = the screen
constexpr uint64_t kFillIds = 0xef0c38;          // inline in the grid fill 0x7100ef0b80, after the ids came
constexpr uint64_t kScreenVtable = 0x333cf28;    // the vtable pointer the screen holds
using layout::kChildOf;
constexpr uint64_t kGridFill = 0xef0734;         // (grid, list, tab, cursor, scroll)
constexpr uint64_t kGridShow = 0xe75028;         // (grid)
constexpr uint64_t kDetailShow = 0xeed680;       // (detail pane, recipe id)
constexpr uint64_t kAsk = 0x1131700;             // (mode, char* buf, size, options*) -> bit 0: OK
constexpr uint64_t kAlloc = 0x1121ee8;           // (allocator, 3, size, align, tag*)
constexpr uint64_t kAllocator = 0x3483d58;
using layout::kOnline;
constexpr uint64_t kRecipeTable = 0x330;
using layout::kLanguage;
constexpr uint64_t kChildren = 0x48, kState = 0xa8;
constexpr uint32_t kGridElem = 0x1fd, kDetailElem = 0x1ff;
constexpr uint16_t kGridClass = 0xe4, kDetailClass = 0xe5;
constexpr uint64_t kModel = 0x80, kCursor = 0x98;
// the key guide
constexpr uint32_t kCraftSet = 26;
constexpr uint16_t kSearchGuide = 158;           // "Search"
constexpr uint16_t kActionY = 0x97;              // an action shown with the Y icon
constexpr uint32_t kMaxLen = 20;                 // characters
constexpr auto kReopenGap = std::chrono::milliseconds(300);

// "Search" / the keyboard's guide text in the game's languages (0..8: JP EN FR IT DE ES zh-Hant KO zh-Hans)
constexpr const char* kSearchWord[9] = {"検索", "Search", "Rechercher", "Cerca", "Suche", "Buscar", "搜尋", "검색", "搜索"};
constexpr const char* kGuideWord[9] = {"アイテムの名前", "Item name", "Nom de l'objet", "Nome dello strumento", "Item-Name", "Nombre del objeto",
                                       "道具名稱", "도구 이름", "道具名称"};

using guest::rd;
using guest::wr;
using guest::ok;
using guest::call;

struct State {
    uint64_t base = 0;
    bool log = false;
    uint64_t screen = 0, prev_buttons = 0;
    std::chrono::steady_clock::time_point last_frame{};
    std::mutex mu;                                 // query (guest UI thread) against the click / label readers
    std::string query;                             // as typed (the keyboard's initial text)
    std::string folded;                            // case-folded; empty = no filter
    bool have_fill = false;
    uint32_t list = 0, tab = 0;                    // the last fill's list and tab
    uint32_t lang = ~0u;
    std::unordered_map<uint16_t, std::string> names;   // recipe id -> folded result name (this language)
    uint64_t buf = 0, opts = 0, strs = 0;          // guest blocks for the keyboard
    // the key guide
    int client = -1;                               // the key guide client
    std::atomic<uint64_t> widget{0};               // the key guide widget that took set 26 last
    uint64_t row = 0;                              // key guide 158's table row
    uint64_t set_ids = 0;                          // set 26's six u16 ids
    std::atomic<bool> online{false};
    std::atomic<bool> click{false};
};
State g;

uint32_t language() {
    const uint64_t p = rd<uint64_t>(g.base + kLanguage);
    const uint32_t l = p && ok(p, 4) ? rd<uint32_t>(p) : 1;
    return l < 9 ? l : 1;
}

// A data-table record (record [row + 8]; the table at +0 while the manager does not hold `off`, pkp_tables.h); 0 if none.
uint64_t table_rec(::Ctx* c, uint64_t off, uint32_t id) {
    const uint64_t row = tables::row_or_first(c, g.base, off, id);
    return row && ok(row + 8, 8) ? rd<uint64_t>(row + 8) : 0;
}

// Case folding for the match: ASCII, Latin-1 and fullwidth Latin capitals to small; markup tags (<...>) dropped. Works on whole UTF-8 sequences,
// so a match never starts or ends inside a character.
std::string fold(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        if (b == '<') {   // the name banks carry tags such as <fcap>
            const size_t e = s.find('>', i);
            if (e != std::string::npos) { i = e + 1; continue; }
        }
        size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (i + n > s.size()) n = 1;
        uint32_t cp = b;
        if (n == 2) cp = (b & 0x1fu) << 6 | (s[i + 1] & 0x3fu);
        if (n == 3) cp = (b & 0x0fu) << 12 | (s[i + 1] & 0x3fu) << 6 | (s[i + 2] & 0x3fu);
        if (n == 1 && b >= 'A' && b <= 'Z') { r += static_cast<char>(b + 32); ++i; continue; }
        if (n == 2 && cp >= 0xc0 && cp <= 0xde && cp != 0xd7) {
            cp += 0x20;
            r += static_cast<char>(0xc0 | cp >> 6);
            r += static_cast<char>(0x80 | (cp & 0x3f));
            i += 2;
            continue;
        }
        if (n == 3 && cp >= 0xff21 && cp <= 0xff3a) {
            cp += 0x20;
            r += static_cast<char>(0xe0 | cp >> 12);
            r += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            r += static_cast<char>(0x80 | (cp & 0x3f));
            i += 3;
            continue;
        }
        r.append(s, i, n);
        i += n;
    }
    return r;
}

// The folded name of a recipe's result item, looked up once per language.
const std::string& recipe_name(::Ctx* c, uint16_t rid) {
    const uint32_t lang = language();
    if (lang != g.lang) { g.names.clear(); g.lang = lang; }
    auto it = g.names.find(rid);
    if (it != g.names.end()) return it->second;
    const uint64_t rec = table_rec(c, kRecipeTable, rid);
    const uint16_t item = rec && ok(rec + 0x12, 2) ? rd<uint16_t>(rec + 0x12) : 0;
    return g.names.emplace(rid, fold(items::name(c, g.base, item))).first->second;
}

uint64_t child(::Ctx* c, uint64_t screen, uint32_t elem, uint16_t want) {
    const uint64_t sp = (c->x[31] - 0x200) & ~uint64_t{0xf};
    wr<uint16_t>(sp + 0x100, 0);
    const uint64_t o = call(c, g.base + kChildOf, screen + kChildren, elem, sp + 0x100, 0, 0, sp);
    return o && rd<uint16_t>(sp + 0x100) == want ? o : 0;
}

uint64_t galloc(::Ctx* c, uint64_t size) {
    const uint64_t a = rd<uint64_t>(g.base + kAllocator);
    if (!a) return 0;
    const uint64_t sp = (c->x[31] - 0x200) & ~uint64_t{0xf};
    wr<uint32_t>(sp + 0x100, 0x35);
    wr<uint64_t>(sp + 0x108, 0);
    return call(c, g.base + kAlloc, rd<uint64_t>(a), 3, size, 0x10, sp + 0x100, sp);
}

// The screen shows its browsing key guide (set 26 with the Search entry) now: the widget that took set 26 has taken no other set since (the
// fill hook clears g.widget then) and is shown (state 1 or 2).
bool guide_up() { return key_guide::shown(g.widget.load()); }
bool screen_up() { return g.screen && std::chrono::steady_clock::now() - g.last_frame < kReopenGap; }

// ---- the list ----

// In the fill, after the ids came: drop the ids whose result name does not hold the word.
void hook_fill_ids(::Ctx* c, uint64_t, void*) {
    g.list = static_cast<uint32_t>(c->x[21]) & 0xff;
    g.tab = static_cast<uint32_t>(c->x[20]) & 0xff;
    g.have_fill = true;
    std::string folded, query;   // copied under g.mu, which ask and clear_query hold while they change them
    {
        std::lock_guard<std::mutex> lk(g.mu);
        folded = g.folded;
        query = g.query;
    }
    if (folded.empty() || g.online) return;
    const uint64_t sp = c->x[31];
    const uint64_t ids = rd<uint64_t>(sp + 8), n = rd<uint64_t>(sp + 0x10);
    if (!ids || n == 0 || n > 0x5dc || !ok(ids, n * 2)) return;
    uint64_t k = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const uint16_t id = rd<uint16_t>(ids + i * 2);
        if (id == 0 || recipe_name(c, id).find(folded) == std::string::npos) continue;
        wr<uint16_t>(ids + k++ * 2, id);
    }
    wr<uint64_t>(sp + 0x10, k);
    if (g.log)
        NXLOG_I(kTag, "craft search: '%s' keeps %llu of %llu recipes (list %u, tab %u)", query.c_str(), static_cast<unsigned long long>(k),
                static_cast<unsigned long long>(n), g.list, g.tab);
}

uint64_t grid_model(uint64_t grid) {
    const uint64_t args = grid ? rd<uint64_t>(grid + kModel) : 0;
    const uint64_t m = args && ok(args, 0x18) && rd<uint64_t>(args + 0x10) ? rd<uint64_t>(rd<uint64_t>(args + 8)) : 0;
    return m && ok(m, 0xa0) ? m : 0;
}
int grid_cursor(::Ctx* c, uint64_t screen) {
    const uint64_t m = grid_model(child(c, screen, kGridElem, kGridClass));
    return m ? rd<int32_t>(m + kCursor) : -1;
}

// After a new word: the front grid is filled again (through the hook above), the cells and the detail pane follow.
void refill(::Ctx* c, uint64_t screen) {
    const uint64_t grid = child(c, screen, kGridElem, kGridClass);
    if (!grid || !g.have_fill) return;
    call(c, g.base + kGridFill, grid, g.list, g.tab, 0, 0);
    call(c, g.base + kGridShow, grid);
    const uint64_t model = grid_model(grid);
    const uint64_t v = model ? rd<uint64_t>(model + 8) : 0;
    const uint64_t n = v ? rd<uint64_t>(v + 8) : 0;
    const uint16_t first = n ? rd<uint16_t>(rd<uint64_t>(v) + 4) : 0;
    if (first)
        if (const uint64_t det = child(c, screen, kDetailElem, kDetailClass)) call(c, g.base + kDetailShow, det, first);
    if (g.log)
        NXLOG_I(kTag, "craft search: list filled again, %llu entries, first recipe %u, cursor %d", static_cast<unsigned long long>(n), first,
                model ? rd<int32_t>(model + kCursor) : -1);
}

// Y / click: the game's keyboard (0x7101131700 waits for the answer on the calling thread, as for a name), then the new word.
void ask(::Ctx* c, uint64_t screen) {
    if (!g.buf) {
        g.buf = galloc(c, 0x100);
        g.opts = galloc(c, 0x20);
        g.strs = galloc(c, 0x100);
    }
    if (!g.buf || !g.opts || !g.strs) {
        NXLOG_W(kTag, "craft search: no guest memory for the keyboard");
        return;
    }
    const uint32_t lang = language();
    std::memset(reinterpret_cast<void*>(g.buf), 0, 0x100);
    std::memcpy(reinterpret_cast<void*>(g.buf), g.query.data(), std::min<size_t>(g.query.size(), 0xff));
    std::memset(reinterpret_cast<void*>(g.opts), 0, 0x20);
    std::memset(reinterpret_cast<void*>(g.strs), 0, 0x100);
    std::snprintf(reinterpret_cast<char*>(g.strs), 0x40, "%s", kSearchWord[lang]);
    std::snprintf(reinterpret_cast<char*>(g.strs + 0x40), 0x80, "%s", kGuideWord[lang]);
    wr<uint64_t>(g.opts + 0x00, g.strs);
    wr<uint64_t>(g.opts + 0x10, g.strs + 0x40);
    wr<uint16_t>(g.opts + 0x18, 0);
    wr<uint16_t>(g.opts + 0x1a, static_cast<uint16_t>(kMaxLen));
    wr<uint8_t>(g.opts + 0x1d, 1);
    const int cursor_before = grid_cursor(c, screen);
    if (g.log) NXLOG_I(kTag, "craft search: asking (initial '%s')", g.query.c_str());
    const bool answered = call(c, g.base + kAsk, 6, g.buf, 0x100, g.opts) & 1;
    std::string word(reinterpret_cast<const char*>(g.buf), strnlen(reinterpret_cast<const char*>(g.buf), 0xff));
    if (g.log)
        NXLOG_I(kTag, "craft search: keyboard %s '%s' (grid cursor %d before, %d after)", answered ? "answered" : "cancelled", word.c_str(), cursor_before,
                grid_cursor(c, screen));
    // OK: the new word. Cancel: no search (the game's text check refuses an empty field, so Cancel is the way back to every recipe)
    std::string f = answered ? fold(word) : std::string();
    while (!f.empty() && f.back() == ' ') f.pop_back();
    while (!f.empty() && f.front() == ' ') f.erase(0, 1);
    if (f == g.folded) return;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.query = f.empty() ? std::string() : word;
        g.folded = f;
    }
    refill(c, screen);
    // the key guide shows the word: the widget takes its set again
    if (const uint64_t w = g.widget.load(); w && guide_up()) key_guide::take_set(c, w, kCraftSet);
}

void clear_query() {
    std::lock_guard<std::mutex> lk(g.mu);
    g.query.clear();
    g.folded.clear();
}

int hook_screen_open(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (!screen || !ok(screen, 0x100) || rd<uint64_t>(screen) != g.base + kScreenVtable) return 0;
    if (g.log && !g.folded.empty()) NXLOG_I(kTag, "craft search: screen opens, search '%s' ends", g.query.c_str());
    clear_query();
    return 0;
}

int hook_screen_frame(::Ctx* c, uint64_t, void*) {
    const uint64_t screen = c->x[0];
    if (!screen || !ok(screen, 0x100) || rd<uint64_t>(screen) != g.base + kScreenVtable) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (screen != g.screen || now - g.last_frame > kReopenGap) {   // the screen (re)opened
        g.screen = screen;
        g.prev_buttons = ~uint64_t{0};   // a button still held from before does not count
        g.click = false;
    }
    g.last_frame = now;
    g.online = call(c, g.base + kOnline, 0) & 1;
    uint64_t buttons = 0;
    if (auto src = hid::input_source()) {
        hid::InputSnapshot snap;
        src->poll(snap);
        buttons = (snap.npad[0].buttons | snap.npad[hid::kNpadSlotHandheld].buttons) & hid::btn(hid::kBtnY);
    }
    const bool clicked = g.click.exchange(false);
    const bool pressed = (buttons && !g.prev_buttons) || clicked;
    g.prev_buttons = buttons;
    if (!pressed) return 0;
    const uint8_t state = rd<uint8_t>(screen + kState);
    if (g.log) {
        const uint64_t w = g.widget.load();
        NXLOG_I(kTag, "craft search: %s, state %u, key guide widget %llx (state %d), online %d", clicked ? "click" : "Y", state, static_cast<unsigned long long>(w),
                key_guide::state(w), g.online.load() ? 1 : 0);
    }
    if (g.online || state != 1 || !guide_up()) return 0;   // browsing only, with the Search prompt up
    ask(c, screen);
    return 0;
}

// ---- the key guide ----

void set_taken(::Ctx* c, uint64_t w, uint32_t set) {
    if (set != kCraftSet) {
        if (w == g.widget.load()) g.widget = 0;
        return;
    }
    g.widget = w;
    if (!g.set_ids) {
        const uint64_t ids = key_guide::set_ids(c, kCraftSet);
        g.row = key_guide::guide_row(c, kSearchGuide);
        if (!ids || !g.row) return;
        g.set_ids = ids;
    }
    // Search in the first free slot, or out of the set while online
    const bool online = call(c, g.base + kOnline, 0) & 1;
    g.online = online;
    int at = -1, free_slot = -1;
    for (int i = 0; i < key_guide::kSetSlots; ++i) {
        const uint16_t v = rd<uint16_t>(g.set_ids + 2 * i);
        if (v == kSearchGuide) at = i;
        if (v == 0 && free_slot < 0) free_slot = i;
    }
    if (!online && at < 0 && free_slot >= 0) wr<uint16_t>(g.set_ids + 2 * free_slot, kSearchGuide);
    if (online && at >= 0) {   // close the gap: the ids after it move up
        for (int i = at; i + 1 < key_guide::kSetSlots; ++i) wr<uint16_t>(g.set_ids + 2 * i, rd<uint16_t>(g.set_ids + 2 * (i + 1)));
        wr<uint16_t>(g.set_ids + 2 * (key_guide::kSetSlots - 1), 0);
    }
}

void part_filled(::Ctx* c, const key_guide::Part& p) {
    if (!g.row || p.row != g.row) return;
    const uint64_t w = g.widget.load();
    if (!w || p.widget != w) return;
    key_guide::set_icon(c, p, kActionY);
    std::string label;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.query.empty()) label = std::string(kSearchWord[language()]) + ": " + g.query;
    }
    if (!label.empty()) key_guide::set_text(c, p, label.c_str());
    key_guide::set_click_layout(g.client, key_guide::layout_of(c, p));
}

// Main thread: a left click on the Search part; the screen asks at its next frame. Not taken unless the crafting screen is up and its
// key guide shows the Search entry.
bool click(play::Act* act) {
    if (!screen_up() || !guide_up() || g.online) return false;
    g.click = true;
    *act = play::Act::None;
    return true;
}

void install(const FeatureInstall& in) {
    const uint64_t base = in.base;
    if (!settings::get_bool(settings::Key::CraftSearch)) {
        NXLOG_I(kTag, "craft search: off ([pkp] craft_search)");
        return;
    }
    g.base = base;
    g.log = settings::is_set(settings::Key::MenuLog) && settings::get_int(settings::Key::MenuLog) > 0;   // only when set: the menu's default 1 does not log the searches
    in.hooks.add_entry(base + kScreenOpen, &hook_screen_open);
    in.hooks.add_entry(base + kScreenFrame, &hook_screen_frame);
    in.hooks.add_inline(base + kFillIds, &hook_fill_ids);
    g.client = key_guide::add({.name = "craft search", .set_taken = &set_taken, .part_filled = &part_filled, .click = &click,
                               .click_note = "the Search key guide -> crafting search"});
    NXLOG_I(kTag, "craft search: on (a (Y) Search key guide in the crafting screen; hooks main+0x%llx, +0x%llx, +0x%llx)",
            static_cast<unsigned long long>(kScreenOpen), static_cast<unsigned long long>(kScreenFrame), static_cast<unsigned long long>(kFillIds));
}

}  // namespace

const Feature kCraftFeature = {.name = "craft", .install = &install};

}  // namespace nxrt::games::pkp
