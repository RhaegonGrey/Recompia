// SPDX-License-Identifier: GPL-3.0-or-later
// The poll wait (poll_wait.h, [kernel] poll_wait): SleepThread calls in the game's polling loops (nxrt/game.h PollWait) end when the
// value the loop polls has changed, at the latest on their time.
#include "poll_wait.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "early_wake_internal.h"
#include "nxrt/clock.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::kernel {

detail::PollWaiting detail::g_poll_waiting;

namespace {
constexpr uint32_t kMaxRows = 8;   // listed call sites served (a thread keeps one bit per row)
constexpr int kSlots = 4;          // sleeps registered at a time; one more sleeps its time, unserved

// A registered sleep. `word` is what its thread sleeps on: 0 = free, else (generation << 2) | state. Only the sleeping thread takes a
// slot (free to Filling), publishes it (Waiting, after the watch is stored) and frees it. A scan turns Waiting into Hit with a
// compare-exchange on the word it read before it read the watch: the generation is new for every sleep, so a hit never lands on a later
// sleep in the same slot, and a watch read while the slot changed hands is dropped by the second look at the word.
enum : uint32_t { kFilling = 1, kWaiting = 2, kHit = 3, kStateMask = 3 };
struct Slot {
    std::atomic<uint32_t> word{0};
    std::atomic<uint64_t> addr{0}, idle{0};
    std::atomic<uint32_t> size{0};
};
Slot g_slots[kSlots];
std::atomic<uint32_t> g_generation{0};

// Rows whose last poll wait on this thread ended early and that have not started a sleep on an idle value since. A loop that sleeps
// again although its value has changed did not act on the change (it could not take a lock, say): until it has, its sleeps run their
// time, as they did without the poll wait, so the loop never turns into a spin.
thread_local uint32_t t_after_early = 0;

struct Stats {
    std::atomic<uint64_t> early{0};      // ended by a scan while it slept
    std::atomic<uint64_t> at_once{0};    // the value had changed between the loop's poll and the sleep: no sleep
    std::atomic<uint64_t> ran{0};        // ran its time
    std::atomic<uint64_t> held{0};       // changed at the start, but the loop had not acted on the change before: slept as usual
    std::atomic<uint64_t> unserved{0};   // no free slot: slept as usual
    std::atomic<uint64_t> saved_ns{0};   // time left of the sleeps that ended early or at once
};
Stats g_stats[kMaxRows];

struct Cfg {
    const game::PollWait* rows = nullptr;
    uint32_t n = 0;
    uint64_t base = 0;
    bool census = false;   // [diag] poll_wait_census
};
const Cfg& cfg() {
    static const Cfg k = [] {
        Cfg r;
        const game::Info& g = game::info();
        if (!g.poll_waits || !g.poll_wait_count) return r;
        const bool want = config_get_bool("kernel", "poll_wait", true);
        if (!want || detail::short_sleep_mode() != detail::ShortSleep::Wait) {
            NXLOG_I("kernel", "poll wait off ([kernel] poll_wait = %s%s)", want ? "true" : "false",
                    want ? ": needs short sleeps on the timed address wait, [kernel] short_sleep = wait" : "");
            return r;
        }
        r.rows = g.poll_waits;
        r.n = std::min(g.poll_wait_count, kMaxRows);
        r.base = g.main_base;
        r.census = config_get_bool("diag", "poll_wait_census", false);
        NXLOG_I("kernel", "poll wait on ([kernel] poll_wait = true): %u SleepThread calls in the game's polling loops, of %lld us or less, end when the value "
                "their loop polls has changed (seen at the next SVC of any guest thread), at the latest on their time",
                r.n, static_cast<long long>(kPollWaitMaxNs / 1000));
        return r;
    }();
    return k;
}

bool changed(uint64_t addr, uint64_t idle, uint32_t size) {
    if (size == 8) {
        uint64_t v;
        std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof v);
        return v != idle;
    }
    uint32_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof v);
    return v != static_cast<uint32_t>(idle);
}

// A free slot with w stored and published; *waiting is its word while nothing has happened. Null when all are taken.
Slot* claim(const game::PollWatch& w, uint32_t* waiting) {
    const uint32_t gen = (g_generation.fetch_add(1, std::memory_order_relaxed) + 1) << 2;
    for (Slot& s : g_slots) {
        uint32_t free_word = 0;
        if (!s.word.compare_exchange_strong(free_word, gen | kFilling)) continue;
        s.addr.store(w.addr, std::memory_order_relaxed);
        s.idle.store(w.idle, std::memory_order_relaxed);
        s.size.store(w.size, std::memory_order_relaxed);
        *waiting = gen | kWaiting;
        s.word.store(*waiting);
        detail::g_poll_waiting.n.fetch_add(1);
        return &s;
    }
    return nullptr;
}

void release(Slot& s) {
    detail::g_poll_waiting.n.fetch_sub(1);
    s.word.store(0);
}

uint64_t sum(std::atomic<uint64_t> Stats::* field) {
    uint64_t n = 0;
    for (const Stats& s : g_stats) n += (s.*field).load(std::memory_order_relaxed);
    return n;
}
GaugeHook gauge_early("kernel.poll_wait.ended_early", [] { return static_cast<int64_t>(sum(&Stats::early) + sum(&Stats::at_once)); });
GaugeHook gauge_ran("kernel.poll_wait.ran_time", [] { return static_cast<int64_t>(sum(&Stats::ran) + sum(&Stats::held) + sum(&Stats::unserved)); });

// [diag] poll_wait_census (default off): every 10 s, at the end of a poll wait, one line with what the sleeps of each listed call site
// did since the previous line.
constexpr uint64_t kCensusPeriodNs = 10000000000ull;
std::atomic<uint64_t> g_census_next{0};
std::mutex g_census_lock;
struct CensusRow {
    uint64_t early, at_once, ran, held, unserved, saved_ns;
};
CensusRow g_census_prev[kMaxRows] = {};   // under g_census_lock
uint64_t g_census_t0 = 0;                 // under g_census_lock
void census(const Cfg& k, uint64_t now) {
    if (now < g_census_next.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(g_census_lock, std::try_to_lock);
    if (!lk.owns_lock() || now < g_census_next.load(std::memory_order_relaxed)) return;
    g_census_next.store(now + kCensusPeriodNs, std::memory_order_relaxed);
    std::string line;
    for (uint32_t i = 0; i < k.n; ++i) {
        const Stats& s = g_stats[i];
        const CensusRow cur{s.early.load(std::memory_order_relaxed), s.at_once.load(std::memory_order_relaxed), s.ran.load(std::memory_order_relaxed),
                            s.held.load(std::memory_order_relaxed), s.unserved.load(std::memory_order_relaxed), s.saved_ns.load(std::memory_order_relaxed)};
        const CensusRow p = g_census_prev[i];
        g_census_prev[i] = cur;
        const uint64_t early = cur.early - p.early, at_once = cur.at_once - p.at_once, ran = cur.ran - p.ran, held = cur.held - p.held,
                       unserved = cur.unserved - p.unserved, ended = early + at_once;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s+0x%llx: %llu sleeps, %llu ended early and %llu at once (%.2f ms before their time on average), %llu ran their time, "
                      "%llu held, %llu unserved", i ? "; " : "", static_cast<unsigned long long>(k.rows[i].ret),
                      static_cast<unsigned long long>(ended + ran + held + unserved), static_cast<unsigned long long>(early),
                      static_cast<unsigned long long>(at_once), ended ? static_cast<double>(cur.saved_ns - p.saved_ns) / static_cast<double>(ended) / 1e6 : 0.0,
                      static_cast<unsigned long long>(ran), static_cast<unsigned long long>(held), static_cast<unsigned long long>(unserved));
        line += buf;
    }
    const uint64_t t0 = g_census_t0;
    g_census_t0 = now;
    if (!t0) return;   // the first call only takes the baseline
    NXLOG_I("kernel", "poll waits over %.1f s, by call site in main: %s", static_cast<double>(now - t0) / 1e9, line.c_str());
}
}  // namespace

void detail::poll_wait_scan() {
    for (Slot& s : g_slots) {
        uint32_t seen = s.word.load();
        if ((seen & kStateMask) != kWaiting) continue;
        const uint64_t addr = s.addr.load(std::memory_order_relaxed), idle = s.idle.load(std::memory_order_relaxed);
        const uint32_t size = s.size.load(std::memory_order_relaxed);
        if (s.word.load() != seen) continue;   // the slot changed hands while its watch was read
        if (!changed(addr, idle, size)) continue;
        if (s.word.compare_exchange_strong(seen, (seen & ~kStateMask) | kHit)) address_wake_all(s.word);
    }
}

bool poll_wait_sleep(const ::Ctx& c, int64_t ns) {
    const Cfg& k = cfg();
    if (!k.n || ns > kPollWaitMaxNs) return false;
    const uint64_t lr = c.x[30];   // nn::os::SleepThread is a leaf that ends in the svc: lr is the game's call site
    uint32_t row = 0;
    while (row < k.n && k.base + k.rows[row].ret != lr) ++row;
    if (row == k.n || clock::is_virtual()) return false;
    game::PollWatch w;
    if (!k.rows[row].watch || !k.rows[row].watch(c, &w) || !w.addr || (w.size != 4 && w.size != 8)) return false;

    Stats& st = g_stats[row];
    const uint32_t bit = 1u << row;
    // The clock is read once before the sleep and once after only when the sleep did not end on a reading of its own (sleep_on_word).
    int64_t t0 = 0, end = 0;
    bool early = false, slept = false;
    if (changed(w.addr, w.idle, w.size)) {
        if (t_after_early & bit) {
            st.held.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        early = true;
    } else {
        t_after_early &= ~bit;
        uint32_t waiting = 0;
        Slot* s = claim(w, &waiting);
        if (!s) {
            st.unserved.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // The value may have changed while the slot was taken, with no scan since: looked at once more before sleeping.
        early = changed(w.addr, w.idle, w.size);
        if (!early) {
            t0 = static_cast<int64_t>(qpc::now_ns());
            early = detail::sleep_on_word(t0, ns, s->word, waiting, &end);
        }
        release(*s);
        slept = true;
    }
    uint32_t arm = 0;
    detail::early_wake_take(&arm);   // an early-wake arm of this thread was for this sleep: used up, as SleepThread does
    const uint64_t t1 = end ? static_cast<uint64_t>(end) : qpc::now_ns();
    if (!t0) t0 = static_cast<int64_t>(t1);   // ended at once: none of its time was slept
    if (early) {
        t_after_early |= bit;
        (slept ? st.early : st.at_once).fetch_add(1, std::memory_order_relaxed);
        const uint64_t due = static_cast<uint64_t>(t0 + ns);
        if (due > t1) st.saved_ns.fetch_add(due - t1, std::memory_order_relaxed);
    } else {
        st.ran.fetch_add(1, std::memory_order_relaxed);
    }
    if (k.census) census(k, t1);
    return true;
}

}  // namespace nxrt::kernel
