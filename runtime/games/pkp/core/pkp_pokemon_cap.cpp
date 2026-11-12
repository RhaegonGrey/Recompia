// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia v2.0.1a resident limit and active-appearance budget (gameplay mod pokemon_cap_50; the registry installs it only while on).
// Ported from the Eglantine cap's default (non-staged) behavior. The game still has 64 actor slots, which is also the configured resident
// maximum.
#include "core/pkp_pokemon_cap.h"
#include "core/pkp_resident_census.h"

#include <algorithm>
#include <cstdint>

#include "core/pkp_guest.h"
#include "core/pkp_settings.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {

constexpr std::uint64_t kPokemonParamsRoot = 0x36ca678;  // global in main: -> holder; holder+0x20 -> resident parameter object
constexpr std::uint64_t kParamsLink = 0x20;
constexpr std::uint64_t kLivingOffset = 0x4bc;
constexpr std::uint64_t kObjectsOffset = 0x4c0;
constexpr std::uint64_t kActiveField = 0;    // [holder+0]: active budget block
constexpr std::uint64_t kActiveBudget = 0xc; // u32, original 30
constexpr std::uint32_t kOriginalLiving = 25;
constexpr std::uint32_t kOriginalObjects = 30;
constexpr std::uint32_t kPoolSize = 64;
constexpr std::uint32_t kMaximumLiving = 64;
constexpr std::uint32_t kVisitorHeadroom = 14; // the remaining slots with 50 residents

struct State {
    std::uint64_t base = 0;
    std::uint64_t params = 0;
    std::uint64_t active_block = 0;
    std::uint32_t requested = 31;
    bool limits_reported = false;
    bool active_reported = false;
    bool warned_layout = false;
    bool warned_active = false;
};
State g;

void game_frame(::Ctx*) {
    if (!g.base) return;
    resident_census::tick();

    // Pointers are guest-mapped addresses, not native process offsets.
    const auto global = g.base + kPokemonParamsRoot;
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
    // Never interpret an unknown layout or not-yet-initialized parameter as a cap.
    if (living < kOriginalLiving || living > kMaximumLiving ||
        objects < kOriginalObjects || objects > kPoolSize) {
        if ((living || objects) && !g.warned_layout) {
            NXLOG_W("pkp", "pokemon cap: unexpected limits %u living / %u objects; untouched",
                    living, objects);
            g.warned_layout = true;
        }
        return;
    }

    // Eglantine's parameter forcing with object headroom bounded by the pool:
    // never lower a previously admitted population during the same session.
    const auto target_living = std::max(g.requested, living);
    const auto desired_objects = std::max(objects, target_living + kVisitorHeadroom);
    const auto target_objects = std::min(kPoolSize, desired_objects);
    if (living != target_living)
        guest::wr<std::uint32_t>(params + kLivingOffset, target_living);
    if (objects != target_objects)
        guest::wr<std::uint32_t>(params + kObjectsOffset, target_objects);
    if (!g.limits_reported) {
        NXLOG_I("pkp", "pokemon cap: applied %u living / %u objects (previous %u/%u; pool capacity 64)",
                target_living, target_objects, living, objects);
        g.limits_reported = true;
    }

    // The separate active-appearance budget: the original 30-budget keeper parks excess actors out of the scene.
    const auto block = guest::rd<std::uint64_t>(holder + kActiveField);
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
    const auto target_active = std::max(active, target_objects);
    if (active != target_active)
        guest::wr<std::uint32_t>(block + kActiveBudget, target_active);
    if (!g.active_reported) {
        NXLOG_I("pkp", "pokemon cap: active budget %u -> %u (pool capacity 64)",
                active, target_active);
        g.active_reported = true;
    }
}

void install(const FeatureInstall& in) {
    g = {};
    g.base = in.base;
    g.requested = static_cast<std::uint32_t>(settings::get_int(settings::Key::PokemonCapLimit));
    resident_census::start(in.base);
    NXLOG_I("pkp", "pokemon cap: requested %u living (physical actor pool: 64)", g.requested);
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
