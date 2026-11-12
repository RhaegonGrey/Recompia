// SPDX-License-Identifier: GPL-3.0-or-later
// Whether the game's 2D (HUD, markers, menus, text) is drawn over its 3D picture. Set by code that has no renderer to call (the mods host:
// Modtopia's ui_visible) and read by the NVN replay at each draw (nvn_replay_ui.cpp). While hidden, the replay leaves out every draw whose
// vertex shader has vertex inputs into a colour target 0 that shares its image with a texture the game presents; full-screen draws without
// vertex inputs (the composite of the 3D picture, fades, a blurred menu background) stay. Only a game whose game::Render::ui_after_picture
// says its 2D is drawn that way supports it. Any thread.
#pragma once

namespace nxrt::ui_layer {

bool supported();          // the game's rendering lets its 2D be told from its picture (game::Render::ui_after_picture)
void set_visible(bool v);  // false: hide the game's 2D from the next draw on; ignored when !supported()
bool hidden();             // one relaxed atomic load: the replay asks it at every draw

}  // namespace nxrt::ui_layer
