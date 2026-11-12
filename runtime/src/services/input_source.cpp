// SPDX-License-Identifier: GPL-3.0-or-later
// The input source the hid service reads (include/nxrt/input_source.h), the filter it runs each snapshot through, and the vibration sink
// the hid service forwards to.
#include <atomic>
#include <mutex>

#include "nxrt/input_source.h"
#include "services_internal.h"

namespace nxrt::hid {

namespace {
std::mutex g_mu;
std::shared_ptr<InputSource> g_src;
// The vibration sink and its user pointer. The sink is called with g_vib_mu held, so after set_vibration_sink() returns no call is in flight;
// a sink must not call set_vibration_sink() itself.
std::mutex g_vib_mu;
VibrationSink g_vib_fn = nullptr;
void* g_vib_user = nullptr;
std::atomic<uint64_t> g_sampled{0};
// The input filter, called with g_filter_mu held (as the vibration sink).
std::mutex g_filter_mu;
InputFilter g_filter_fn = nullptr;
void* g_filter_user = nullptr;
}  // namespace

void set_input_filter(InputFilter fn, void* user) {
    std::lock_guard<std::mutex> lk(g_filter_mu);
    g_filter_fn = fn;
    g_filter_user = user;
}

uint64_t sampled_buttons() { return g_sampled.load(std::memory_order_relaxed); }
void set_sampled_buttons(uint64_t buttons) { g_sampled.store(buttons, std::memory_order_relaxed); }

void set_input_source(std::shared_ptr<InputSource> src) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_src = std::move(src);
}

std::shared_ptr<InputSource> input_source() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_src;
}

void set_vibration_sink(VibrationSink fn, void* user) {
    std::lock_guard<std::mutex> lk(g_vib_mu);
    g_vib_fn = fn;
    g_vib_user = user;
}

}  // namespace nxrt::hid

namespace nxrt::services {

// Called by the hid service on each snapshot before it samples it. Does nothing when no filter is registered.
void input_filter_apply(nxrt::hid::InputSnapshot& snap) {
    std::lock_guard<std::mutex> lk(nxrt::hid::g_filter_mu);
    if (nxrt::hid::g_filter_fn) nxrt::hid::g_filter_fn(snap, nxrt::hid::g_filter_user);
}

// Called by the hid service after it has stored a vibration value for one handle. Does nothing when no sink is registered.
void vibration_dispatch(uint32_t npad_slot, int hand, const nxrt::hid::VibrationValue& v) {
    std::lock_guard<std::mutex> lk(nxrt::hid::g_vib_mu);
    if (nxrt::hid::g_vib_fn) nxrt::hid::g_vib_fn(npad_slot, hand, v, nxrt::hid::g_vib_user);
}

}  // namespace nxrt::services
