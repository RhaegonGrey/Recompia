// SPDX-License-Identifier: GPL-3.0-or-later
// Kernel-level synchronisation with the semantics of the Horizon SVCs: ArbitrateLock/Unlock (guest mutex words),
// WaitProcessWideKeyAtomic/SignalProcessWideKey (condition variables), WaitForAddress/SignalToAddress, WaitSynchronization and
// CancelSynchronization. The recompiled SDK's nn::os mutexes and condition variables sit on these.
// Logic ported from SuperMarioWonderRecomp runtime/src/kernel.cpp (MIT, Copyright (c) 2026 BelmanteGu; see THIRD_PARTY_NOTICES.md),
// reworked: one SRWLOCK kernel lock, a wake-up of its own per thread, waiters woken in priority order, a pending
// CancelSynchronization is remembered until the next wait.
//
// [kernel] sync_wait: how a thread sleeps in these waits and how it is woken.
//   cv (default): on its own condition variable with g_k. The waker calls WakeConditionVariable with g_k held, and the woken thread
//     takes g_k again before it returns: a thread woken while the waker still holds g_k sleeps a second time, on the lock, and the
//     waker's release has to wake it again (Wine's SRW lock does not spin).
//   address: on its own park word (KThread::park) with the host's wait on an address (RtlWaitOnAddress, what Wine's condition variable is
//     made of). The waker sets the thread's wake flag and adds one to the word under g_k, and wakes the word after it released g_k
//     (release_k). A thread that finds its wake flag set returns without taking g_k: everything it reads was written before the flag.
//     Only a timeout takes g_k again, to leave the queues, and then checks the flag once more under it.
// Both keep the same queues, the same order of waiters and the same timeouts (whole milliseconds rounded up).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <atomic>
#include <vector>

#include "early_wake_internal.h"
#include "kernel_internal.h"
#include "nxrt/clock.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::kernel {
namespace detail {

SRWLOCK g_k = SRWLOCK_INIT;

namespace {
std::vector<KThread*> g_sync_waiters;    // in WaitSynchronization: woken by any change of waitable state
std::vector<KThread*> g_mutex_waiters;   // waiting for a guest mutex word to be handed over
std::vector<KThread*> g_cv_waiters;      // waiting on a process-wide key
std::vector<KThread*> g_addr_waiters;    // WaitForAddress

// [kernel] sync_wait (above), [kernel] sync_spin_us, [diag] sync_stats; read once, at the first kernel sync call.
// sync_spin_us (default 0 = none, 200 at most; address only): before a thread blocks it watches its park word for that long in a pause
// loop. Off by default: on a machine whose processors are all busy the spin takes time from a thread that has work, and on SMT from the
// sibling thread of its core.
struct SyncCfg {
    bool address = false;
    int64_t spin_ns = 0;
    bool stats = false;
};
const SyncCfg& sync_cfg() {
    static const SyncCfg cfg = [] {
        SyncCfg c;
        const std::string m = config_get_str("kernel", "sync_wait", "cv");
        const bool want = m == "address";
        c.address = want && address_wait_available();
        if (c.address) c.spin_ns = std::clamp<int64_t>(config_get_int("kernel", "sync_spin_us", 0), 0, 200) * 1000;
        c.stats = config_get_bool("diag", "sync_stats", false);
        if (c.address)
            NXLOG_I("kernel", "guest waits: on an address ([kernel] sync_wait = address): woken after the kernel lock is released, a woken thread returns without the lock; spin before blocking %lld us ([kernel] sync_spin_us)",
                    static_cast<long long>(c.spin_ns / 1000));
        else
            NXLOG_I("kernel", "guest waits: on a condition variable with the kernel lock ([kernel] sync_wait = %s%s)", m.c_str(),
                    want ? ": this ntdll has no RtlWaitOnAddress" : "");
        return c;
    }();
    return cfg;
}

// ---- [diag] sync_stats: every 10 s one line with the counts of the period and the time from a wake to the woken thread running ----
enum WaitKind { kWaitMutex, kWaitKey, kWaitAddr, kWaitKinds };   // ArbitrateLock, WaitProcessWideKeyAtomic, WaitForAddress
constexpr const char* kWaitKindName[kWaitKinds] = {"mutex", "key", "address"};
constexpr uint32_t kLatBoundUs[] = {5, 10, 15, 20, 30, 40, 60, 80, 120, 160, 240, 320, 500, 1000, 2000, 5000};
constexpr int kLatBuckets = static_cast<int>(std::size(kLatBoundUs)) + 1;   // the last one: over the last bound
struct SyncStats {
    std::atomic<uint64_t> waits[kWaitKinds]{};                 // waits that blocked (not the ones that returned at once)
    std::atomic<uint64_t> lat[kWaitKinds][kLatBuckets]{};      // wake to running, by kLatBoundUs
    std::atomic<uint64_t> lat_sum_ns[kWaitKinds]{}, lat_max_ns[kWaitKinds]{};
    std::atomic<uint64_t> timeouts{0}, sleeps{0}, spin_hits{0}, wakes{0}, wakes_late{0}, contended{0};
};
SyncStats g_stats;
constexpr uint64_t kStatsPeriodNs = 10000000000ull;
std::atomic<uint64_t> g_stats_next{0};
std::mutex g_stats_lock;
uint64_t g_stats_t0 = 0;   // under g_stats_lock

void stats_count(std::atomic<uint64_t>& n) { n.fetch_add(1, std::memory_order_relaxed); }

// The woken thread, back in its wait: the time since wake_thread set its wake flag.
void stats_latency(WaitKind kind, const KThread& t) {
    const uint64_t now = qpc::now_ns(), ns = now > t.wake_at_ns ? now - t.wake_at_ns : 0;
    int b = 0;
    while (b < kLatBuckets - 1 && ns > static_cast<uint64_t>(kLatBoundUs[b]) * 1000) b++;
    stats_count(g_stats.lat[kind][b]);
    g_stats.lat_sum_ns[kind].fetch_add(ns, std::memory_order_relaxed);
    uint64_t worst = g_stats.lat_max_ns[kind].load(std::memory_order_relaxed);
    while (ns > worst && !g_stats.lat_max_ns[kind].compare_exchange_weak(worst, ns, std::memory_order_relaxed)) {}
}

// Called without g_k, before a kernel sync call takes it. Counts added while the line is built fall into this period or the next.
void stats_tick() {
    const uint64_t now = qpc::now_ns();
    if (now < g_stats_next.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(g_stats_lock, std::try_to_lock);
    if (!lk.owns_lock() || now < g_stats_next.load(std::memory_order_relaxed)) return;
    g_stats_next.store(now + kStatsPeriodNs, std::memory_order_relaxed);
    const uint64_t t0 = g_stats_t0;
    g_stats_t0 = now;
    auto take = [](std::atomic<uint64_t>& n) { return n.exchange(0, std::memory_order_relaxed); };
    if (!t0) {   // the first call starts the first period
        for (int k = 0; k < kWaitKinds; k++) {
            take(g_stats.waits[k]), take(g_stats.lat_sum_ns[k]), take(g_stats.lat_max_ns[k]);
            for (auto& n : g_stats.lat[k]) take(n);
        }
        take(g_stats.timeouts), take(g_stats.sleeps), take(g_stats.spin_hits), take(g_stats.wakes), take(g_stats.wakes_late), take(g_stats.contended);
        return;
    }
    const double secs = static_cast<double>(now - t0) / 1e9;
    std::string line;
    char b[320];
    std::snprintf(b, sizeof b, "sync stats over %.1f s (sync_wait = %s), per second:", secs, sync_cfg().address ? "address" : "cv");
    line += b;
    for (int k = 0; k < kWaitKinds; k++) {
        std::snprintf(b, sizeof b, " %s waits %.0f", kWaitKindName[k], static_cast<double>(take(g_stats.waits[k])) / secs);
        line += b;
    }
    const double timeouts = static_cast<double>(take(g_stats.timeouts)), sleeps = static_cast<double>(take(g_stats.sleeps));
    const double spins = static_cast<double>(take(g_stats.spin_hits)), wakes = static_cast<double>(take(g_stats.wakes));
    const double late = static_cast<double>(take(g_stats.wakes_late)), contended = static_cast<double>(take(g_stats.contended));
    std::snprintf(b, sizeof b, ", timeouts %.0f, host sleeps %.0f, spin hits %.0f, wakes %.0f, park words woken after the lock %.0f, kernel lock found held %.0f; wake to running in us (n p50 p90 p99 max mean):",
        timeouts / secs, sleeps / secs, spins / secs, wakes / secs, late / secs, contended / secs);
    line += b;
    for (int k = 0; k < kWaitKinds; k++) {
        uint64_t h[kLatBuckets], n = 0;
        for (int b = 0; b < kLatBuckets; b++) n += h[b] = take(g_stats.lat[k][b]);
        const double sum = static_cast<double>(take(g_stats.lat_sum_ns[k])), worst = static_cast<double>(take(g_stats.lat_max_ns[k])) / 1e3;
        if (!n) {
            line += std::string(" ") + kWaitKindName[k] + " 0";
            continue;
        }
        // A percentile is the upper bound of the bucket it falls in (the period's longest for the last bucket).
        auto pct = [&](double p) {
            const uint64_t want = static_cast<uint64_t>(p * static_cast<double>(n - 1));
            uint64_t seen = 0;
            for (int b = 0; b < kLatBuckets - 1; b++)
                if ((seen += h[b]) > want) return static_cast<double>(kLatBoundUs[b]);
            return worst;
        };
        std::snprintf(b, sizeof b, " %s %llu %.0f %.0f %.0f %.0f %.1f", kWaitKindName[k], static_cast<unsigned long long>(n), pct(0.50), pct(0.90), pct(0.99), worst,
                      sum / static_cast<double>(n) / 1e3);
        line += b;
    }
    NXLOG_I("kernel", "%s", line.c_str());
}

// ---- the kernel lock ----
// Wake-ups put off until g_k is released (sync_wait = address): the park words kick() changed since g_k was taken. Under g_k.
constexpr int kMaxLateWakes = 32;
const std::atomic<uint32_t>* g_late_wakes[kMaxLateWakes];
int g_late_wake_n = 0;

// Releases g_k, then wakes the park words kick() noted. Every release of g_k after a kick goes through here (thread.cpp and the
// samplers take g_k too, but never kick). A word is only named here, never read: its thread may already be gone.
void release_k() {
    const std::atomic<uint32_t>* words[kMaxLateWakes];
    const int n = g_late_wake_n;
    for (int i = 0; i < n; i++) words[i] = g_late_wakes[i];
    g_late_wake_n = 0;
    ReleaseSRWLockExclusive(&g_k);
    for (int i = 0; i < n; i++) address_wake_word(words[i]);
}

// g_k for one kernel call. A wait may hand it back released (sleep_locked with sync_wait = address): held() says which.
class KLock {
public:
    KLock() {
        if (sync_cfg().stats) stats_tick();
        lock();
    }
    ~KLock() {
        if (held_) unlock();
    }
    KLock(const KLock&) = delete;
    KLock& operator=(const KLock&) = delete;
    void lock() {
        if (!sync_cfg().stats) {
            AcquireSRWLockExclusive(&g_k);
        } else if (!TryAcquireSRWLockExclusive(&g_k)) {
            stats_count(g_stats.contended);
            AcquireSRWLockExclusive(&g_k);
        }
        held_ = true;
    }
    void unlock() {
        held_ = false;
        release_k();
    }
    bool held() const { return held_; }

private:
    bool held_ = false;
};

void remove_from(std::vector<KThread*>& v, KThread* t) { v.erase(std::remove(v.begin(), v.end(), t), v.end()); }

uint32_t load32(uint64_t a) { return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST); }
void store32(uint64_t a, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(a), v, __ATOMIC_SEQ_CST); }
bool cas32(uint64_t a, uint32_t expected, uint32_t v) {
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// The calling guest thread, or a per-host-thread stand-in (tests and host threads that call kernel functions directly).
KThread& self() {
    if (KThread* t = current_thread()) return *t;
    static thread_local KThread host;
    return host;
}

// Highest priority (lowest number) first, FIFO among equals: the order in which Horizon hands a mutex over or signals a key.
KThread* pick_best(std::vector<KThread*>& v, uint64_t KThread::*field, uint64_t value) {
    KThread* best = nullptr;
    for (KThread* t : v)
        if (t->*field == value && (!best || t->prio < best->prio)) best = t;
    return best;
}

// Wakes t's current sleep (g_k held): its condition variable, or its park word (changed here, woken by release_k; at once when more
// than kMaxLateWakes are waiting for the release), and its event while it is in a precise wait's last stretch.
void kick(KThread& t) {
    if (!sync_cfg().address) {
        WakeConditionVariable(thread_cv(t));
    } else {
        t.park.fetch_add(1);
        if (g_late_wake_n < kMaxLateWakes) {
            g_late_wakes[g_late_wake_n++] = &t.park;
            if (sync_cfg().stats) stats_count(g_stats.wakes_late);
        } else {
            address_wake_word(&t.park);
        }
    }
    if (t.precise_event) SetEvent(static_cast<HANDLE>(t.precise_event));
}

// Ends t's wait with result (g_k held). The wake flag is written last: with sync_wait = address t may return as soon as it is set.
void wake_thread(KThread& t, uint32_t result = 0) {
    t.wait_result = result;
    if (sync_cfg().stats) {
        t.wake_at_ns = qpc::now_ns();
        stats_count(g_stats.wakes);
    }
    t.wake = true;
    kick(t);
}

// Hands the mutex word at addr to its best waiter (or clears it). g_k held.
void unlock_mutex(uint64_t addr) {
    KThread* next = pick_best(g_mutex_waiters, &KThread::wait_mutex, addr);
    if (!next) {
        store32(addr, 0);
        return;
    }
    int more = 0;
    for (KThread* t : g_mutex_waiters)
        if (t != next && t->wait_mutex == addr) more++;
    remove_from(g_mutex_waiters, next);
    store32(addr, next->wait_tag | (more ? kHandleWaitMask : 0));
    wake_thread(*next);
}

// Monotonic nanoseconds of the console tick (virtual time under a virtual clock).
int64_t now_ns() { return static_cast<int64_t>(clock::now_ns()); }

// set_blocked_hook: the host thread that runs a hook while it is blocked in a kernel wait, and the hook.
std::atomic<DWORD> g_blocked_tid{0};
std::atomic<void (*)()> g_blocked_fn{nullptr};

// One sleep of t (k held), for ms at most, ended by kick(t). cv: on t's condition variable, back with k held. address: on t's park word,
// read under k so that a kick after the release ends the wait at once; back with k released. On the thread set_blocked_hook names it
// sleeps 10 ms at most and, when that runs out, calls the hook with k released. Every caller loops and re-checks its condition, so a
// wake that comes while the hook runs is not lost.
void sleep_locked(KLock& k, KThread& t, DWORD ms) {
    const SyncCfg& c = sync_cfg();
    if (c.stats) stats_count(g_stats.sleeps);
    void (*fn)() = g_blocked_fn.load(std::memory_order_acquire);
    if (fn && GetCurrentThreadId() != g_blocked_tid.load(std::memory_order_relaxed)) fn = nullptr;
    if (!c.address) {
        if (!fn) {
            SleepConditionVariableSRW(thread_cv(t), &g_k, ms, 0);
        } else if (!SleepConditionVariableSRW(thread_cv(t), &g_k, std::min<DWORD>(ms, 10), 0)) {
            k.unlock();
            fn();
            k.lock();
        }
        return;
    }
    if (fn) ms = std::min<DWORD>(ms, 10);
    const uint32_t seen = t.park.load();
    k.unlock();
    if (c.spin_ns) {
        const uint64_t until = qpc::now_ns() + static_cast<uint64_t>(ms == INFINITE ? c.spin_ns : std::min<int64_t>(c.spin_ns, static_cast<int64_t>(ms) * 1000000));
        do {
            for (int i = 0; i < 16; i++) {
                if (t.park.load(std::memory_order_relaxed) != seen) {
                    if (c.stats) stats_count(g_stats.spin_hits);
                    return;
                }
                YieldProcessor();
            }
        } while (qpc::now_ns() < until);
    }
    if (!address_wait_word(&t.park, seen, ms == INFINITE ? -1 : static_cast<int64_t>(ms) * 1000000) && fn) fn();
}

// One sleep of a timed wait with rem_ns (> 0) left, or forever (rem_ns < 0); k held, back as sleep_locked leaves it. The caller loops and
// re-checks its condition and the deadline. Default: whole ms rounded up, so a 100 us timeout takes 1 ms (2 ms and more at a coarse timer).
// [sched] precise_waits: whole ms rounded down while more than 2.5 ms remain, then the last stretch on the thread's waitable timer with the
// lateness correction (precise_wait.cpp), ended early by kick(). From pokopia-native (MIT), windows/src/nxrt/host/hle_thread.c block @7c3655d
// (their last stretch cannot be woken; here an event is). Not under the deterministic clock (now_ns is then virtual time).
void sleep_rem_locked(KLock& k, KThread& t, int64_t rem_ns) {
    if (rem_ns < 0) return sleep_locked(k, t, INFINITE);
    if (precise_waits_enabled() && !clock::is_virtual()) {
        if (rem_ns > 2500000) return sleep_locked(k, t, static_cast<DWORD>(std::min<int64_t>((rem_ns - 1500000) / 1000000, 0x7FFFFFFE)));
        struct Event {
            HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ~Event() { if (h) CloseHandle(h); }
        };
        static thread_local Event tl_ev;   // closed when the thread ends
        if (const HANDLE ev = tl_ev.h) {
            t.precise_event = ev;
            k.unlock();
            precise_wait_ns(rem_ns, ev);
            k.lock();
            t.precise_event = nullptr;
            ResetEvent(ev);      // a kick that came after the wait ended must not end the next one
            return;
        }
    }
    const int64_t m = (rem_ns + 999999) / 1000000;
    sleep_locked(k, t, static_cast<DWORD>(m > 0x7FFFFFFE ? 0x7FFFFFFE : m));
}

// Waits (k held) until t.wake or the timeout. False on timeout, with k held: the caller takes t out of its queue. True when woken, with
// k held or released (k.held()): the caller then only reads what wake_thread wrote.
bool block_on_wake(KLock& k, KThread& t, int64_t timeout_ns, WaitKind kind) {
    const bool stats = sync_cfg().stats;
    if (stats) stats_count(g_stats.waits[kind]);
    const int64_t until = timeout_ns < 0 ? 0 : now_ns() + timeout_ns;
    while (!t.wake) {
        int64_t rem = -1;
        if (timeout_ns >= 0) {
            rem = until - now_ns();
            if (rem <= 0) {
                if (stats) stats_count(g_stats.timeouts);
                return false;
            }
        }
        sleep_rem_locked(k, t, rem);
        if (k.held()) continue;
        if (t.wake) break;
        k.lock();
    }
    if (stats) stats_latency(kind, t);
    return true;
}

}  // namespace

void thread_mark_exited(KThread& t) {
    KLock k;
    t.exited = true;
    for (KThread* w : g_sync_waiters) kick(*w);
}

void sync_reset() {
    KLock k;
    g_sync_waiters.clear();
    g_mutex_waiters.clear();
    g_cv_waiters.clear();
    g_addr_waiters.clear();
}

}  // namespace detail

using namespace detail;

std::string sync_wait_report() {
    // Called from the watchdog thread while the process may be frozen: never block on the kernel lock.
    if (!TryAcquireSRWLockExclusive(&g_k)) return "kernel sync report: the kernel lock is held (a thread is inside a sync call)\n";
    struct Release { ~Release() { ReleaseSRWLockExclusive(&g_k); } } release;
    std::string out = "kernel sync waiters:\n";
    char b[256];
    auto list = [&](const char* kind, const std::vector<KThread*>& v, uint64_t KThread::*field) {
        for (KThread* t : v) {
            const uint64_t a = t->*field;
            std::snprintf(b, sizeof b, "  %-18s %s (guest %llu) address 0x%llx value 0x%08x tag 0x%x wake %d\n", kind, t->name.c_str(),
                          static_cast<unsigned long long>(t->id), static_cast<unsigned long long>(a), a ? load32(a) : 0u, t->wait_tag, t->wake ? 1 : 0);
            out += b;
        }
    };
    list("WaitForAddress", g_addr_waiters, &KThread::wait_addr);
    list("process-wide key", g_cv_waiters, &KThread::wait_key);
    list("mutex hand-over", g_mutex_waiters, &KThread::wait_mutex);
    std::snprintf(b, sizeof b, "  WaitSynchronization waiters: %zu\n", g_sync_waiters.size());
    out += b;
    return out;
}

uint32_t arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    if (addr & 3) return kInvalidAddress;
    KLock k;
    KThread& me = self();
    if (load32(addr) != (owner | kHandleWaitMask)) return kOk;      // the owner already released it: the caller retries in guest code
    me.wake = false;
    me.wait_mutex = addr;
    me.wait_tag = tag;
    g_mutex_waiters.push_back(&me);
    block_on_wake(k, me, -1, kWaitMutex);
    return me.wait_result;
}

uint32_t arbitrate_unlock(uint64_t addr) {
    if (addr & 3) return kInvalidAddress;
    KLock k;
    unlock_mutex(addr);
    return kOk;
}

uint32_t wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout_ns) {
    if (mutex & 3 || key & 3) return kInvalidAddress;
    KLock k;
    KThread& me = self();
    // Like Horizon: the condition variable's "has waiters" word is written BEFORE the mutex is released. The SDK's Signal/Broadcast
    // read that word in guest code and skip the SVC when it is 0; the other order lost wake-ups (Wonder: a thread took the freed mutex,
    // posted, saw 0, and the waiter then slept forever).
    me.wake = false;
    me.wait_mutex = mutex;
    me.wait_key = key;
    me.wait_tag = tag;
    me.wait_result = 0;
    g_cv_waiters.push_back(&me);
    store32(key, 1);
    unlock_mutex(mutex);
    if (timeout_ns == 0 || !block_on_wake(k, me, timeout_ns, kWaitKey)) {
        // Timed out (not handed the mutex): leave the condition variable or the mutex queue a signal moved us to, return without
        // the mutex (the SDK re-locks it itself: TimedWait -> InternalCriticalSection::Enter).
        remove_from(g_cv_waiters, &me);
        remove_from(g_mutex_waiters, &me);
        bool others = std::any_of(g_cv_waiters.begin(), g_cv_waiters.end(), [&](KThread* t) { return t->wait_key == key; });
        if (!others) store32(key, 0);
        return kTimedOut;
    }
    return me.wait_result;
}

uint32_t signal_process_wide_key(uint64_t key, int32_t count) {
    if (key & 3) return kInvalidAddress;
    KLock k;
    int n = 0;
    while (count <= 0 || n < count) {
        KThread* t = pick_best(g_cv_waiters, &KThread::wait_key, key);
        if (!t) break;
        remove_from(g_cv_waiters, t);
        n++;
        // Try to give it the mutex right away.
        for (;;) {
            uint32_t v = load32(t->wait_mutex);
            if (v == 0) {
                if (!cas32(t->wait_mutex, 0, t->wait_tag)) continue;
                wake_thread(*t);
                break;
            }
            if (!(v & kHandleWaitMask) && !cas32(t->wait_mutex, v, v | kHandleWaitMask)) continue;
            g_mutex_waiters.push_back(t);
            break;
        }
    }
    bool others = std::any_of(g_cv_waiters.begin(), g_cv_waiters.end(), [&](KThread* t) { return t->wait_key == key; });
    if (!others) store32(key, 0);
    return kOk;
}

uint32_t wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout_ns) {
    if (addr & 3) return kInvalidAddress;
    if (type > 2) return kInvalidEnum;
    KLock k;
    KThread& me = self();
    int32_t cur = static_cast<int32_t>(load32(addr));
    switch (type) {
        case 0:  // WaitIfLessThan
            if (!(cur < value)) return kInvalidState;
            break;
        case 1:  // DecrementAndWaitIfLessThan: one atomic step, as the kernel's exclusive load/store loop (guest threads change the word
                 // with their own atomics without the kernel lock; a plain load and store lost their updates)
            for (;;) {
                if (!(cur < value)) return kInvalidState;
                if (cas32(addr, static_cast<uint32_t>(cur), static_cast<uint32_t>(cur) - 1u)) break;
                cur = static_cast<int32_t>(load32(addr));
            }
            break;
        case 2:  // WaitIfEqual
            if (cur != value) return kInvalidState;
            break;
    }
    if (timeout_ns == 0) return kTimedOut;
    me.wake = false;
    me.wait_addr = addr;
    me.wait_result = 0;
    g_addr_waiters.push_back(&me);
    if (!block_on_wake(k, me, timeout_ns, kWaitAddr)) {
        remove_from(g_addr_waiters, &me);
        return kTimedOut;
    }
    return me.wait_result;
}

uint32_t signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    if (addr & 3) return kInvalidAddress;
    if (type > 2) return kInvalidEnum;
    KLock k;
    int waiters = static_cast<int>(std::count_if(g_addr_waiters.begin(), g_addr_waiters.end(), [&](KThread* t) { return t->wait_addr == addr; }));
    switch (type) {
        case 0:  // Signal
            break;
        case 1:  // SignalAndIncrementIfEqual
            if (!cas32(addr, static_cast<uint32_t>(value), static_cast<uint32_t>(value + 1))) return kInvalidState;
            break;
        case 2: {  // SignalAndModifyByWaitingCountIfEqual
            int32_t updated;
            if (count <= 0)
                updated = waiters ? value - 2 : value + 1;
            else if (waiters)
                updated = waiters <= count ? value - 1 : value;
            else
                updated = value + 1;
            if (!cas32(addr, static_cast<uint32_t>(value), static_cast<uint32_t>(updated))) return kInvalidState;
            break;
        }
    }
    int n = 0;
    while (count <= 0 || n < count) {
        KThread* t = pick_best(g_addr_waiters, &KThread::wait_addr, addr);
        if (!t) break;
        remove_from(g_addr_waiters, t);
        wake_thread(*t);
        n++;
    }
    return kOk;
}

uint32_t wait_synchronization(const uint32_t* handles, int32_t count, int64_t timeout_ns, int32_t* index_out) {
    if (count < 0 || count > 0x40) return kOutOfRange;
    std::vector<std::shared_ptr<KObject>> objs;
    for (int i = 0; i < count; i++) {
        auto o = handle_get(handles[i]);
        if (!o) {
            if (index_out) *index_out = -1;
            return kInvalidHandle;
        }
        objs.push_back(std::move(o));
    }
    auto ready = [&]() -> int {
        for (int i = 0; i < count; i++)
            if (objs[i]->signaled()) return i;
        return -1;
    };
    const int64_t until = timeout_ns <= 0 ? 0 : now_ns() + timeout_ns;
    KLock k;
    KThread& me = self();
    g_sync_waiters.push_back(&me);
    struct Leave {
        KThread* t;
        ~Leave() { remove_from(g_sync_waiters, t); }
    } leave{&me};
    for (;;) {
        if (me.cancel_sync) {
            me.cancel_sync = false;
            return kCancelled;
        }
        int i = ready();
        if (i >= 0) {
            if (index_out) *index_out = i;
            objs[i]->consume();
            return kOk;
        }
        if (timeout_ns == 0) return kTimedOut;
        int64_t rem = -1;
        if (timeout_ns > 0) {
            rem = until - now_ns();
            if (rem <= 0) return kTimedOut;
        }
        sleep_rem_locked(k, me, rem);
        if (!k.held()) k.lock();
    }
}

namespace detail {
void (*blocked_hook_here())() {
    void (*fn)() = g_blocked_fn.load(std::memory_order_acquire);
    return fn && GetCurrentThreadId() == g_blocked_tid.load(std::memory_order_relaxed) ? fn : nullptr;
}
}  // namespace detail

void set_blocked_hook(uint32_t host_tid, void (*fn)()) {
    g_blocked_tid.store(host_tid, std::memory_order_relaxed);
    g_blocked_fn.store(fn, std::memory_order_release);
}

uint32_t cancel_synchronization(uint32_t thread_handle) {
    auto t = handle_as<KThread>(thread_handle);
    if (!t) return kInvalidHandle;
    KLock k;
    t->cancel_sync = true;
    if (std::find(g_sync_waiters.begin(), g_sync_waiters.end(), t.get()) != g_sync_waiters.end()) kick(*t);
    return kOk;
}

void notify_waitable_changed() {
    KLock k;
    for (KThread* t : g_sync_waiters) kick(*t);
}

// ---- gauges (include/nxrt/gauge.h): wait-queue lengths ----
namespace {

// Length of one wait queue under the kernel lock. The sampler never waits for the kernel lock: -1 while a thread holds it.
int64_t queue_len(const std::vector<KThread*>& v) {
    if (!TryAcquireSRWLockExclusive(&g_k)) return -1;
    const int64_t n = static_cast<int64_t>(v.size());
    ReleaseSRWLockExclusive(&g_k);
    return n;
}

GaugeHook gauge_wait_sync("kernel.waiters_sync", [] { return queue_len(g_sync_waiters); });
GaugeHook gauge_wait_mutex("kernel.waiters_mutex", [] { return queue_len(g_mutex_waiters); });
GaugeHook gauge_wait_cv("kernel.waiters_cv", [] { return queue_len(g_cv_waiters); });
GaugeHook gauge_wait_addr("kernel.waiters_addr", [] { return queue_len(g_addr_waiters); });
}  // namespace

}  // namespace nxrt::kernel
