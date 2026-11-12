// SPDX-License-Identifier: GPL-3.0-or-later
// Early wake for short guest sleeps (nxrt/early_wake.h): the wake generation, the per-thread arm, and the host's timed wait on an
// address that the early wake, [kernel] short_sleep = wait and the poll wait (poll_wait.cpp) sleep on.
#include <windows.h>

#include <atomic>
#include <string>

#if !defined(_WIN32) && defined(__linux__)
#include <cerrno>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "early_wake_internal.h"
#include "nxrt/config.h"
#include "nxrt/early_wake.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::kernel {

namespace {
// The host's timed wait on a 4-byte word and its wake, as the three callers below need them: wait_word blocks until the word is no longer
// `expected`, for ns at most (ns < 0: no timeout), and answers -ETIMEDOUT when the time ended it. Every word waited on is 4 bytes in
// host memory (the wake generation here, a poll-wait slot, a guest thread's park word), never a guest shared mapping: the Linux form is
// the private futex.
#if defined(_WIN32)
using RtlWaitOnAddressFn = LONG(WINAPI*)(const void*, const void*, SIZE_T, const LARGE_INTEGER*);
using RtlWakeAddressAllFn = void(WINAPI*)(const void*);

struct AddressWait {
    RtlWaitOnAddressFn wait = nullptr;
    RtlWakeAddressAllFn wake_all = nullptr;
};
const AddressWait& address_wait() {
    static const AddressWait a = [] {
        AddressWait r;
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            r.wait = reinterpret_cast<RtlWaitOnAddressFn>(GetProcAddress(ntdll, "RtlWaitOnAddress"));
            r.wake_all = reinterpret_cast<RtlWakeAddressAllFn>(GetProcAddress(ntdll, "RtlWakeAddressAll"));
        }
        if (!r.wait || !r.wake_all) r = {};
        return r;
    }();
    return a;
}
bool wait_available() { return address_wait().wait != nullptr; }
int wait_word(const void* word, uint32_t expected, int64_t ns) {
    LARGE_INTEGER span;
    span.QuadPart = -((ns + 99) / 100);   // relative, in 100 ns units
    const LONG r = address_wait().wait(word, &expected, sizeof expected, ns < 0 ? nullptr : &span);
    return r == 0x102 ? -ETIMEDOUT : 0;   // STATUS_TIMEOUT
}
void wake_word(const void* word) { address_wait().wake_all(word); }
#elif defined(__linux__)
// The futex syscall: the same primitive RtlWaitOnAddress is under Wine, without the ntdll thunk. The kernel compares the word and
// queues the waiter in one step, so a wake that races the wait is never lost.
int wait_word(const void* word, uint32_t expected, int64_t ns) {
    timespec ts;
    timespec* timeout = nullptr;
    if (ns >= 0) {
        ts.tv_sec = static_cast<time_t>(ns / 1000000000);
        ts.tv_nsec = static_cast<long>(ns % 1000000000);
        timeout = &ts;
    }
    const long r = ::syscall(SYS_futex, const_cast<void*>(word), FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0);
    return r == -1 && errno == ETIMEDOUT ? -ETIMEDOUT : 0;
}
void wake_word(const void* word) { ::syscall(SYS_futex, const_cast<void*>(word), FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0); }
bool wait_available() {
    static const bool ok = [] {
        uint32_t word = 0;
        return ::syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 0, nullptr, nullptr, 0) == 0;   // wakes nobody: is there a futex at all
    }();
    return ok;
}
#else
// macOS has no futex, and libSystem's __ulock_wait is the primitive that would stand in for it (docs/macos-optimisations.md). Short
// sleeps keep yielding through there.
int wait_word(const void*, uint32_t, int64_t) { return -ETIMEDOUT; }
void wake_word(const void*) {}
bool wait_available() { return false; }
#endif

std::atomic<uint32_t> g_gen{0};       // the wake generation: the word armed sleeps wait on
std::atomic<int32_t> g_sleepers{0};   // threads inside address_wait_ns on g_gen
std::atomic<uint64_t> g_armed_sleeps{0}, g_ended_early{0};

// The calling thread's arm: the token its poll read, and when it armed (the sleep must start within kEarlyWakeArmNs).
struct Arm {
    uint32_t token = 0;
    uint64_t at_ns = 0;
    bool set = false;
};
thread_local Arm t_arm;

GaugeHook gauge_armed("kernel.early_wake.sleeps", [] { return static_cast<int64_t>(g_armed_sleeps.load(std::memory_order_relaxed)); });
GaugeHook gauge_early("kernel.early_wake.ended_early", [] { return static_cast<int64_t>(g_ended_early.load(std::memory_order_relaxed)); });
}  // namespace

bool detail::address_wait_available() { return wait_available(); }

bool detail::address_wait_ns(const uint32_t* token, int64_t ns) {
    if (!token) {
        static thread_local uint32_t quiet = 0;   // nobody wakes it: the wait ends on its timeout
        wait_word(&quiet, quiet, ns);
        return false;
    }
    if (g_gen.load(std::memory_order_relaxed) != *token) return true;   // a fire came since the token was read: nothing to wait for
    // Counted before the wait compares the word: a fire either sees the sleeper and wakes it, or changed the word before the compare.
    g_sleepers.fetch_add(1);
    wait_word(&g_gen, *token, ns);
    g_sleepers.fetch_sub(1);
    return g_gen.load(std::memory_order_relaxed) != *token;
}

bool detail::address_wait_word(const std::atomic<uint32_t>* word, uint32_t seen, int64_t ns) {
    if (word->load(std::memory_order_relaxed) != seen) return true;
    wait_word(word, seen, ns);
    // Only a changed word is a wake: RtlWaitOnAddress may return spuriously and a futex wait returns EINTR when a signal arrives,
    // and a caller that treats either as "woken" ends a guest's timed sleep early (wait_sleep, time.cpp).
    return word->load(std::memory_order_relaxed) != seen;
}

void detail::address_wake_word(const std::atomic<uint32_t>* word) { wake_word(word); }

bool detail::address_wait_word_ns(const std::atomic<uint32_t>& word, uint32_t seen, int64_t ns) {
    if (word.load(std::memory_order_relaxed) != seen) return true;
    wait_word(&word, seen, ns);
    return word.load(std::memory_order_relaxed) != seen;   // as above: a spurious return is not a wake
}

void detail::address_wake_all(const std::atomic<uint32_t>& word) { wake_word(&word); }

bool early_wake_enabled() {
    static const bool on = [] {
        const std::string m = config_get_str("kernel", "early_wake", "auto");
        const detail::ShortSleep mode = detail::short_sleep_mode();
        const bool want = m == "on" || m == "1" || m == "true" || (m == "auto" && mode == detail::ShortSleep::Wait);
        const bool can = mode != detail::ShortSleep::Delay;
        if (want && can)
            NXLOG_I("kernel", "early wake on ([kernel] early_wake = %s): a SleepThread of %lld us or less right after a poll that armed it ends when what it polled completes (%s)",
                    m.c_str(), static_cast<long long>(kEarlyWakeMaxNs / 1000),
                    mode == detail::ShortSleep::Wait ? "woken from its address wait" : "its yield loop ends; sleeps over the short span keep their timer");
        else
            NXLOG_I("kernel", "early wake off ([kernel] early_wake = %s%s)", m.c_str(),
                    want ? ": short sleeps are NtDelayExecution, which cannot be ended; needs [kernel] short_sleep = wait or yield"
                         : m == "auto" ? ", short sleeps are not host waits here" : "");
        return want && can;
    }();
    return on;
}

uint32_t early_wake_token() { return g_gen.load(); }

void early_wake_arm(uint32_t token) {
    if (!early_wake_enabled()) return;
    if (!current_thread()) return;
    t_arm = {token, qpc::now_ns(), true};
}

void early_wake_fire() {
    g_gen.fetch_add(1);
    if (g_sleepers.load() > 0) wake_word(&g_gen);   // sleepers exist only with the address wait resolved
}

bool detail::early_wake_take(uint32_t* token) {
    if (!t_arm.set) return false;
    t_arm.set = false;
    if (qpc::now_ns() - t_arm.at_ns > static_cast<uint64_t>(kEarlyWakeArmNs)) return false;   // the thread went on to other work: not the poll loop's sleep
    *token = t_arm.token;
    return true;
}

bool detail::early_wake_passed(uint32_t token) { return g_gen.load(std::memory_order_relaxed) != token; }

void detail::early_wake_note(bool ended_early) {
    g_armed_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (ended_early) g_ended_early.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nxrt::kernel
