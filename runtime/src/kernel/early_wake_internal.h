// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/kernel: what SleepThread (time.cpp), the early wake (early_wake.cpp) and the poll wait (poll_wait.cpp) need from
// each other.
#pragma once
#include <atomic>
#include <cstdint>

namespace nxrt::kernel::detail {

// How SleepThread spends a short sleep ([kernel] short_sleep resolved for this host; time.cpp, read once).
enum class ShortSleep {
    Yield,   // yielded through until the deadline
    Delay,   // NtDelayExecution
    Wait,    // a timed wait on an address (address_wait_ns)
};
ShortSleep short_sleep_mode();

// The host's wait on an address with a timeout in 100 ns units (ntdll RtlWaitOnAddress / RtlWakeAddressAll). Under Wine it is a futex
// wait inside the process: no wineserver call, and the timeout is the host kernel's (hrtimer), not a millisecond grid.
bool address_wait_available();
// Blocks for ns at most. token null: a plain sleep on a word nobody wakes. token set: ends when the wake generation is no longer *token
// (checked before blocking too); true then. May return early without that (a stale wake): the caller checks its deadline.
bool address_wait_ns(const uint32_t* token, int64_t ns);
// The same wait on a word of the caller's: blocks for ns at most while word holds `seen` (checked before blocking too); true when it no
// longer does. May return early with the word unchanged, as above. Whoever changes the word calls address_wake_all on it.
bool address_wait_word_ns(const std::atomic<uint32_t>& word, uint32_t seen, int64_t ns);
void address_wake_all(const std::atomic<uint32_t>& word);
// One sleep of ns on that wait, as SleepThread's short sleep runs it (the wait's learned lateness taken off, the remainder spun; counted
// with the short sleeps when it ran its time and ns is a short span): true when it ended because word left `seen`. Only with
// short_sleep_mode() == Wait. t0: the start, as qpc::now_ns gives it; *end: the sleep's last clock reading (0 when the word ended it with
// none after the wait), so the caller reads the clock neither before nor after it.
bool sleep_on_word(int64_t t0, int64_t ns, const std::atomic<uint32_t>& word, uint32_t seen, int64_t* end);

// The same wait on any word, for a waiter that has its own (kernel_sync.cpp: a guest thread's park word). Blocks while *word == seen, for
// ns at most (ns < 0: no timeout). False when the timeout ended it. May return true with the word unchanged (a stale wake): the caller
// re-checks its condition.
bool address_wait_word(const std::atomic<uint32_t>* word, uint32_t seen, int64_t ns);
// Wakes the threads waiting on word. It never reads the word: calling it for a word that no longer exists is harmless.
void address_wake_word(const std::atomic<uint32_t>* word);

// SleepThread asks whether the calling thread armed itself just before (early_wake_arm, kEarlyWakeArmNs); the arm is used up either way.
bool early_wake_take(uint32_t* token);
bool early_wake_passed(uint32_t token);   // a fire came since the token was read
void early_wake_note(bool ended_early);   // counts one armed sleep

}  // namespace nxrt::kernel::detail
