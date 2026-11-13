// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia: Eglantine-derived native resident, object, and active-appearance caps.
// The 64-actor pool remains fixed; Settings > Gameplay offers 25..63 residents
// (default 30). The legacy pokemon_cap_50 setting name remains supported.
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {
extern const Feature kPokemonCapFeature;
}
