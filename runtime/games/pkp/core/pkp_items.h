// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: an item slot's fields and an item's name, for every hook file that reads items (the inventory sort, the crafting
// search, the item editor). The addresses are in pkp_layout.h; how the slots were measured is in pkp_sort.cpp.
//
// A slot is 16 bytes: a vtable qword (main + layout::kItemSlotVtable), a packed u32 (bits 0..12 the item id, 0 = empty; the bits above are
// per-slot data) and a u8 count at +0xc. Only the 8 bytes from +8 (the payload) ever move; every slot keeps its vtable.
#pragma once
#include <cstdint>
#include <string>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

namespace nxrt::games::pkp::items {

constexpr uint64_t kSlotSize = 16, kSlotPayload = 8;
constexpr uint32_t kIdMask = 0x1fff;
constexpr uint64_t kCountMask = uint64_t{0xff} << 32;

// The payload's fields.
constexpr uint32_t id_of(uint64_t p) { return static_cast<uint32_t>(p) & kIdMask; }
constexpr uint32_t count_of(uint64_t p) { return static_cast<uint32_t>((p >> 32) & 0xff); }
constexpr uint64_t with_count(uint64_t p, uint32_t n) { return (p & ~kCountMask) | (uint64_t{n} << 32); }

// An item's record in the item table (the table at +0 while the manager does not hold it yet, pkp_tables.h); 0 if none.
inline uint64_t record(::Ctx* c, uint64_t base, uint32_t id) {
    const uint64_t row = tables::row_or_first(c, base, layout::kItemTable, id);
    return row && guest::ok(row + 8, 8) ? guest::rd<uint64_t>(row + 8) : 0;
}

// An item's singular name in the game's language (UTF-8, lower case, may hold markup tags); empty when the item has none.
inline std::string name(::Ctx* c, uint64_t base, uint32_t id) {
    std::string out;
    const uint64_t rec = id ? record(c, base, id) : 0;
    const uint16_t nid = rec && guest::ok(rec + layout::kItemNameId, 2) ? guest::rd<uint16_t>(rec + layout::kItemNameId) : 0;
    if (!nid || nid > layout::kNameIdMax) return out;
    const uint64_t p = guest::call(c, base + layout::kNameOf, nid, 0);
    for (uint64_t a = p; p && out.size() < 200 && guest::ok(a, 1); ++a) {
        const char ch = guest::rd<char>(a);
        if (!ch) break;
        out += ch;
    }
    return out;
}

}  // namespace nxrt::games::pkp::items
