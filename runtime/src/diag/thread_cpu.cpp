// SPDX-License-Identifier: GPL-3.0-or-later
// Thread CPU table: see thread_cpu.h.
//
// One sample: the thread list of this process (a toolhelp snapshot), GetThreadTimes on a query-only handle per thread, GetProcessTimes for
// the whole process. A handle is opened the first time a thread is listed and kept until the thread is gone from the list, so its last
// interval is still counted and its id cannot be given to another thread while it is tracked. A thread first listed now that was created
// after the previous sample counts from zero; one that started and ended between two samples is never listed and shows only as the
// difference between the process and the threads ("ended").
//
// The line (tag "tcpu"), numbers in ms of CPU per second of wall time, then the same as percent of one processor:
//   thread cpu over 10.0 s on 8 cpus, ms/s (% of one): process 4840 (484.0%), threads 4801 (480.1%); main 912 (91.2%), nvn-render 896 (89.6%),
//   nvn-gpl x6 410 (41.0%), tid 4312 133 (13.3%), ...; 37 more 21 (2.1%); ended 39 (3.9%)
// Threads with the same name are one entry ("x6" = six threads); a thread without a name is "tid <host thread id>". The entries are the
// kRows busiest, sorted; "more" is every other thread together.
#include "thread_cpu.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::diag {
namespace {

constexpr const char* kTag = "tcpu";
constexpr size_t kRows = 16;
constexpr double kListMs = 0.5;      // an entry with less than this many ms/s is not listed (it goes into "more")

uint64_t ft64(const FILETIME& f) { return static_cast<uint64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime; }

struct Tracked {
    HANDLE h = nullptr;
    uint64_t created = 0;
    uint64_t cpu = 0;                // kernel + user time at the previous sample, 100 ns units
    bool fresh = true;               // opened in this sample: `created` and `cpu` are not read yet
    bool listed = true;              // in this sample's thread list
};

struct State {
    bool read = false, on = false;
    std::unordered_map<uint32_t, Tracked> threads;      // by host thread id
    uint64_t at_ticks = 0;           // qpc of the previous sample (0 = none yet)
    uint64_t at_time = 0;            // system time of the previous sample, to tell a thread created since
    uint64_t proc_cpu = 0;
};

struct Name {
    uint64_t created = 0;
    std::string name;
};

struct Row {
    std::string label;
    unsigned count = 0;
    uint64_t cpu = 0;                // 100 ns units used in the interval
};

// Marks the tracked threads that are still in the process and opens the new ones. False when the list could not be read.
bool list_threads(State& s) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    for (auto& kv : s.threads) kv.second.listed = false;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        auto it = s.threads.find(te.th32ThreadID);
        if (it != s.threads.end()) {
            it->second.listed = true;
            continue;
        }
        Tracked t;
        t.h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (t.h) s.threads.emplace(te.th32ThreadID, t);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return true;
}

}  // namespace

void thread_cpu_tick() {
    static State s;                  // the hitch monitor thread is the only caller
    if (!s.read) {
        s.read = true;
        s.on = config_get_bool("diag", "thread_cpu", true);
        if (s.on) NXLOG_I(kTag, "thread CPU table: one line with every frame summary ([diag] thread_cpu)");
    }
    if (!s.on) return;

    const uint64_t now_ticks = qpc::ticks();
    FILETIME ft_now, created, exited, kernel_t, user_t;
    GetSystemTimeAsFileTime(&ft_now);
    const uint64_t now_time = ft64(ft_now);
    uint64_t proc_cpu = s.proc_cpu;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_t, &user_t)) proc_cpu = ft64(kernel_t) + ft64(user_t);
    if (!list_threads(s)) return;

    std::unordered_map<uint32_t, Name> names;
    log_each_thread_name(
        [](uint32_t tid, uint64_t made, const char* name, void* user) {
            if (name[0]) (*static_cast<std::unordered_map<uint32_t, Name>*>(user))[tid] = Name{made, name};
        },
        &names);

    const bool have_prev = s.at_ticks != 0;
    std::unordered_map<std::string, size_t> by_label;
    std::vector<Row> rows;
    uint64_t threads_cpu = 0;
    for (auto it = s.threads.begin(); it != s.threads.end();) {
        Tracked& t = it->second;
        uint64_t used = 0;
        if (GetThreadTimes(t.h, &created, &exited, &kernel_t, &user_t)) {
            const uint64_t cpu = ft64(kernel_t) + ft64(user_t);
            if (t.fresh) {
                t.created = ft64(created);
                t.cpu = (have_prev && t.created >= s.at_time) ? 0 : cpu;     // created since the previous sample: all of it is this interval's
                t.fresh = false;
            }
            used = cpu > t.cpu ? cpu - t.cpu : 0;
            t.cpu = cpu;
        }
        if (have_prev) {
            threads_cpu += used;
            auto nm = names.find(it->first);
            const std::string label = (nm != names.end() && nm->second.created == t.created) ? nm->second.name : "tid " + std::to_string(it->first);
            auto [slot, added] = by_label.try_emplace(label, rows.size());
            if (added) rows.push_back(Row{label, 0, 0});
            rows[slot->second].count++;
            rows[slot->second].cpu += used;
        }
        if (!t.listed) {             // ended: its last interval is counted above
            CloseHandle(t.h);
            it = s.threads.erase(it);
        } else {
            ++it;
        }
    }

    const double secs = have_prev ? static_cast<double>(now_ticks - s.at_ticks) / static_cast<double>(qpc::freq()) : 0;
    const uint64_t proc_used = proc_cpu > s.proc_cpu ? proc_cpu - s.proc_cpu : 0;
    s.at_ticks = now_ticks;
    s.at_time = now_time;
    s.proc_cpu = proc_cpu;
    if (!(secs > 0)) return;         // the first sample only sets the baselines

    auto ms_s = [secs](uint64_t cpu) { return static_cast<double>(cpu) / 10000.0 / secs; };
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu != b.cpu ? a.cpu > b.cpu : a.label < b.label; });
    char b[160];
    std::snprintf(b, sizeof b, "thread cpu over %.1f s on %u cpus, ms/s (%% of one): process %.0f (%.1f%%), threads %.0f (%.1f%%)", secs,
                  std::max(1u, std::thread::hardware_concurrency()), ms_s(proc_used), ms_s(proc_used) / 10.0, ms_s(threads_cpu), ms_s(threads_cpu) / 10.0);
    std::string line = b;
    size_t shown = 0;
    unsigned more_threads = 0;
    uint64_t more_cpu = 0;
    for (const Row& r : rows) {
        if (shown < kRows && ms_s(r.cpu) >= kListMs) {
            char cnt[16] = "";
            if (r.count > 1) std::snprintf(cnt, sizeof cnt, " x%u", r.count);
            std::snprintf(b, sizeof b, "%s%.60s%s %.0f (%.1f%%)", shown ? ", " : "; ", r.label.c_str(), cnt, ms_s(r.cpu), ms_s(r.cpu) / 10.0);
            line += b;
            shown++;
        } else {
            more_threads += r.count;
            more_cpu += r.cpu;
        }
    }
    if (more_threads) {
        std::snprintf(b, sizeof b, "; %u more %.0f (%.1f%%)", more_threads, ms_s(more_cpu), ms_s(more_cpu) / 10.0);
        line += b;
    }
    const uint64_t ended = proc_used > threads_cpu ? proc_used - threads_cpu : 0;
    std::snprintf(b, sizeof b, "; ended %.0f (%.1f%%)", ms_s(ended), ms_s(ended) / 10.0);
    line += b;
    NXLOG_I(kTag, "%s", line.c_str());
}

}  // namespace nxrt::diag
