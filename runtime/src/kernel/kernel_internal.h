// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/kernel: state shared by thread.cpp, kernel_sync.cpp and svc_thread.cpp.
#pragma once
#include <windows.h>

#include "nxrt/kernel.h"

namespace nxrt::kernel::detail {

static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*), "KThread::cv_storage holds one CONDITION_VARIABLE");
inline CONDITION_VARIABLE* thread_cv(KThread& t) { return reinterpret_cast<CONDITION_VARIABLE*>(&t.cv_storage[0]); }

void sync_reset();                       // forget every wait queue (kernel_sync.cpp)
void thread_mark_exited(KThread& t);     // marks exited under the kernel lock and wakes WaitSynchronization waiters (kernel_sync.cpp)
extern SRWLOCK g_k;                      // the "kernel lock": wait queues and wait state of every thread
// The set_blocked_hook function when the calling thread is the one it names, else null (kernel_sync.cpp).
void (*blocked_hook_here())();
// The sleep of the yield back-off: always the high-resolution timer, never the yield loop sleep_ns uses for spans up to
// 200 us (time.cpp). Windows wakes it ~0.5 ms late at the shortest; Wine/Proton sleeps about the span asked for.
void sleep_timer_ns(int64_t ns);
const ProcessHooks& process_hooks();     // what set_process_hooks gave (svc_info.cpp)
std::string state_report();              // process_hooks().state_report(), or a note that there is none

}  // namespace nxrt::kernel::detail
