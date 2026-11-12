// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from SuperMarioWonderRecomp runtime/src/memguard_win.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
#include "nxrt/game.h"
#include "nxrt/memguard.h"

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxrt {
namespace {

using Clock = std::chrono::steady_clock;

struct Settings {
    uint64_t limit;
    uint32_t poll_ms;
    void (*on_exceed)(uint64_t footprint, uint64_t limit, void* user);
    void* user;
};

std::mutex g_ctl;             // serialises start / stop / running
std::mutex g_mu;              // guards g_stop and the poll wait
std::condition_variable g_cv;
bool g_stop = false;          // guarded by g_mu
std::thread g_thread;         // guarded by g_ctl
std::atomic<uint64_t> g_peak{0};
std::atomic<bool> g_tripped{false};
std::atomic<uint64_t (*)()> g_discount{nullptr};

constexpr double kBytesPerGb = 1073741824.0;

uint64_t gb_to_bytes(double gb) {
    const double b = gb * kBytesPerGb;
    if (b >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(b);
}

void raise_peak(uint64_t f) {
    uint64_t p = g_peak.load();
    while (f > p && !g_peak.compare_exchange_weak(p, f)) {
    }
}

// The process memory counters, read once for the footprint, the limit report and the log. All zero when the call fails. Under Wine
// PrivateUsage stays 0 (only the base fields are filled in, PagefileUsage among them), which is why the log shows both.
bool read_process_memory(PROCESS_MEMORY_COUNTERS_EX* pmc) {
    *pmc = PROCESS_MEMORY_COUNTERS_EX{};
    pmc->cb = sizeof *pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(pmc), sizeof *pmc) != 0;
}

// "private commit X GB, pagefile usage Y GB" for the log, each reading marked when it is 0. The footprint the guard compares is the
// private commit minus the video memory counted as the GPU's.
std::string commit_readings_text() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    read_process_memory(&pmc);
    char b[160];
    std::snprintf(b, sizeof b, "private commit %.2f GB%s, pagefile usage %.2f GB%s", static_cast<double>(pmc.PrivateUsage) / kBytesPerGb,
                  pmc.PrivateUsage ? "" : " (reads 0)", static_cast<double>(pmc.PagefileUsage) / kBytesPerGb, pmc.PagefileUsage ? "" : " (reads 0)");
    return b;
}

// The memory-limit report: same file as the crash report. The core of it (reason, footprint, frame times) is written first; the gauges follow
// once read, on a helper thread because a gauge callback takes the lock of the thing it counts and that lock may be held by a stuck thread.
void write_limit_report(uint64_t footprint, uint64_t limit) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    read_process_memory(&pmc);
    const uint64_t priv = static_cast<uint64_t>(pmc.PrivateUsage);
    char b[512];
    std::string text;
    std::snprintf(b, sizeof b, "MEMORY LIMIT EXCEEDED (reason: memory limit)\n  footprint %.2f GB is over the %.1f GB limit ([memory] limit_gb)\n",
                  static_cast<double>(footprint) / kBytesPerGb, static_cast<double>(limit) / kBytesPerGb);
    text += b;
    std::snprintf(b, sizeof b, "  private commit %.2f GB (the footprint is that minus %.2f GB of video memory), working set %.2f GB, peak working set %.2f GB, peak footprint %.2f GB\n",
                  static_cast<double>(priv) / kBytesPerGb, static_cast<double>(priv > footprint ? priv - footprint : 0) / kBytesPerGb,
                  static_cast<double>(pmc.WorkingSetSize) / kBytesPerGb, static_cast<double>(pmc.PeakWorkingSetSize) / kBytesPerGb,
                  static_cast<double>(g_peak.load()) / kBytesPerGb);
    text += b;
    std::snprintf(b, sizeof b, "  presented frames: %llu\n", static_cast<unsigned long long>(diag_frame_count()));
    text += b;
    text += frame_times_summary(30.0);

    const std::string path = crash_report_path();
    auto write = [&path](const std::string& t) {
        if (path.empty()) return;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
    };
    auto log_text = [](const std::string& t) {
        for (size_t pos = 0; pos < t.size();) {
            size_t eol = t.find('\n', pos);
            if (eol == std::string::npos) eol = t.size();
            NXLOG_E("memory", "%.*s", static_cast<int>(eol - pos), t.data() + pos);
            pos = eol + 1;
        }
    };
    log_text(text);
    log_flush();
    write(text);

    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::vector<std::pair<std::string, int64_t>> gauges;
    };
    auto sh = std::make_shared<Shared>();
    std::thread([sh] {
        auto g = gauge_read_all();
        std::lock_guard<std::mutex> lk(sh->m);
        sh->gauges = std::move(g);
        sh->done = true;
        sh->cv.notify_all();
    }).detach();
    std::string gtext;
    {
        std::unique_lock<std::mutex> lk(sh->m);
        if (sh->cv.wait_for(lk, std::chrono::seconds(3), [&] { return sh->done; })) {
            gtext = "gauges:\n";
            for (const auto& kv : sh->gauges) {
                std::snprintf(b, sizeof b, "  %s = %lld\n", kv.first.c_str(), static_cast<long long>(kv.second));
                gtext += b;
            }
        } else {
            gtext = "gauges: (not read: a gauge callback did not return within 3 s)\n";
        }
    }
    log_text(gtext);
    log_flush();
    write(text + gtext);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

void guard_loop(Settings s) {
    log_set_thread_name("memguard");
    const double limit_gb = static_cast<double>(s.limit) / kBytesPerGb;
    auto next_log = Clock::now() + std::chrono::seconds(30);
    bool warned = false;
    bool blind_noted = false;
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        // Returns true as soon as g_stop is set (memguard_stop notifies), so stop is prompt.
        if (g_cv.wait_for(lk, std::chrono::milliseconds(s.poll_ms), [] { return g_stop; })) return;
        lk.unlock();

        const uint64_t f = memguard_footprint_host();
        raise_peak(f);
        const double gb = static_cast<double>(f) / kBytesPerGb;

        if (Clock::now() >= next_log) {
            next_log = Clock::now() + std::chrono::seconds(30);
            PROCESS_MEMORY_COUNTERS_EX pmc;
            read_process_memory(&pmc);
            NXLOG_I("memory", "footprint %.2f GB (peak %.2f GB, limit %.1f GB); %s", gb,
                    static_cast<double>(g_peak.load()) / kBytesPerGb, limit_gb, commit_readings_text().c_str());
            if (!blind_noted && pmc.PrivateUsage == 0 && pmc.PagefileUsage != 0) {
                blind_noted = true;
                NXLOG_W("memory", "private commit reads 0 while pagefile usage does not: the memory limit compares the pagefile usage instead (resident anonymous memory plus swap)");
            }
        }

        if (!warned && f > s.limit / 4 * 3) {
            warned = true;
            NXLOG_W("memory", "footprint %.2f GB is past 75%% of the %.1f GB limit ([memory] limit_gb): at the limit the game is stopped with a report (%s)", gb, limit_gb,
                    commit_readings_text().c_str());
        }

        if (f > s.limit && !g_tripped.exchange(true)) {
            if (s.on_exceed) {
                s.on_exceed(f, s.limit, s.user);
            } else {
                NXLOG_E("memory", "%.2f GB is over the %.1f GB limit ([memory] limit_gb): stopping", gb, limit_gb);
                write_limit_report(f, s.limit);
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }

        lk.lock();
    }
}

}  // namespace

uint64_t memguard_footprint() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (!read_process_memory(&pmc)) return 0;
    // Under Proton the private commit can read 0 while the pagefile usage (resident anonymous memory plus swap) does not: the limit
    // would never trigger. Fall back to the pagefile usage, which is never more than the private commit on Windows.
    if (pmc.PrivateUsage == 0 && pmc.PagefileUsage != 0) return static_cast<uint64_t>(pmc.PagefileUsage);
    return static_cast<uint64_t>(pmc.PrivateUsage);
}

void memguard_set_discount(uint64_t (*gpu_bytes)()) { g_discount.store(gpu_bytes); }

uint64_t memguard_footprint_host() {
    const uint64_t f = memguard_footprint();
    auto fn = g_discount.load();
    const uint64_t d = fn ? fn() : 0;
    return d < f ? f - d : 0;
}

double memguard_limit_gb(const char* env_text) {
    const double gb = env_text ? std::atof(env_text) : 0.0;
    return (gb > 0.0 && std::isfinite(gb)) ? gb : 8.0;
}

void memguard_start(const MemGuardOptions& opt) {
    std::lock_guard<std::mutex> ctl(g_ctl);
    if (g_thread.joinable()) return;  // already running: ignored

    Settings s{};
    s.limit = opt.limit_bytes ? opt.limit_bytes : gb_to_bytes(memguard_limit_gb(config_get_str("memory", "limit_gb", game::info().platform == game::Platform::Switch2 ? "16" : "8").c_str()));
    const int64_t poll = config_get_int("memory", "poll_ms", 250);
    s.poll_ms = opt.poll_ms ? opt.poll_ms : (poll > 0 ? static_cast<uint32_t>(poll) : 250u);
    s.on_exceed = opt.on_exceed;
    s.user = opt.user;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = false;
    }
    g_peak.store(0);
    g_tripped.store(false);
    g_thread = std::thread(guard_loop, s);
}

void memguard_stop() {
    std::lock_guard<std::mutex> ctl(g_ctl);
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    g_thread.join();
}

}  // namespace nxrt
