// SPDX-License-Identifier: GPL-3.0-or-later
// Item editor (pkp_item_edit.cpp): the count of a stack in the bag, changed from the Items tab of the game's Settings screen
// (pkp_options_items.cpp). [pkp] item_editor, off by default.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kItemEditFeature;   // no hooks: the Items tab calls item_edit below

namespace item_edit {

struct Stack {
    uint32_t slot;      // its bag slot
    uint32_t id;        // the item
    uint32_t count;
    std::string name;   // in the game's language; empty when the item has none
};

struct Bag {
    std::vector<Stack> stacks;   // the bag's stacks in slot order; empty when the bag cannot be edited now
    uint32_t max_stack = 0;      // the game's max stack (at most 255: a slot's count is a byte)
    const char* why = "";        // stacks is empty: the reason, for the player
};

// The bag as it is now. The game thread.
Bag read_bag(::Ctx* c);

// Sets the stack's count (1..max stack). False, and nothing written, when the bag cannot be edited now or the slot no longer holds that
// item. The game thread.
bool set_count(::Ctx* c, const Stack& s, uint32_t count);

}  // namespace item_edit
}  // namespace nxrt::games::pkp
