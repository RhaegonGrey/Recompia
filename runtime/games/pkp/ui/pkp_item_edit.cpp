// SPDX-License-Identifier: GPL-3.0-or-later
// Item editor: reads the bag's stacks and sets a stack's count. [pkp] item_editor (a mod the player opts in to: it gives items for free).
// The Items tab of the game's Settings screen (pkp_options_items.cpp) is its only caller.
//
// What it relies on (main 2.0.1a; the slots are described in pkp_items.h and pkp_sort.cpp):
//  * Offline the bag is 120 slots at save common + layout::kBagOffline; every slot holds the slot vtable. A bag whose slots do not is not
//    read at all, so a save that is not loaded, or another layout, shows no stacks instead of a guess.
//  * Only the count byte of a slot that already holds an item is written, the same 8-byte store the inventory sort does when it merges
//    stacks. An empty slot is never filled: the packed word's bits above the id are per-slot data this does not know how to make.
//  * The count stays within 1 and the game's max stack (layout::kMaxStack), the range the game's own stacking keeps.
//  * While an item is held the bag screen works on a temp copy (layout::kItemTempFlag) that it copies back over the bag; the Settings
//    screen is not the bag screen, but the flag is checked all the same.
//  * Link Play: online the bag is another array and moves go through the game's network requests, so nothing is read or written while
//    layout::kOnline is true (as the inventory sort).
#include "ui/pkp_item_edit.h"

#include <algorithm>

#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";

using guest::call;
using guest::ok;
using guest::rd;
using guest::wr;

struct State {
    uint64_t base = 0;
    bool on = false;
} g;

// The bag's slots when it can be edited now, else 0 with the reason in *why.
uint64_t bag_slots(::Ctx* c, const char** why) {
    if (!g.on) return *why = "Switch on Item editor in the Gameplay tab and start the game again.", 0;
    if (call(c, g.base + layout::kOnline, 0) & 1) return *why = "Not during Link Play.", 0;
    const uint64_t save = rd<uint64_t>(g.base + layout::kSaveRoot);
    const uint64_t arr = save ? save + layout::kSaveCommon + layout::kBagOffline : 0;
    if (!arr || !ok(arr, layout::kBagSlots * items::kSlotSize)) return *why = "Load your save first.", 0;
    if (rd<uint8_t>(g.base + layout::kItemTempFlag)) return *why = "Put down the item you are holding first.", 0;
    for (uint32_t i = 0; i < layout::kBagSlots; ++i)
        if (rd<uint64_t>(arr + i * items::kSlotSize) != g.base + layout::kItemSlotVtable) return *why = "Load your save first.", 0;
    return arr;
}

uint32_t max_stack(::Ctx* c) { return std::min<uint32_t>(static_cast<uint32_t>(call(c, g.base + layout::kMaxStack, 0) & 0xffff), 0xff); }

void install(const FeatureInstall& in) {
    g.base = in.base;
    g.on = settings::get_bool(settings::Key::ItemEditor);
    if (g.on) NXLOG_I(kTag, "item editor: on (the Items tab of the Settings screen)");
}

void remove() { g = State{}; }

}  // namespace

namespace item_edit {

Bag read_bag(::Ctx* c) {
    Bag b;
    const uint64_t arr = bag_slots(c, &b.why);
    if (!arr) return b;
    b.max_stack = max_stack(c);
    if (!b.max_stack) return b.why = "Load your save first.", b;
    for (uint32_t i = 0; i < layout::kBagSlots; ++i) {
        const uint64_t p = rd<uint64_t>(arr + i * items::kSlotSize + items::kSlotPayload);
        if (!items::id_of(p) || !items::count_of(p)) continue;
        b.stacks.push_back({i, items::id_of(p), items::count_of(p), items::name(c, g.base, items::id_of(p))});
    }
    if (b.stacks.empty()) b.why = "Your bag is empty.";
    return b;
}

bool set_count(::Ctx* c, const Stack& s, uint32_t count) {
    const char* why = "";
    const uint64_t arr = bag_slots(c, &why);
    if (!arr || s.slot >= layout::kBagSlots || count < 1 || count > max_stack(c)) {
        NXLOG_W(kTag, "item editor: slot %u not changed (%s)", s.slot, arr ? "out of range" : why);
        return false;
    }
    const uint64_t at = arr + s.slot * items::kSlotSize + items::kSlotPayload;
    const uint64_t p = rd<uint64_t>(at);
    if (items::id_of(p) != s.id || !items::count_of(p)) {   // the bag changed since it was read
        NXLOG_W(kTag, "item editor: slot %u no longer holds item 0x%x, not changed", s.slot, s.id);
        return false;
    }
    wr<uint64_t>(at, items::with_count(p, count));
    NXLOG_I(kTag, "item editor: slot %u item 0x%x count %u -> %u", s.slot, s.id, items::count_of(p), count);
    return true;
}

}  // namespace item_edit

const Feature kItemEditFeature = {.name = "item_edit", .install = &install, .remove = &remove};

}  // namespace nxrt::games::pkp
