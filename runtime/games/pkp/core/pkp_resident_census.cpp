// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Pokopia v2.0.1a census. Candidate actor links are not verified GPU/model handles.
#include "core/pkp_resident_census.h"

#include <array>
#include <cmath>
#include <cstdint>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::resident_census {
namespace {
constexpr std::uint64_t kPoolGlobal = 0x3483bd0;
constexpr std::uint64_t kWorldGlobal = 0x3483b60;
constexpr std::uint64_t kResidentsGlobal = layout::kPlayerManager;
constexpr std::uint64_t kSlotsOffset = 0x50;
constexpr std::uint64_t kFreeOffset = 0x6608;
constexpr std::uint64_t kResidentDataOffset = 0x2a0;
constexpr std::uint64_t kResidentCountOffset = 0x2ac;
constexpr std::uint64_t kResidentRow = 0x382c;
constexpr std::uint64_t kKind = 0x39f0;
constexpr std::uint64_t kFlags = 0x245e;
constexpr std::uint64_t kLinkA = 0x3a50;
constexpr std::uint64_t kLinkB = 0x18a8;
constexpr std::uint64_t kWorldTransforms = 0x835af0;
constexpr std::uint32_t kSlots = 64;
constexpr std::uint32_t kMaxResidentRow = 1050;
constexpr std::uint32_t kSampleFrames = 120;
constexpr std::uint32_t kDetailEverySamples = 15;

struct State {
    std::uint64_t base = 0;
    std::uint64_t pool = 0;
    std::uint64_t world = 0;
    std::uint64_t frames = 0;
    std::uint32_t occupied = 0;
    std::uint32_t samples = 0;
    std::uint32_t failed_samples = 0;
    bool valid = false;
};
State g;

std::uint64_t root(std::uint64_t off, const char** error = nullptr) {
    const auto addr = g.base + off;
    if (!guest::ok(addr, sizeof(std::uint64_t))) {
        if (error) *error = "root pointer cell is not mapped";
        return 0;
    }
    const auto holder = guest::rd<std::uint64_t>(addr);
    if (!guest::ok(holder, sizeof(std::uint64_t))) {
        if (error) *error = "root holder pointer is null or unmapped";
        return 0;
    }
    const auto object = guest::rd<std::uint64_t>(holder);
    if (!guest::ok(object, sizeof(std::uint64_t))) {
        if (error) *error = "root object pointer is null or unmapped";
        return 0;
    }
    return object;
}

void unavailable(const char* reason) {
    ++g.failed_samples;
    if (g.failed_samples == 1 || g.failed_samples % kDetailEverySamples == 0)
        NXLOG_W("pkp", "pokemon census: unavailable (%s), attempts=%u; checking again",
                reason, g.failed_samples);
}

std::uint32_t resident_count() {
    const auto addr = g.base + kResidentsGlobal;
    if (!guest::ok(addr, sizeof(std::uint64_t))) return 0;
    const auto p = guest::rd<std::uint64_t>(addr);
    if (!guest::ok(p + kResidentCountOffset, sizeof(std::uint32_t)) ||
        !guest::ok(p + kResidentDataOffset, sizeof(std::uint64_t))) return 0;
    const auto data = guest::rd<std::uint64_t>(p + kResidentDataOffset);
    if (!data) return 0;
    const auto n = guest::rd<std::uint32_t>(p + kResidentCountOffset);
    return n <= 10000 ? n : 0;
}

struct Slot {
    std::uint32_t slot = 0;
    std::uint32_t row = 0;
    std::uint16_t kind = 0;
    std::uint32_t flags = 0;
    bool valid = false;
    bool positioned = false;
    bool offmap = false;
    float x = 0, y = 0, z = 0;
    std::uint64_t link_a = 0, link_b = 0;
};

Slot read_slot(std::uint32_t index, std::uint64_t actor, std::uint64_t world) {
    Slot s;
    s.slot = index;
    if (!guest::ok(actor + kResidentRow, sizeof(std::uint32_t)) ||
        !guest::ok(actor + kKind, sizeof(std::uint16_t))) return s;
    s.valid = true;
    s.row = guest::rd<std::uint32_t>(actor + kResidentRow);
    s.kind = guest::rd<std::uint16_t>(actor + kKind);
    if (guest::ok(actor + kFlags, sizeof(std::uint32_t)))
        s.flags = guest::rd<std::uint32_t>(actor + kFlags);
    if (guest::ok(actor + kLinkA, sizeof(std::uint64_t)))
        s.link_a = guest::rd<std::uint64_t>(actor + kLinkA);
    if (guest::ok(actor + kLinkB, sizeof(std::uint64_t)))
        s.link_b = guest::rd<std::uint64_t>(actor + kLinkB);
    if (world && s.row < kMaxResidentRow) {
        const auto pos = world + kWorldTransforms + std::uint64_t{s.row} * 0x30 + 0x10;
        if (guest::ok(pos, sizeof(float) * 3)) {
            s.x = guest::rd<float>(pos);
            s.y = guest::rd<float>(pos + 4);
            s.z = guest::rd<float>(pos + 8);
            s.positioned = std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z);
            if (s.positioned)
                s.offmap = std::fabs(s.x - 96850.f) < 1.f &&
                           std::fabs(s.y - 100.f) < 1.f &&
                           std::fabs(s.z - 96850.f) < 1.f;
        }
    }
    return s;
}

bool crossed(std::uint32_t before, std::uint32_t after, std::uint32_t n) {
    return before < n && after >= n;
}
} // namespace

void start(std::uint64_t main_base) {
    g = {};
    g.base = main_base;
    NXLOG_I("pkp", "pokemon census: installed (read-only, samples every 120 game frames)");
}
void stop() { g = {}; }

void tick() {
    if (!g.base || ++g.frames % kSampleFrames) return;
    const char* root_error = "unknown actor pool error";
    const auto pool = root(kPoolGlobal, &root_error);
    if (!pool) {
        unavailable(root_error);
        return;
    }
    if (!guest::ok(pool + kSlotsOffset, kSlots * sizeof(std::uint64_t))) {
        unavailable("pool slot array is not mapped");
        return;
    }
    if (!guest::ok(pool + kFreeOffset, sizeof(std::uint32_t))) {
        unavailable("pool free-slot counter is not mapped");
        return;
    }
    const auto free = guest::rd<std::uint32_t>(pool + kFreeOffset);
    if (free > kSlots) {
        unavailable("pool free-slot counter exceeds 64 (possibly wrong pointer chain)");
        return;
    }
    if (g.failed_samples)
        NXLOG_I("pkp", "pokemon census: actor pool available again after %u unsuccessful samples", g.failed_samples);
    g.failed_samples = 0;
    const auto world = root(kWorldGlobal);
    std::array<Slot, kSlots> details{};
    std::uint32_t occupied = 0, offmap = 0, positioned = 0, invalid = 0, missing_links = 0;
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        const auto actor = guest::rd<std::uint64_t>(pool + kSlotsOffset + std::uint64_t{i} * 8);
        if (!actor) continue;
        ++occupied;
        details[i] = read_slot(i, actor, world);
        const Slot& s = details[i];
        if (!s.valid) { ++invalid; continue; }
        if (!s.link_a || !s.link_b) ++missing_links;
        if (s.positioned) ++positioned;
        if (s.offmap) ++offmap;
    }
    const bool root_changed = !g.valid || pool != g.pool || world != g.world;
    const bool count_changed = !g.valid || occupied != g.occupied;
    const bool periodic = (++g.samples % kDetailEverySamples) == 0;
    if (root_changed || count_changed || periodic) {
        NXLOG_I("pkp", "pokemon census: actors=%u/64 free=%u resident_records=%u positioned=%u offmap=%u invalid=%u missing_candidate_links=%u pool=%llx world=%llx",
                occupied, free, resident_count(), positioned, offmap, invalid, missing_links,
                static_cast<unsigned long long>(pool), static_cast<unsigned long long>(world));
        if (occupied + free != kSlots)
            NXLOG_W("pkp", "pokemon census: pool counters disagree (actors=%u free=%u capacity=64)", occupied, free);
    }
    // Rows only when something changed: 64 lines every 30 s would be log writes on the game thread for nothing.
    const bool details_needed = root_changed ||
        (count_changed && (crossed(g.occupied, occupied, 25) || crossed(g.occupied, occupied, 32) ||
                           crossed(g.occupied, occupied, 33) || crossed(g.occupied, occupied, 40) ||
                           crossed(g.occupied, occupied, 50)));
    if (details_needed && occupied) {
        for (const Slot& s : details) {
            if (!s.valid) continue;
            NXLOG_I("pkp", "pokemon census row: slot=%u resident=%u kind=%u flags=%08x pos_known=%u offmap=%u xyz=%.1f,%.1f,%.1f linkA=%llx linkB=%llx",
                    s.slot, s.row, static_cast<unsigned>(s.kind), s.flags,
                    s.positioned ? 1u : 0u, s.offmap ? 1u : 0u, s.x, s.y, s.z,
                    static_cast<unsigned long long>(s.link_a), static_cast<unsigned long long>(s.link_b));
        }
    }
    g.pool = pool;
    g.world = world;
    g.occupied = occupied;
    g.valid = true;
}

} // namespace nxrt::games::pkp::resident_census
