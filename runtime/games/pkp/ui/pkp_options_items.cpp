// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the Items tab (pkp_options_model.h): the item editor's screen (pkp_item_edit.h, [pkp] item_editor).
// "Item" picks one of the bag's stacks, read when the screen opens; "New count" is the count that stack gets when the screen commits
// ("Keep" changes nothing). A discard drops it like every other row. While the bag cannot be edited (the mod is off, Link Play, no save)
// the tab holds one read-only row that says why. The screen's hooks run on the game thread, so the bag is read and written with that
// thread's Ctx (kernel::current_ctx), as item_edit asks.
#include "ui/pkp_options_model.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "nxrt/kernel.h"
#include "ui/pkp_item_edit.h"

namespace nxrt::games::pkp::options_detail {
namespace {

constexpr int kItemRows = 2;

item_edit::Bag g_bag;                          // the bag when the screen opened
std::string g_labels[256];                     // the Item row's text per stack (a slider has at most 256 positions)
Row g_item_rows[kItemRows];                    // filled by ItemsTab::open

// A name as the row shows it: without the game's markup tags (<...>), or the item's number when it has no name.
std::string shown_name(const item_edit::Stack& s) {
    std::string out;
    bool tag = false;
    for (const char ch : s.name) {
        if (ch == '<') tag = true;
        else if (ch == '>') tag = false;
        else if (!tag) out += ch;
    }
    if (out.empty()) {
        char num[24];
        std::snprintf(num, sizeof num, "Item %u", s.id);
        out = num;
    }
    return out;
}

int stack_count() { return static_cast<int>(std::min<size_t>(g_bag.stacks.size(), std::size(g_labels))); }

const char* item_text(int v, char*, size_t) { return v >= 0 && v < stack_count() ? g_labels[v].c_str() : ""; }
int item_max() { return std::max(stack_count() - 1, 0); }
int count_max() { return static_cast<int>(g_bag.max_stack); }

constexpr const char* kUnavailable[4] = {"Unavailable", "Unavailable"};

// Reads the bag again and fills the rows; the number of rows the tab shows.
int load_items(::Ctx* c) {
    g_bag = c ? item_edit::read_bag(c) : item_edit::Bag{};
    const auto none = [](const S&) { return 0; };
    const auto keep = [](S&, int) {};
    if (g_bag.stacks.empty()) {
        g_item_rows[0] = Row{"Item editor", g_bag.why, {kUnavailable[0], kUnavailable[1]}, 2, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr, none, keep, false, false};
        return 1;
    }
    for (int i = 0; i < stack_count(); ++i) g_labels[i] = shown_name(g_bag.stacks[i]) + " x" + std::to_string(g_bag.stacks[i].count);
    g_item_rows[0] = dynamic("Item", "The stack in your bag to change. The list is read when this screen opens.", &item_text, &item_max, none, keep);
    g_item_rows[1] = Row{"New count", "The count the stack gets when you leave this screen. Keep changes nothing. Items made this way are not earned: keep a backup of your save.",
                         {}, 0, 0, 0, "%.0f", 0, 1, nullptr, "Keep", nullptr, &count_max, none, keep, false, false};
    return kItemRows;
}

class ItemsTab final : public StaticTab {
public:
    ItemsTab() : StaticTab(RowList{g_item_rows, 1}) {}
    void open() override {
        count_ = load_items(kernel::current_ctx());   // the count follows the bag
        StaticTab::open();
    }
    Shape shape(int i) const override {
        Shape s = StaticTab::shape(i);
        if (g_bag.stacks.empty()) s.use = Use::ReadOnly;   // the "Unavailable" row
        return s;
    }
    // The picked stack gets the new count, and the count row goes back to "Keep". Nothing of play-mode Settings changes, so the rows'
    // own (empty) setters are not run.
    void commit(Apply&) override {
        const int pick = cur[0], count = cur[1];
        cur[1] = 0;
        if (g_bag.stacks.empty() || count <= 0 || pick < 0 || pick >= stack_count()) return;
        ::Ctx* c = kernel::current_ctx();
        item_edit::Stack& s = g_bag.stacks[pick];
        if (!c || !item_edit::set_count(c, s, static_cast<uint32_t>(count))) return;
        s.count = static_cast<uint32_t>(count);
        g_labels[pick] = shown_name(s) + " x" + std::to_string(s.count);
    }
};

ItemsTab g_items_tab;

}  // namespace

TabModel& items_tab() { return g_items_tab; }

}  // namespace nxrt::games::pkp::options_detail
