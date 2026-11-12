// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the mods host's frame tick (pkp_mods.h). The registry's game-frame tick (layout::kFrameStep) runs once a frame on the
// game's thread, which is where Modtopia posts FrameStart and reads the mappings' [core] area (AreaEntered, SaveLoaded).
#include "core/pkp_mods.h"

#include "nxrt/mods.h"

namespace nxrt::games::pkp {

namespace {

void game_frame(::Ctx*) { mods::frame(); }

}  // namespace

const Feature kModsFeature = {
    .name = "mods",
    .game_frame = &game_frame,
};

}  // namespace nxrt::games::pkp
