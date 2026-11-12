// SPDX-License-Identifier: GPL-3.0-or-later
// High-resolution sleep (the system tick is in core/clock.cpp). Pattern from smo-recomp runtime/os/os_threads.c sleep_ns.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "early_wake_internal.h"
#include "nxrt/clock.h"
#include "nxrt/config.h"
#include "nxrt/early_wake.h"
#include "nxrt/gauge.h"
#include "nxrt/host_device.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "kernel_internal.h"

namespace nxrt::kernel {

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so the wait runs on a high-resolution waitable timer (100 ns units); Sleep() is only the fallback.
// A zero or negative span yields, as on the console.
namespace {
// Short sleeps as real sleeps: under Wine, NtDelayExecution (not alertable) is a nanosleep on the host, good to tens of microseconds,
// so a 100 us poll costs no processor time, but wakes ~50 us late (Linux timer slack). On Windows it rounds up to the timer's grid
// (about half a millisecond or more). [kernel] short_sleep = auto | yield | sleep | wait (sleep and wait only under Wine; Windows always
// yields). auto: wait on a handheld (host_is_handheld; sleep when ntdll has no address wait), yield elsewhere.
// Measured in game with sleep against yield: on the PC, uncapped, yielding 231 fps on 4.1 cores and sleeping 198 fps on 3.8 cores; on a Steam Deck LCD, where the frame waits on the GPU anyway and CPU and GPU share one power budget,
// sleeping 54 fps (1% lows ~30) against 52 yielding (lows ~27).
// Pokopia polls its GPU fences with ~80 us SleepThread calls on two threads, thousands a second: yielded through, each keeps a
// processor core busy for as long as the GPU takes.
//
// On a Steam Deck OLED under Proton the same call measured ~1.1 ms for a 100 us request (a 60 s trace in the world: 46,000 sleeps on each
// fence-polling thread). NtDelayExecution hands the processor over first (NtYieldExecution) and then sleeps in select(); which of the
// two costs the millisecond with every core busy is not known. short_sleep = wait (auto on a Wine handheld) sleeps on the host's timed
// address wait instead (early_wake.cpp: under Wine a futex wait with the host kernel's timer, no hand-over, no wineserver call), asked
// for the span less the wait's learned lateness, with a remainder of kWaitSpinMaxNs or less spun through. It is also the wait the early
// wake ends (nxrt/early_wake.h). [kernel] short_sleep_max_us (default 200) is the longest span handled as a short sleep.
using NtDelayExecutionFn = LONG(WINAPI*)(BOOLEAN, PLARGE_INTEGER);
struct ShortCfg {
    detail::ShortSleep mode = detail::ShortSleep::Yield;
    NtDelayExecutionFn delay = nullptr;   // mode Delay
    int64_t max_ns = 200000;
};
const ShortCfg& short_cfg() {
    static const ShortCfg cfg = [] {
        ShortCfg c;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const bool wine = host_is_wine();
        const std::string m = config_get_str("kernel", "short_sleep", "auto");
        const bool hosted = wine && (m == "sleep" || m == "wait" || (m == "auto" && host_is_handheld()));   // not yielded through
        if (hosted && m != "sleep" && detail::address_wait_available()) {
            c.mode = detail::ShortSleep::Wait;
        } else if (hosted && ntdll) {   // sleep, or no address wait in this ntdll
            c.delay = reinterpret_cast<NtDelayExecutionFn>(GetProcAddress(ntdll, "NtDelayExecution"));
            if (c.delay) c.mode = detail::ShortSleep::Delay;
        }
        c.max_ns = std::clamp<int64_t>(config_get_int("kernel", "short_sleep_max_us", 200), 0, 5000) * 1000;
        NXLOG_I("kernel", "short sleeps (up to %lld us): %s ([kernel] short_sleep = %s%s)", static_cast<long long>(c.max_ns / 1000),
                c.mode == detail::ShortSleep::Wait ? "timed address wait (RtlWaitOnAddress), lateness corrected"
                : c.mode == detail::ShortSleep::Delay ? "slept (NtDelayExecution)" : "yielded through",
                m.c_str(), !wine ? ", Windows" : m == "auto" ? (host_is_handheld() ? ", a handheld" : ", not a handheld") : "");
        return c;
    }();
    return cfg;
}

int64_t qpc_ns() { return static_cast<int64_t>(qpc::now_ns()); }

// What short sleeps cost here: a line after the first kShortLogAt of them, and the running lateness as a gauge. Sleeps an early wake ended
// are not in the averages.
constexpr uint64_t kShortLogAt = 4096;
std::atomic<uint64_t> g_short_n{0}, g_short_asked_ns{0}, g_short_took_ns{0}, g_short_worst_ns{0};
std::atomic<int64_t> g_short_over{0};   // moving average of took - asked, ns
GaugeHook gauge_short_over("kernel.short_sleep_over_us", [] { return g_short_over.load(std::memory_order_relaxed) / 1000; });

void short_sleep_note(int64_t asked_ns, int64_t took_ns) {
    const int64_t over = took_ns - asked_ns, avg = g_short_over.load(std::memory_order_relaxed);
    g_short_over.store(avg + (over - avg) / 16, std::memory_order_relaxed);   // racing threads may drop a sample: harmless for an average
    // The sums only make the one line below: once it is written, a short sleep updates the average alone (no locked add per sleep).
    if (g_short_n.load(std::memory_order_relaxed) >= kShortLogAt) return;
    g_short_asked_ns.fetch_add(static_cast<uint64_t>(asked_ns), std::memory_order_relaxed);
    g_short_took_ns.fetch_add(static_cast<uint64_t>(took_ns), std::memory_order_relaxed);
    uint64_t worst = g_short_worst_ns.load(std::memory_order_relaxed);
    while (static_cast<uint64_t>(took_ns) > worst && !g_short_worst_ns.compare_exchange_weak(worst, static_cast<uint64_t>(took_ns), std::memory_order_relaxed)) {}
    if (g_short_n.fetch_add(1, std::memory_order_relaxed) + 1 != kShortLogAt) return;
    NXLOG_I("kernel", "short sleeps, the first %llu: asked %.0f us on average, took %.0f us (longest %.0f us)", static_cast<unsigned long long>(kShortLogAt),
            static_cast<double>(g_short_asked_ns.load()) / kShortLogAt / 1e3, static_cast<double>(g_short_took_ns.load()) / kShortLogAt / 1e3,
            static_cast<double>(g_short_worst_ns.load()) / 1e3);
}

// One sleep on the timed address wait, from t0 for ns; token set: an early wake ends it (true then); word set too: the sleep is on that
// word instead and ends when it no longer holds *token (true then). *end: the last clock reading it made (qpc_ns), so the caller needs no
// other; 0 when the wake or the word ended the wait with no reading after it. The wait is asked for the time left
// less its learned lateness (start 60 us: the Linux timer slack of 50 us plus the wake-up; precise_late_update's average), never for
// less than kWaitMinNs, so a host whose wait is far later than the span sleeps late instead of spinning the span through. What is left
// after the wait is spun when it is kWaitSpinMaxNs or less, else waited again.
constexpr int64_t kWaitLateStartNs = 60000;
constexpr int64_t kWaitMinNs = 20000;
constexpr int64_t kWaitSpinMaxNs = 30000;
std::atomic<int64_t> g_wait_late{kWaitLateStartNs};
GaugeHook gauge_wait_late("kernel.short_wait_late_us", [] { return g_wait_late.load(std::memory_order_relaxed) / 1000; });

bool wait_sleep(int64_t t0, int64_t ns, const uint32_t* token, const std::atomic<uint32_t>* word, int64_t* end) {
    const int64_t deadline = t0 + ns;
    for (int64_t now = t0;;) {
        const int64_t rem = deadline - now;
        if (rem <= 0) {
            *end = now;
            return false;
        }
        if (rem <= kWaitSpinMaxNs) {
            for (;;) {
                const int64_t t = qpc_ns();
                *end = t;
                if (t >= deadline) return false;
                if (word ? word->load(std::memory_order_relaxed) != *token : token && detail::early_wake_passed(*token)) return true;
                YieldProcessor();
            }
        }
        const int64_t late = g_wait_late.load(std::memory_order_relaxed);
        const int64_t ask = std::max(rem - late, kWaitMinNs);
        if (word ? detail::address_wait_word_ns(*word, *token, ask) : detail::address_wait_ns(token, ask)) {
            *end = 0;
            return true;
        }
        const int64_t after = qpc_ns();
        const int64_t next = precise_late_update(late, ask, after - now);   // a wait that came back early (a stale wake) is no sample
        if (next != late) g_wait_late.store(next, std::memory_order_relaxed);
        now = after;
    }
}

// The calling thread's high-resolution waitable timer for ns (> 0); Sleep() in whole milliseconds without one.
void timer_wait(int64_t ns) {
    static thread_local HANDLE timer = nullptr;
    static thread_local bool no_timer = false;
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = true;
    }
    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
    else
        WaitForSingleObject(timer, INFINITE);
}

void sleep_once(int64_t ns) {
    if (ns <= 0) {
        yield_host();
        return;
    }
    // Short sleeps (polls such as Pokopia's GPU-fence wait, 100 us a round) overshoot to about half a millisecond on the timer: yield until the
    // deadline instead. Longer ones keep the timer (no busy waiting while a thread idles).
    // A thread that armed an early wake with a poll just before (nxrt/early_wake.h) is in a poll loop whatever span it asks for: on the
    // address wait its sleep is a short one up to kEarlyWakeMaxNs, and a fire ends it.
    const ShortCfg& cfg = short_cfg();
    uint32_t token = 0;
    const bool armed = early_wake_enabled() && detail::early_wake_take(&token);
    if (ns <= cfg.max_ns || (armed && cfg.mode == detail::ShortSleep::Wait && ns <= kEarlyWakeMaxNs)) {
        const int64_t t0 = qpc_ns();
        int64_t end = 0;   // the wait's last clock reading (Wait mode)
        bool early = false;
        if (cfg.mode == detail::ShortSleep::Wait) {
            early = wait_sleep(t0, ns, armed ? &token : nullptr, nullptr, &end);
        } else if (cfg.mode == detail::ShortSleep::Delay) {
            LARGE_INTEGER span;
            span.QuadPart = -((ns + 99) / 100);             // relative, in 100 ns units
            cfg.delay(FALSE, &span);
        } else {
            LARGE_INTEGER f, s, n;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&s);
            const int64_t ticks = ns * f.QuadPart / 1000000000;
            do {
                yield_host();
                if (armed && detail::early_wake_passed(token)) {
                    early = true;
                    break;
                }
                QueryPerformanceCounter(&n);
            } while (n.QuadPart - s.QuadPart < ticks);
        }
        if (armed) detail::early_wake_note(early);
        if (!early) short_sleep_note(ns, (end ? end : qpc_ns()) - t0);
        return;
    }
    // [sched] precise_sleep: the timer is asked for less by its learned lateness (precise_wait.cpp). Pokopia's main thread waits out each
    // frame in about 900 SleepThread(1 ms) a second; uncorrected, each takes about 1.4-1.5 ms (pokopia-native m4-title: title 59.3 Hz).
    if (precise_sleep_enabled()) {
        precise_wait_ns(ns, nullptr);
        return;
    }
    timer_wait(ns);
}
}  // namespace

detail::ShortSleep detail::short_sleep_mode() { return short_cfg().mode; }

bool detail::sleep_on_word(int64_t t0, int64_t ns, const std::atomic<uint32_t>& word, uint32_t seen, int64_t* end) {
    const bool early = wait_sleep(t0, ns, &seen, &word, end);
    if (!early && ns <= short_cfg().max_ns) short_sleep_note(ns, *end - t0);
    return early;
}

void detail::sleep_timer_ns(int64_t ns) {
    if (ns <= 0 || clock::virtual_sleep(ns)) return;
    timer_wait(ns);
}

// On the thread set_blocked_hook names, a sleep longer than 10 ms is cut into 10 ms slices with the hook run between them, as in the kernel
// waits: Pokopia's main thread sleeps in SleepThread while the software keyboard applet is open (the freeze report's main stack).
void sleep_ns(int64_t ns) {
    if (clock::virtual_sleep(ns)) return;                   // a virtual clock (deterministic test mode): time advances, the thread only yields
    constexpr int64_t kSlice = 10000000;
    void (*fn)() = ns > kSlice ? detail::blocked_hook_here() : nullptr;
    if (!fn) return sleep_once(ns);
    const int64_t until = qpc_ns() + ns;
    for (int64_t rem = ns; rem > 0; rem = until - qpc_ns()) {
        if (rem <= kSlice) return sleep_once(rem);
        sleep_once(kSlice);
        fn();
    }
}

}  // namespace nxrt::kernel
