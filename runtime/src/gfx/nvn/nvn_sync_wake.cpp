// SPDX-License-Identifier: GPL-3.0-or-later
// The early wake of the game's sync polls ([kernel] early_wake, nxrt/early_wake.h). A game thread that waits for the GPU polls a sync
// with timeout 0 (nvnSyncWait) and sleeps a short time when it is not signaled. sync_wake_want arms that sleep; the wake fires when the
// completed serial advances (complete_serial, nvn_gpu.cpp). A poll only reads the timeline, so somebody has to be waiting on it for the
// advance to be seen when it happens and not at the next poll: the nvn-sync-wake thread here, while polls keep asking. It waits for the
// next value after the completed one, so any submission that completes ends the sleeps; a thread whose sync is a later one polls again,
// finds it unsignaled and arms again.
#include <atomic>
#include <chrono>
#include <thread>

#include "nxrt/early_wake.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {

namespace {
constexpr uint64_t kSliceNs = 2ull * 1000 * 1000;   // one timeline wait; a poll loop asks again well within it

std::thread g_th;                     // started and stopped with the back end (install / tear_down, under its install lock)
std::atomic<bool> g_run{false};
std::atomic<uint32_t> g_want{0};      // 1: a poll found its sync unsignaled since the watcher last looked
std::atomic<uint64_t> g_seen{0};      // completions the watcher saw first

GaugeHook gauge_seen("nvn.sync_wake.seen", [] { return static_cast<int64_t>(g_seen.load(std::memory_order_relaxed)); });

void watch_loop() {
    log_set_thread_name("nvn-sync-wake");
    for (;;) {
        g_want.wait(0);
        if (!g_run.load()) return;
        g_want.store(0);
        const auto t0 = std::chrono::steady_clock::now();
        if (timeline_wait(completed_serial() + 1, kSliceNs)) {
            g_seen.fetch_add(1, std::memory_order_relaxed);
            poll_completed();   // GPU writes land, then the serial is published: complete_serial fires the wake
        } else if (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // not a timeout (device lost, an error): no busy loop while polls go on
        }
    }
}
}  // namespace

void sync_wake_start() {
    if (!kernel::early_wake_enabled() || g_th.joinable()) return;
    g_want.store(0);
    g_run.store(true);
    g_th = std::thread(watch_loop);
    NXLOG_I("nvn", "sync wake: sync polls arm the kernel's early wake; the nvn-sync-wake thread waits on the timeline while they poll");
}

void sync_wake_stop() {
    if (!g_th.joinable()) return;
    g_run.store(false);
    g_want.store(1);
    g_want.notify_all();
    g_th.join();
}

void sync_wake_want(uint32_t token) {
    if (!g_run.load(std::memory_order_relaxed)) return;
    kernel::early_wake_arm(token);
    if (!g_want.exchange(1)) g_want.notify_one();
}

}  // namespace nxrt::nvn::be::gpu
