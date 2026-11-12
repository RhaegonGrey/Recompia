// SPDX-License-Identifier: GPL-3.0-or-later
// The final picture's colour matrix (nxrt/color_matrix.h): the latest one set.
#include "nxrt/color_matrix.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace nxrt::color_matrix {

namespace {
std::mutex g_mu;
float g_m[12] = {};
std::atomic<bool> g_on{false};   // the present path asks every frame: no lock while none is set
}  // namespace

void set(const float* m12) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (m12) std::memcpy(g_m, m12, sizeof g_m);
    g_on.store(m12 != nullptr, std::memory_order_release);
}

bool get(float out[12]) {
    if (!g_on.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_on.load(std::memory_order_relaxed)) return false;
    std::memcpy(out, g_m, sizeof g_m);
    return true;
}

}  // namespace nxrt::color_matrix
