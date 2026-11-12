// SPDX-License-Identifier: GPL-3.0-or-later
// Guarded calls: run code that touches memory which may go away under it (a guest page the game unmapped or decommitted) and get
// false back instead of a crash. SEH (__try / __except) on Windows, the POSIX shim's guarded call (nxos_guarded_call, a sigsetjmp
// frame the SIGSEGV/SIGBUS handler jumps back to) on Linux and macOS: src/diag/fault_guard.cpp. Vectored handlers (the write
// tracker, the guest fault paths) still see the fault first.
// A fault skips the rest of the call without running destructors, as __except does: keep the guarded code to plain copies, reads
// and calls into functions whose objects live outside it.
#pragma once

#include <type_traits>

namespace nxrt {

enum class FaultCatch {
    AccessViolation,   // memory faults only; on Windows anything else still crashes
    Any,               // every exception (EXCEPTION_EXECUTE_HANDLER); the POSIX shim catches memory faults only, as before
};

// Calls fn(ctx). False when it faulted (of the kind `what`), true when it returned.
bool fault_guarded_call(void (*fn)(void*), void* ctx, FaultCatch what);

namespace fault_guard_detail {
template <class F>
struct Thunk {
    F* fn;
    // p is the Thunk, not the callable: casting it to F* reads `fn` and the words past it as the lambda's captures, and a
    // by-reference lambda then copies to wherever those garbage pointers lead (the land thread's stack top, 2026-11-10).
    static void call(void* p) { (*static_cast<Thunk*>(p)->fn)(); }
};
}  // namespace fault_guard_detail

// The same for a callable: fault_guarded([&] { std::memcpy(d, s, n); }).
template <class F>
bool fault_guarded(F&& fn, FaultCatch what = FaultCatch::AccessViolation) {
    fault_guard_detail::Thunk<std::remove_reference_t<F>> thunk{&fn};
    return fault_guarded_call(&fault_guard_detail::Thunk<std::remove_reference_t<F>>::call, &thunk, what);
}

}  // namespace nxrt
