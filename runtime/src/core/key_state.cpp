// SPDX-License-Identifier: GPL-3.0-or-later
// Held keyboard keys (nxrt/key_state.h): one bit per scancode.
#include "nxrt/key_state.h"

#include <atomic>
#include <cstdint>

namespace nxrt::key_state {

namespace {
constexpr int kWords = kMaxScancode / 64;
std::atomic<uint64_t> g_bits[kWords];
std::atomic<NameFn> g_name{nullptr};
bool in_range(int sc) { return sc >= 0 && sc < kMaxScancode; }
}  // namespace

void set(int sc, bool down) {
    if (!in_range(sc)) return;
    const uint64_t bit = uint64_t{1} << (sc & 63);
    if (down) g_bits[sc >> 6].fetch_or(bit, std::memory_order_relaxed);
    else g_bits[sc >> 6].fetch_and(~bit, std::memory_order_relaxed);
}

void clear() {
    for (auto& w : g_bits) w.store(0, std::memory_order_relaxed);
}

bool down(int sc) { return in_range(sc) && (g_bits[sc >> 6].load(std::memory_order_relaxed) >> (sc & 63) & 1); }

void set_name_lookup(NameFn fn) { g_name.store(fn, std::memory_order_relaxed); }

bool down_by_name(const char* name) {
    const NameFn fn = g_name.load(std::memory_order_relaxed);
    return name && fn && down(fn(name));
}

}  // namespace nxrt::key_state
