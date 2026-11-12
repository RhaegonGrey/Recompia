// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: host stack capture without allocation (safe to call while another thread is suspended).
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

// The instruction pointer of a CONTEXT: Rip on x64, pc on arm64. The field name is the only difference, and every
// diag path that reads it (sampler.cpp, watchdog.cpp) needs the same one.
#if defined(_M_X64)
#define NX_CONTEXT_PC(c) ((c).Rip)   // the Win32 x64 CONTEXT
#else
#define NX_CONTEXT_PC(c) ((c).pc)    // the arm64 CONTEXT, on Windows and on the POSIX shim alike
#endif

namespace nxrt::detail {

// Walks the host stack described by *ctx. Windows uses RtlLookupFunctionEntry/RtlVirtualUnwind, macOS the arm64 frame
// pointer chain (stackwalk.cpp). Writes up to `max` return addresses (the first is the current pc). Does not modify
// *ctx. Never allocates.
size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max);

}  // namespace nxrt::detail
