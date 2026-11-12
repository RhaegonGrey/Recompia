// SPDX-License-Identifier: GPL-3.0-or-later
// Early wake for short guest sleeps (src/kernel/early_wake.cpp, [kernel] early_wake).
//
// A guest thread that waits for something the host completes (a GPU submission behind an NVN sync) often polls it and sleeps a short
// time between polls. The sleep then ends on the host's timer, not when the thing completed. With this, the layer that answers the poll
// opts the thread in, and the layer that sees the completion ends those sleeps at once:
//
//     const uint32_t token = kernel::early_wake_token();   // before the poll reads the state
//     if (!ready()) kernel::early_wake_arm(token);          // the poll answers "not yet"
//     ...
//     kernel::early_wake_fire();                            // any thread, when the state the polls read has advanced
//
// The arm covers one SleepThread: the thread's next one, when it starts within kEarlyWakeArmNs of the arm and asks for kEarlyWakeMaxNs or
// less. That sleep ends when a fire came after the token was read (also one that came between the poll and the sleep), or after its own
// time. A thread that did not poll, or polled and went on to other work before it sleeps, sleeps its full time. There is one wake source
// for the process: a fire for something else is a spurious wake, after which the guest's loop polls again and sleeps again. The kernel
// knows nothing about who fires.
#pragma once
#include <cstdint>

namespace nxrt::kernel {

constexpr int64_t kEarlyWakeMaxNs = 2000000;   // the longest SleepThread an arm applies to
constexpr int64_t kEarlyWakeArmNs = 500000;    // how long an arm lasts: a poll loop sleeps right after its poll

// [kernel] early_wake = auto (default) | on | off, read once and logged. auto: on where short sleeps are host waits ([kernel] short_sleep
// resolves to wait: Wine on a handheld); on: also where they are yielded through (the yield loop then ends on a fire); off. Never with
// short_sleep = sleep (NtDelayExecution cannot be ended).
bool early_wake_enabled();
uint32_t early_wake_token();            // any thread: the wake generation
void early_wake_arm(uint32_t token);    // a guest thread, after a poll that answered "not yet"; no-op when off or on a host thread
void early_wake_fire();                 // any thread; cheap when no thread sleeps (one atomic add and one load)

}  // namespace nxrt::kernel
