// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Native-recomp port of Eglantine's Pokopia resident/object/active caps.
// Keep the legacy [pkp] pokemon_cap_50 toggle for launcher compatibility.
// The experimental staged-fill mode was not ported: the supplied implementation
// reports that no extra Pokemon appear when attempting to fill during play.
#include "core/pkp_pokemon_cap.h"
#include "core/pkp_resident_census.h"

#include <algorithm>
#include <cstdint>

#include "core/pkp_guest.h"
#include "core/pkp_settings.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {

constexpr std::uint64_t kParamsRoot = 0x36ca678;
constexpr std::uint64_t kParamsLink = 0x20;
constexpr std::uint64_t kLivingOffset = 0x4bc;
constexpr std::uint64_t kObjectsOffset = 0x4c0;
constexpr std::uint64_t kActiveLink = 0;
constexpr std::uint64_t kActiveBudget = 0xc;
constexpr std::uint32_t kNativeLiving = 25;
constexpr std::uint32_t kNativeObjects = 30;
constexpr std::uint32_t kPoolSize = 64;
constexpr std::uint32_t kMaxResidentCap = 63;
constexpr std::uint32_t kVisitorHeadroom = 14;
constexpr std::uint32_t kDefaultCap = 30;

// All three caps matter. Never reduce limits once Pokemon are admitted:
// lowering the living cap can permanently send existing residents away.
struct Limits {
    std::uint32_t living;
    std::uint32_t objects;
    std::uint32_t active;
};
constexpr Limits plan(std::uint32_t living, std::uint32_t objects,
                      std::uint32_t active, std::uint32_t requested) {
    const auto residents = std::max(living, std::clamp(requested, kNativeLiving, kMaxResidentCap));
    // Fifty residents leave fourteen of the fixed sixty-four slots for visitors;
    // higher values allow less room. Twenty-five keeps the game's own limits.
    const auto with_visitors = residents == kNativeLiving
        ? kNativeObjects : std::min(kPoolSize, residents + kVisitorHeadroom);
    const auto all_objects = std::max(objects, std::max(residents, with_visitors));
    return {residents, all_objects, std::max(active, all_objects)};
}
static_assert(plan(25, 30, 30, 25).living == 25 && plan(25, 30, 30, 25).objects == 30);
static_assert(plan(25, 30, 30, 30).living == 30 && plan(25, 30, 30, 30).objects == 44);
static_assert(plan(25, 30, 30, 50).objects == 64 && plan(25, 30, 30, 50).active == 64);
static_assert(plan(25, 30, 30, 63).living == 63 && plan(25, 30, 30, 63).objects == 64);
static_assert(plan(60, 64, 64, 30).living == 60);
static_assert(plan(64, 64, 64, 63).living == 64);

struct State {
    std::uint64_t base = 0;
    std::uint64_t params = 0;
    std::uint64_t active_block = 0;
    std::uint32_t requested = kDefaultCap;
    bool limits_reported = false;
    bool active_reported = false;
    bool warned_layout = false;
    bool warned_active = false;
};
State g;

void game_frame(::Ctx*) {
    if (!g.base) return;
    // Reuse the existing read-only 64-slot pool census and resident diagnostics.
    resident_census::tick();

    const auto global = g.base + kParamsRoot;
    if (!guest::ok(global, sizeof(std::uint64_t))) return;
    const auto holder = guest::rd<std::uint64_t>(global);
    if (!guest::ok(holder, kParamsLink + sizeof(std::uint64_t))) return;
    const auto params = guest::rd<std::uint64_t>(holder + kParamsLink);
    if (!guest::ok(params, kObjectsOffset + sizeof(std::uint32_t))) return;
    if (params != g.params) {
        g.params = params;
        g.limits_reported = false;
        g.warned_layout = false;
    }

    const auto living = guest::rd<std::uint32_t>(params + kLivingOffset);
    const auto objects = guest::rd<std::uint32_t>(params + kObjectsOffset);
    // Game-specific v2.0.1a offsets. Uninitialized/unrecognized layouts are untouched.
    if (living < kNativeLiving || living > kPoolSize ||
        objects < kNativeObjects || objects > kPoolSize || objects < living) {
        if ((living || objects) && !g.warned_layout) {
            NXLOG_W("pkp", "pokemon cap: unexpected limits %u living / %u objects; untouched",
                    living, objects);
            g.warned_layout = true;
        }
        return;
    }

    const auto target = plan(living, objects, 0, g.requested);
    if (living != target.living)
        guest::wr<std::uint32_t>(params + kLivingOffset, target.living);
    if (objects != target.objects)
        guest::wr<std::uint32_t>(params + kObjectsOffset, target.objects);
    if (!g.limits_reported) {
        NXLOG_I("pkp", "pokemon cap: %u residents / %u objects (was %u/%u; pool 64)",
                target.living, target.objects, living, objects);
        g.limits_reported = true;
    }

    // The independent active-appearance keeper originally holds only 30 actors.
    // Without raising it, distant residents are parked offscreen.
    const auto block = guest::rd<std::uint64_t>(holder + kActiveLink);
    if (block != g.active_block) {
        g.active_block = block;
        g.active_reported = false;
        g.warned_active = false;
    }
    if (!guest::ok(block, kActiveBudget + sizeof(std::uint32_t))) return;
    const auto active = guest::rd<std::uint32_t>(block + kActiveBudget);
    if (active == 0 || active > kPoolSize) {
        if (active && !g.warned_active) {
            NXLOG_W("pkp", "pokemon cap: unexpected active budget %u; untouched", active);
            g.warned_active = true;
        }
        return;
    }
    const auto wanted_active = std::max(active, target.active);
    if (active != wanted_active)
        guest::wr<std::uint32_t>(block + kActiveBudget, wanted_active);
    if (!g.active_reported) {
        NXLOG_I("pkp", "pokemon cap: active budget %u -> %u", active, wanted_active);
        g.active_reported = true;
    }
}

void install(const FeatureInstall& in) {
    g = {};
    g.base = in.base;
    g.requested = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(settings::get_int(settings::Key::PokemonCapLimit),
                                 kNativeLiving, kMaxResidentCap));
    resident_census::start(in.base);
    NXLOG_I("pkp", "pokemon cap: requested %u (native 25; pool 64)", g.requested);
    if (g.requested > 50)
        NXLOG_W("pkp", "pokemon cap: %u residents leave at most %u slots for visitors",
                g.requested, kPoolSize - g.requested);
}

void remove() {
    resident_census::stop();
    g = {};
}

} // namespace

const Feature kPokemonCapFeature = {
    .name = "pokemon_cap",
    .install = &install,
    .remove = &remove,
    .game_frame = &game_frame,
};

} // namespace nxrt::games::pkp
