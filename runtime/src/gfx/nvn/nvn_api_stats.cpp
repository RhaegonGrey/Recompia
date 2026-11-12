// SPDX-License-Identifier: GPL-3.0-or-later
// The `nvn api` line of [prof] render_split (nvn_api_stats.h).
#include "nvn_api_stats.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::nvn::api_stats {

std::atomic<uint64_t> g_count[kCounts];

namespace {
std::mutex g_rows_mu;
std::vector<ThreadStats*> g_rows;   // every thread that called NVN while the split was on (never freed: a reader may hold a row)
thread_local ThreadStats* t_row = nullptr;
}  // namespace

ThreadStats* thread_stats() {
    if (t_row) return t_row;
    auto* s = new ThreadStats();
    const char* n = log_thread_name();
    std::snprintf(s->name, sizeof s->name, "%s", n && *n ? n : "?");
    std::lock_guard<std::mutex> lk(g_rows_mu);
    g_rows.push_back(s);
    t_row = s;
    return s;
}

void frame() {
    if (!on()) return;
    static std::mutex mu;
    std::unique_lock<std::mutex> lk(mu, std::try_to_lock);   // one presenting thread logs; another one at the same moment skips
    if (!lk.owns_lock()) return;
    static uint64_t t_last = 0, frames = 0, cnt_last[kCounts] = {};
    struct Last { const ThreadStats* row; uint64_t calls, ticks; };
    static std::vector<Last> last;
    struct Row { const char* name; double calls, ms; };
    std::vector<Row> rows;
    // Moves every row's and counter's last values to now; with f > 0 also fills rows (and returns the counters per frame in c).
    double c[kCounts];
    auto take = [&](double f) {
        std::lock_guard<std::mutex> rl(g_rows_mu);
        for (const ThreadStats* s : g_rows) {
            const uint64_t n = s->calls.load(std::memory_order_relaxed), t = s->ticks.load(std::memory_order_relaxed);
            auto it = std::find_if(last.begin(), last.end(), [s](const Last& l) { return l.row == s; });
            if (it == last.end()) it = last.insert(last.end(), Last{s, 0, 0});
            if (f > 0 && n > it->calls) rows.push_back({s->name, static_cast<double>(n - it->calls) / f, static_cast<double>(ft::tsc_to_ns(t - it->ticks)) / 1e6 / f});
            it->calls = n;
            it->ticks = t;
        }
        for (int i = 0; i < kCounts; ++i) {
            const uint64_t v = g_count[i].load(std::memory_order_relaxed);
            c[i] = f > 0 ? static_cast<double>(v - cnt_last[i]) / f : 0.0;
            cnt_last[i] = v;
        }
    };
    ++frames;
    const uint64_t now = ft::tsc();
    if (!t_last) {   // the window starts at the first present: the boot's calls are not in it
        t_last = now;
        frames = 0;
        take(0);
        return;
    }
    const uint64_t el = ft::tsc_to_ns(now - t_last);
    if (el < 10000000000ull || frames == 0) return;
    take(static_cast<double>(frames));
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
    std::string threads;
    char b[160];
    for (size_t i = 0; i < rows.size() && i < 4; ++i) {
        std::snprintf(b, sizeof b, "%s%s %.0f calls %.3f ms", i ? ", " : "", rows[i].name, rows[i].calls, rows[i].ms);
        threads += b;
    }
    NXLOG_I("prof", "nvn api, %llu frames, per frame: time inside NVN calls by thread: %s | texture registrations %.0f (unchanged %.0f), sampler "
            "registrations %.0f (unchanged %.0f) | flushes %.0f (textures marked %.1f, registry entries looked at %.0f) | lists %.1f, records %.0f, "
            "%.0f KiB, CopyCommands %.0f KiB",
            static_cast<unsigned long long>(frames), threads.empty() ? "none" : threads.c_str(), c[kTexReg], c[kTexRegSame], c[kSmpReg], c[kSmpRegSame],
            c[kFlush], c[kFlushHit], c[kFlushScan], c[kLists], c[kRecords], c[kRecordBytes] / 1024.0, c[kCopyBytes] / 1024.0);
    t_last = now;
    frames = 0;
}

}  // namespace nxrt::nvn::api_stats
