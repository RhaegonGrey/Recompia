// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia (main 2.0.1a): game globals, struct offsets and functions that more than one hook file uses. Main-image values are
// offsets from main's base; struct values are offsets into the object named in the group. A value one file alone uses stays in that file.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp::layout {

// ---- globals in main (qword pointers) ----
constexpr uint64_t kPlayerManager = 0x3561928;   // the player manager (26344 bytes, made in 0x71000ae380)
constexpr uint64_t kManagerPlayer = 0x30;        // player manager: the local player's character (+0x38 the second one)
constexpr uint64_t kSaveRoot = 0x3561930;        // the save data (the save/options block)
constexpr uint64_t kInputMgr = 0x3561718;        // the input manager
constexpr uint64_t kTables = 0x35618f0;          // the data-table manager (the sprite sheet lookups read it too)
constexpr uint64_t kUiMgr = 0x35aebf8;           // the UI manager (UI resource manager: holds the loaded sprite sheets and the layout pools)
constexpr uint64_t kPokemonParamsRoot = 0x36ca678;  // -> holder -> (+0x20) resident parameter object
constexpr uint64_t kLanguage = 0x36ca680;        // -> u32 language 0..8
constexpr uint64_t kWorld = 0x34efc98;           // the world ([GOT 0x3483b10] points here); +0x6000030 = chunk map u16[64*64], +0x6000018 = packed cells

// ---- the data-table manager: table i at +0x10 + its offset (pkp_tables.h looks a row up) ----
// u32: how many tables it holds (8-byte slots from +0x10; a table at off is there while this is above off / 8, pkp_tables.h).
constexpr uint64_t kTableCount = 0x2b70;
constexpr uint64_t kItemTable = 0x638;
constexpr uint64_t kItemNameId = 0x22;           // item record: u16 name id (1..kNameIdMax, for kNameOf)
constexpr uint16_t kNameIdMax = 3000;

// ---- the input manager ----
// Its vector at +0xcd8 (end at +0xce0) maps an action to its button (u32 per action, low byte: 0..3 the d-pad, 4 Y, 5 A, 6 X, 7 B, 8 (-),
// 9 (+), 0xa L, 0xb R, 0xc ZL, 0xd ZR, 0xe / 0xf the stick clicks; 0x7101126e18 reads it for the key guide icons).
constexpr uint64_t kActionButtons = 0xcd8;

// ---- character (a player's) ----
constexpr uint64_t kPosition = 0x20c0;           // world position (vec4; 3 floats, Y up; vtable slot 2 0x71002e40d8 returns it)
constexpr uint64_t kHeading = 0x2404;            // heading (radians, atan2(x, z)), a copy of the movement controller's (vtable slot 3 0x71002e4478)
// The local player's class (vtable main+0x322fda8) overrides both getters (0x71002e4084, 0x71002e4420): while its mode word at +0x5a34 is 0
// and the byte at +0x5a39 is 0 (vtable slots 504/512), the position and heading come from the save data instead (+0x7419f0, +0x741a10).
constexpr uint64_t kPlayerMode = 0x5a34, kPlayerModeByte = 0x5a39;
constexpr uint64_t kMouseModeOn = 0xaa81;        // the game's Joy-Con 2 mouse mode is on (u8)
constexpr uint64_t kMouseHitValid = 0xaa82;      // the pointer's hit is valid (u8)
constexpr uint64_t kMouseSwingPos = 0xaa50, kMouseSwingValid = 0xaa84;   // the swing tools' position under the pointer (vec4), its flag

// ---- save data ----
constexpr uint64_t kSaveHeading = 0x741a10;      // the local player's heading while kPlayerMode and kPlayerModeByte are 0
// The items (pkp_items.h has a slot's fields, pkp_sort.cpp how they were measured).
constexpr uint64_t kSaveCommon = 0x13d4920;      // the common block
constexpr uint64_t kBagOffline = 0x830;          // common block: the bag's slots while offline (m_useItemInventoryOffline)
constexpr uint32_t kBagSlots = 120;

// ---- items in main ----
constexpr uint64_t kItemSlotVtable = 0x322c1d8;  // the vtable every item slot holds
constexpr uint64_t kItemTempFlag = 0x36bbfd8;    // u8 per player: the bag / container screen works on a temp copy (an item is held)

// ---- movement component ----
constexpr uint64_t kController = 0xbc0;          // the movement controller (embedded)

// ---- the main loop ----
// mid: the frame step (0xbfc404) where its frame multiplier is joined (s0 the multiplier, s8 the frame time): the game thread, once a
// frame. The feature registry's game-frame tick (pkp_game.cpp) and the frame-rate hook (pkp_frame_rate.cpp) sit here, in that order.
constexpr uint64_t kFrameStep = 0xbfc4fc;

// ---- game functions ----
constexpr uint64_t kOnline = 0x376c4;            // () -> bit 0: Link Play session
constexpr uint64_t kMaxStack = 0x2a7f0c;         // () -> the max stack (u16), one value for every item
constexpr uint64_t kNameOf = 0xa34e1c;           // (name id, form) -> UTF-8 name in the game's language
constexpr uint64_t kLayoutOf = 0x9132fc;         // (source) -> layout
constexpr uint64_t kChildOf = 0xa6f92c;          // (children (screen + 0x48), element id, u16* class) -> child widget
constexpr uint64_t kTextString = 0xf35f20;       // (widget, text slot, text id, const char*): a text id with a string argument
constexpr uint32_t kStringText = 0xa8;           // text id "<string;0(lstr1)>"
// The key guide widget's functions, offsets and tables are in pkp_key_guide.cpp, its one user.

}  // namespace nxrt::games::pkp::layout
