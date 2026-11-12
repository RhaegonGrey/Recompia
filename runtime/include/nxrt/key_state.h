// SPDX-License-Identifier: GPL-3.0-or-later
// Which keyboard keys are held, for the code that is not the platform's event pump: the mods host. The graphics layer (play mode) feeds it
// from the key events that reach the game and clears it when the window loses focus; keys are SDL scancodes. It also registers how a key
// is named (the names key settings store: "F2", "H", "Left Shift"). Any thread.
#pragma once

namespace nxrt::key_state {

constexpr int kMaxScancode = 512;   // SDL_SCANCODE_COUNT

void set(int scancode, bool down);
void clear();   // the window lost focus: nothing is held
bool down(int scancode);

// Scancode of a key's name, or -1 for a name that is not a keyboard key (play registers its own parser, case-insensitive).
using NameFn = int (*)(const char* name);
void set_name_lookup(NameFn fn);
bool down_by_name(const char* name);   // false for an unknown name

}  // namespace nxrt::key_state
