// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the game's side of the Modtopia mods host (nxrt/mods.h). Ticks the mods once a game frame on the game thread; the host
// itself was started by pkp_main before the game's first instruction, and Link Play reads its list in pkp_linkplay.cpp.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {
extern const Feature kModsFeature;
}
