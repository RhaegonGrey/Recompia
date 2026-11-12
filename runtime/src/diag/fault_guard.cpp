// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt/fault_guard.h: the one guarded call. No C++ object may live in this frame (SEH).
#include "nxrt/fault_guard.h"

#include <windows.h>   // POSIX: the shim's windows.h, which declares nxos_guarded_call (src/os/nx_os.h)

namespace nxrt {

#if defined(_WIN32)
namespace {
int filter(unsigned long code, FaultCatch what) {
    if (what == FaultCatch::Any || code == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

bool fault_guarded_call(void (*fn)(void*), void* ctx, FaultCatch what) {
    __try {
        fn(ctx);
        return true;
    } __except (filter(GetExceptionCode(), what)) {
        return false;
    }
}
#else
bool fault_guarded_call(void (*fn)(void*), void* ctx, FaultCatch) {
    return nxos_guarded_call(fn, ctx) != 0;   // a SIGSEGV/SIGBUS inside fn lands back here
}
#endif

}  // namespace nxrt
