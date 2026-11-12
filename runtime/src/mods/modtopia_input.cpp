// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host's pads (MtBackend pad_read, pad_hold, pad_block): the hid service's input filter (nxrt/input_source.h) records each player's
// own input and then holds back what the mods block and adds what they hold, before the game's sample. MT_PAD_* bits are NpadButton bits
// 0..15, and sticks are in the same -32767..32767 range, so a pad passes through unchanged.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "mods_internal.h"
#include "nxrt/input_source.h"

namespace nxrt::mods::detail {

namespace {

constexpr int kPlayers = 8;   // MtBackend: p 0..7
constexpr uint64_t kPadBits = 0xffff;
constexpr uint64_t kLeftDirBits = hid::btn(hid::kBtnStickLLeft) | hid::btn(hid::kBtnStickLUp) | hid::btn(hid::kBtnStickLRight) |
                                  hid::btn(hid::kBtnStickLDown);
constexpr uint64_t kRightDirBits = hid::btn(hid::kBtnStickRLeft) | hid::btn(hid::kBtnStickRUp) | hid::btn(hid::kBtnStickRRight) |
                                   hid::btn(hid::kBtnStickRDown);

struct Pads {
    std::mutex mu;
    MtPad read[kPlayers] = {};    // each player's own input in the last sample, before the mods' block and hold
    MtPad hold[kPlayers] = {};    // what the mods add (the core has combined every mod's)
    MtPad block[kPlayers] = {};   // what the mods hold back
    bool installed = false;
};
Pads& pads() {
    static Pads p;
    return p;
}

int16_t clamp16(int32_t v) { return static_cast<int16_t>(std::clamp(v, -32767, 32767)); }

// Player p's npad slots: its own, and for player 1 also the handheld pad (handheld mode).
template <class F> void each_slot_of(hid::InputSnapshot& snap, int p, F&& f) {
    f(snap.npad[p]);
    if (p == 0) f(snap.npad[hid::kNpadSlotHandheld]);
}

void filter(hid::InputSnapshot& snap, void*) {
    Pads& s = pads();
    std::lock_guard<std::mutex> lk(s.mu);
    for (int p = 0; p < kPlayers; ++p) {
        MtPad own{};
        bool connected = false;
        each_slot_of(snap, p, [&](const hid::NpadInput& n) {   // every device of the player combined: buttons ORed, the stick moved most
            if (!n.connected) return;
            connected = true;
            own.buttons |= static_cast<uint32_t>(n.buttons & kPadBits);
            if (std::abs(n.left.x) + std::abs(n.left.y) > std::abs(own.lx) + std::abs(own.ly)) own.lx = clamp16(n.left.x), own.ly = clamp16(n.left.y);
            if (std::abs(n.right.x) + std::abs(n.right.y) > std::abs(own.rx) + std::abs(own.ry)) own.rx = clamp16(n.right.x), own.ry = clamp16(n.right.y);
        });
        s.read[p] = own;
        if (!connected) continue;
        const MtPad& b = s.block[p];
        const MtPad& h = s.hold[p];
        if (!b.buttons && !b.lx && !b.ly && !b.rx && !b.ry && !h.buttons && !h.lx && !h.ly && !h.rx && !h.ry) continue;
        bool first = true;   // the hold goes to one connected slot of the player, the block to all of them
        each_slot_of(snap, p, [&](hid::NpadInput& n) {
            if (!n.connected) return;
            n.buttons &= ~(b.buttons & kPadBits);
            if (b.lx || b.ly) n.left = {}, n.buttons &= ~kLeftDirBits;
            if (b.rx || b.ry) n.right = {}, n.buttons &= ~kRightDirBits;
            if (!first) return;
            first = false;
            n.buttons |= h.buttons & kPadBits;
            if (h.lx || h.ly) n.left = {clamp16(n.left.x + h.lx), clamp16(n.left.y + h.ly)}, n.buttons &= ~kLeftDirBits;
            if (h.rx || h.ry) n.right = {clamp16(n.right.x + h.rx), clamp16(n.right.y + h.ry)}, n.buttons &= ~kRightDirBits;
        });
    }
}

}  // namespace

void install_input() {
    Pads& s = pads();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.installed) return;
        s.installed = true;
    }
    hid::set_input_filter(&filter, nullptr);
}

int pad_read(int p, MtPad* out) {
    if (p < 0 || p >= kPlayers || !out) return MT_ERR_UNKNOWN;
    Pads& s = pads();
    std::lock_guard<std::mutex> lk(s.mu);
    *out = s.read[p];
    return MT_OK;
}

int pad_hold(int p, const MtPad* add) {
    if (p < 0 || p >= kPlayers) return MT_ERR_UNKNOWN;
    Pads& s = pads();
    std::lock_guard<std::mutex> lk(s.mu);
    s.hold[p] = add ? *add : MtPad{};
    return MT_OK;
}

int pad_block(int p, const MtPad* mask) {
    if (p < 0 || p >= kPlayers) return MT_ERR_UNKNOWN;
    Pads& s = pads();
    std::lock_guard<std::mutex> lk(s.mu);
    s.block[p] = mask ? *mask : MtPad{};
    return MT_OK;
}

}  // namespace nxrt::mods::detail
