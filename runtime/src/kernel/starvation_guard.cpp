// SPDX-License-Identifier: GPL-3.0-or-later
// Aging for guest threads that the host scheduler leaves ready but never runs (S20, debug note 422).
//
// The console core model pins every guest thread to one host processor and maps the HOS priorities onto six Windows bands with the wake-up
// boost off (kernel/sched.cpp, debug notes 180 and 221). That keeps the console's strict priority order, and it has a price on a shared PC:
// the lowest bands rank at or below ordinary programs, so while a build or another program keeps the pinned processor busy, a thread in
// those bands stays ready and gets no time. The resource loader sits in the lowest band: with a normal-priority load on its processor the
// game's load never finishes (the frames keep coming, the loader sits in its decompress loop) and a scripted run reports a "hang".
// Windows' own remedy, a boost to 15 for two time slices once a thread has waited four seconds, is too slow for that.
//
// This guard asks the system which of our threads are in the Ready state (NtQuerySystemInformation: thread state and context-switch count).
// A guest thread that was ready at several polls in a row without a single context switch in between gets Windows' top normal priority for
// 450 ms (and its yields stop handing the processor over, which cost it most of a boost: a yield gives a time slice to any ready thread, a lower
// priority one included), then goes back to its band. Healthy runs never starve for that long, so nothing changes for them: it is an escape from
// starvation, not a second scheduler. Under [sched] affinity = strict the boost also widens the thread's affinity to the whole processor pool
// for its length: a higher priority alone does not help a thread pinned to a processor that another thread at the top normal priority (the
// highest band is 15 in the high priority class, the same as the boost) or DPC work holds; any idle processor does. Config: [sched] starvation_guard (default on), [sched] starvation_ms (default 100, time a thread must
// have been ready and unscheduled before it is boosted), [sched] starvation_poll_ms (default 50).
//
// The guard can only work on a host that fills in the thread state and the context-switch count. Its own thread proves it: the thread that
// asks is Running, and has been switched in at least once, in its own record. A host whose records say otherwise can never show the guard
// a starved thread, and the query is not free (under Proton on the Steam Deck the 50 ms poll took 7% of one core in play), so after
// kProbePolls polls without that proof the guard thread ends ([sched] starvation_guard_probe, default on; off: it polls regardless).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::kernel {
namespace {

// The parts of the NtQuerySystemInformation(SystemProcessInformation) records that are used (stable since Windows Vista, x64 layout).
struct ThreadRec {
    LARGE_INTEGER kernel_time, user_time, create_time;
    ULONG wait_time;
    PVOID start_address;
    PVOID unique_process, unique_thread;
    LONG priority, base_priority;
    ULONG context_switches, thread_state, wait_reason;
};
static_assert(sizeof(ThreadRec) == 0x50, "SYSTEM_THREAD_INFORMATION");
struct ProcRec {
    ULONG next_entry_offset, number_of_threads;
    uint8_t body[0x100 - 8];       // the rest of SYSTEM_PROCESS_INFORMATION up to the thread array
};
static_assert(sizeof(ProcRec) == 0x100, "SYSTEM_PROCESS_INFORMATION");
constexpr ULONG kStateReady = 1;
constexpr ULONG kStateRunning = 2;
constexpr ULONG kStateStandby = 3;
constexpr int kSystemProcessInformation = 5;

using NtQuery = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

struct Guard {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool stop = false;
    std::atomic<uint64_t> boosts{0};
};

Guard& guard() {
    static Guard* g = new Guard;     // never destroyed: a joinable thread must not die with the static
    return *g;
}

struct Seen {
    ULONG switches = 0;
    int ready_polls = 0;
    int boost_polls = 0;
    bool boosted = false;
};
constexpr int kBoostMs = 450;      // a boost lasts this long (rounded up to whole polls)
constexpr int kProbePolls = 3;     // polls the host gets to show the polling thread as Running ([sched] starvation_guard_probe)

// The polling thread's own record: Running, with a context-switch count, on a host that reports the two fields the guard reads.
const ThreadRec* own_record(const ThreadRec* recs, ULONG n, uint32_t self) {
    for (ULONG i = 0; i < n; i++)
        if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(recs[i].unique_thread)) == self) return &recs[i];
    return nullptr;
}

// CPU time (100 ns units) of every thread of this process and of every other process at one poll: the starvation diagnosis compares the
// poll at which the starved thread was first seen ready with the poll that boosts it, which names who had the processor meanwhile.
struct CpuSnap {
    std::unordered_map<uint32_t, uint64_t> mine;              // host tid -> kernel + user time
    std::unordered_map<uint64_t, uint64_t> procs;             // pid -> kernel + user time (other processes)
    std::unordered_map<uint64_t, std::wstring> names;         // pid -> image name
};

// The snapshot of one poll, from the system's process list in `list`. Our process is the record whose threads are `mine`.
// UniqueProcessId sits at offset 0x50 of SYSTEM_PROCESS_INFORMATION on x64; UserTime 0x28, KernelTime 0x30, ImageName 0x38 (buffer at 0x40).
constexpr size_t kProcIdOff = 0x50;
CpuSnap take_snap(const uint8_t* list, uint32_t pid, const ThreadRec* mine, ULONG nthreads) {
    CpuSnap snap;
    for (size_t off = 0;;) {
        const ProcRec* p = reinterpret_cast<const ProcRec*>(list + off);
        const uint8_t* rec = list + off;
        const uint64_t ppid = *reinterpret_cast<const uint64_t*>(rec + kProcIdOff);
        if (ppid != pid) {
            snap.procs[ppid] = *reinterpret_cast<const uint64_t*>(rec + 0x28) + *reinterpret_cast<const uint64_t*>(rec + 0x30);
            const USHORT len = *reinterpret_cast<const USHORT*>(rec + 0x38);
            const wchar_t* nm = *reinterpret_cast<const wchar_t* const*>(rec + 0x40);
            if (nm && len) snap.names[ppid].assign(nm, len / sizeof(wchar_t));
        }
        if (!p->next_entry_offset) break;
        off += p->next_entry_offset;
    }
    for (ULONG i = 0; i < nthreads; i++)
        snap.mine[static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mine[i].unique_thread))] =
            static_cast<uint64_t>(mine[i].kernel_time.QuadPart) + static_cast<uint64_t>(mine[i].user_time.QuadPart);
    return snap;
}

const char* band_name(int host_prio) {
    switch (host_prio) {
        case THREAD_PRIORITY_TIME_CRITICAL: return "time critical";
        case THREAD_PRIORITY_HIGHEST: return "highest";
        case THREAD_PRIORITY_ABOVE_NORMAL: return "above normal";
        case THREAD_PRIORITY_NORMAL: return "normal";
        case THREAD_PRIORITY_BELOW_NORMAL: return "below normal";
        case THREAD_PRIORITY_LOWEST: return "lowest";
        default: return "?";
    }
}

// One line: the starved thread, its band and console cores, and the busiest threads of this process and the busiest other programs between
// the two snapshots (ms of CPU). A starver of ours shows as a thread with about the whole window; none: another program or the system.
// The thread's host placement when it was found starved (before the boost changed it): affinity, ideal processor, the processor of one of
// its last 16 SVCs (svc_dispatch samples it).
std::string placement(const KThread& t) {
    PROCESSOR_NUMBER pn{};
    char ideal[24] = "?";
    if (GetThreadIdealProcessorEx(static_cast<HANDLE>(t.host_handle), &pn)) std::snprintf(ideal, sizeof ideal, "%u:%u", pn.Group, pn.Number);
    char buf[128];
    std::snprintf(buf, sizeof buf, "host affinity 0x%llx, ideal processor %s, last ran on processor %d (at a recent SVC)",
                  static_cast<unsigned long long>(t.host_affinity.load(std::memory_order_relaxed)), ideal, t.last_proc.load(std::memory_order_relaxed));
    return buf;
}

void log_starvation(const KThread& t, const std::string& where, int ready_ms, const CpuSnap& a, const CpuSnap& b, uint64_t boost_no) {
    std::unordered_map<uint32_t, std::string> names;
    for (const ThreadRecord& r : diag_threads()) names[r.host_tid] = r.name;
    std::vector<std::pair<uint64_t, uint32_t>> mine;
    for (const auto& [tid, v] : b.mine) {
        auto it = a.mine.find(tid);
        const uint64_t d = (it != a.mine.end() && it->second <= v) ? v - it->second : 0;
        if (d) mine.emplace_back(d, tid);
    }
    std::sort(mine.begin(), mine.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::vector<std::pair<uint64_t, uint64_t>> procs;
    for (const auto& [pid, v] : b.procs) {
        auto it = a.procs.find(pid);
        if (it == a.procs.end() || it->second > v) continue;
        if (v - it->second) procs.emplace_back(v - it->second, pid);
    }
    std::sort(procs.begin(), procs.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::string busy, other;
    char buf[160];
    for (size_t i = 0; i < mine.size() && i < 5; i++) {
        auto it = names.find(mine[i].second);
        const std::string n = it != names.end() ? it->second : "host thread " + std::to_string(mine[i].second);
        std::snprintf(buf, sizeof buf, "%s%s %.0f", busy.empty() ? "" : ", ", n.c_str(), static_cast<double>(mine[i].first) / 10000.0);
        busy += buf;
    }
    for (size_t i = 0; i < procs.size() && i < 3; i++) {
        std::string n;
        auto it = b.names.find(procs[i].second);
        if (it != b.names.end())
            for (wchar_t c : it->second) n += c < 128 ? static_cast<char>(c) : '?';
        if (n.empty()) n = procs[i].second == 0 ? "idle" : "pid " + std::to_string(procs[i].second);
        std::snprintf(buf, sizeof buf, "%s%s %.0f", other.empty() ? "" : ", ", n.c_str(), static_cast<double>(procs[i].first) / 10000.0);
        other += buf;
    }
    NXLOG_W("sched", "starved: %s (console cores 0x%llx, band %s; %s) was ready for %d ms without running: boosted for %d ms (boost %llu). CPU ms in that window: "
            "busiest threads of this process: %s; busiest other processes: %s",
            t.name.c_str(), static_cast<unsigned long long>(t.core_mask), band_name(sched_host_priority(t.prio)), where.c_str(), ready_ms, kBoostMs,
            static_cast<unsigned long long>(boost_no), busy.empty() ? "none" : busy.c_str(), other.empty() ? "none" : other.c_str());
}

void loop(Guard* g, int poll_ms, int need_polls, int probe_polls) {
    log_set_thread_name("starve-guard");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);        // it must run when everything else is starved
    NtQuery nt_query = reinterpret_cast<NtQuery>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"));
    if (!nt_query) return;
    std::vector<uint8_t> buf(1 << 20);
    std::unordered_map<uint32_t, Seen> seen;       // by host thread id
    const uint32_t pid = GetCurrentProcessId();
    const uint32_t self = GetCurrentThreadId();
    const int boost_polls = std::max(1, (kBoostMs + poll_ms - 1) / poll_ms);
    std::unordered_map<uint32_t, const ThreadRec*> by_tid;
    // The snapshots of the last polls in a row at which a guest thread was ready (need_polls at most, oldest first). Only a boost reads
    // them, and the thread it boosts was ready at each of the last need_polls polls, so the list is whole whenever it is read; a poll
    // with no ready guest thread takes none and empties it.
    std::deque<CpuSnap> hist;
    ULONGLONG last_line = 0;
    uint64_t quiet = 0;                            // boosts not logged since the last line (at most one line per second)
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g->m);
            if (g->cv.wait_for(lk, std::chrono::milliseconds(poll_ms), [g] { return g->stop; })) return;
        }
        ULONG need = 0;
        LONG st = nt_query(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &need);
        if (st < 0) {
            if (need > buf.size()) buf.resize(need + (need >> 3));
            continue;
        }
        const ThreadRec* mine = nullptr;
        ULONG nthreads = 0;
        for (size_t off = 0;;) {
            const ProcRec* p = reinterpret_cast<const ProcRec*>(buf.data() + off);
            const uint8_t* rec = buf.data() + off;
            if (*reinterpret_cast<const uint64_t*>(rec + kProcIdOff) == pid) {
                mine = reinterpret_cast<const ThreadRec*>(rec + sizeof(ProcRec));
                nthreads = p->number_of_threads;
                break;
            }
            if (!p->next_entry_offset) break;
            off += p->next_entry_offset;
        }
        if (probe_polls > 0) {                          // before the test for our process: a list without it proves nothing either
            const ThreadRec* own = mine ? own_record(mine, nthreads, self) : nullptr;
            if (own && own->thread_state == kStateRunning && own->context_switches) {
                probe_polls = 0;
                NXLOG_I("sched", "starvation guard: this host reports thread states and context switches, the guard keeps polling");
            } else if (--probe_polls == 0) {
                NXLOG_I("sched", "starvation guard off: this host does not report thread states and context switches (the polling thread's own record: %s, state %lu, "
                        "%lu context switches), so no starved thread could ever be seen; the guard thread ends. [sched] starvation_guard_probe = 0 keeps it polling",
                        own ? "found" : "missing", own ? own->thread_state : 0ul, own ? own->context_switches : 0ul);
                return;
            }
        }
        if (!mine) continue;
        bool snapped = false;
        const auto snap_once = [&] {
            if (snapped) return;
            snapped = true;
            hist.push_back(take_snap(buf.data(), pid, mine, nthreads));
            while (hist.size() > static_cast<size_t>(need_polls)) hist.pop_front();
        };
        by_tid.clear();
        for (ULONG i = 0; i < nthreads; i++) by_tid[static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mine[i].unique_thread))] = &mine[i];
        for (const std::shared_ptr<KThread>& t : threads()) {
            if (!t->started || t->exited || !t->host_handle || !t->host_tid) continue;
            auto it = by_tid.find(t->host_tid);
            Seen& s = seen[t->host_tid];
            if (it == by_tid.end()) {
                s = Seen{};
                continue;
            }
            const ThreadRec& r = *it->second;
            const bool ready = r.thread_state == kStateReady || r.thread_state == kStateStandby;
            if (ready && r.context_switches == s.switches) s.ready_polls++;
            else s.ready_polls = ready ? 1 : 0;
            s.switches = r.context_switches;
            if (s.ready_polls && !s.boosted) snap_once();
            HANDLE h = static_cast<HANDLE>(t->host_handle);
            if (s.boosted) {                                    // boosted: keep it for boost_polls polls, then back to its band
                if (++s.boost_polls >= boost_polls) {
                    sched_apply(*t);                            // its band, and under strict its own processor again
                    t->aged = false;
                    s.boosted = false;
                    s.ready_polls = 0;
                }
            } else if (s.ready_polls >= need_polls) {
                const std::string where = placement(*t);
                const SchedInfo si = sched_info();
                if (si.active && si.pool) {                     // strict: any idle processor of the pool; soft: already the pool
                    SetThreadAffinityMask(h, static_cast<DWORD_PTR>(si.pool));
                    t->host_affinity.store(si.pool, std::memory_order_relaxed);
                }
                SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
                t->aged = true;                                 // its yields keep the processor while it is aged
                s.boosted = true;
                s.boost_polls = 0;
                const uint64_t n = ++g->boosts;
                const ULONGLONG now = GetTickCount64();
                if (now - last_line >= 1000) {          // every boost names its starver, at most one line per second
                    last_line = now;
                    if (quiet) NXLOG_W("sched", "(%llu more starvation boosts since the last line)", static_cast<unsigned long long>(quiet));
                    quiet = 0;
                    log_starvation(*t, where, (s.ready_polls - 1) * poll_ms, hist.front(), hist.back(), n);
                } else {
                    ++quiet;
                }
            }
        }
        if (!snapped) hist.clear();
    }
}

}  // namespace

void starvation_guard_start() {
    if (!config_get_bool("sched", "starvation_guard", true)) {
        NXLOG_I("sched", "[sched] starvation_guard = 0: guest threads the host leaves ready are not aged");
        return;
    }
    Guard& g = guard();
    std::lock_guard<std::mutex> lk(g.m);
    if (g.th.joinable()) return;
    // A thread counts as starved when it was ready at `need` polls in a row with no context switch in between: proven ready for (need - 1)
    // polls. The old 150 ms poll with two polls let a starved game thread sit 150..300 ms (the ~230 ms hitches of the p1 soak line up with
    // boosts of main and guest-89); a 50 ms poll and 100 ms catch it after 100..150 ms.
    const int ms = static_cast<int>(std::max<int64_t>(1, config_get_int("sched", "starvation_ms", 100)));
    const int poll = static_cast<int>(std::clamp<int64_t>(config_get_int("sched", "starvation_poll_ms", 50), 10, 1000));
    const int need = (ms + poll - 1) / poll + 1;
    g.stop = false;
    // [sched] starvation_guard_probe: the guard ends itself on a host that cannot show it a starved thread (see the top of the file)
    const int probe = config_get_bool("sched", "starvation_guard_probe", true) ? kProbePolls : 0;
    g.th = std::thread(loop, &g, poll, need, probe);
    NXLOG_I("sched", "starvation guard on: a guest thread ready for %d ms without running (polled every %d ms) is boosted to top priority on the whole processor pool for %d ms (debug note 422)",
            (need - 1) * poll, poll, kBoostMs);
}

void starvation_guard_stop() {
    Guard& g = guard();
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (!g.th.joinable()) return;
        g.stop = true;
    }
    g.cv.notify_all();
    g.th.join();
}

uint64_t starvation_boosts() { return guard().boosts.load(); }

namespace {
// Gauge (include/nxrt/gauge.h). The guard object is never destroyed (see guard()), so the callback stays valid through exit.
const bool g_gauge_boosts_registered = [] {
    nxrt::gauge_register("kernel.starvation_boosts", [] { return static_cast<int64_t>(starvation_boosts()); });
    nxrt::hitch_add_counter("starvation_boosts", []() -> uint64_t { return starvation_boosts(); });   // the hitch line: a slow frame that ended with a boost
    return true;
}();
}  // namespace

}  // namespace nxrt::kernel
