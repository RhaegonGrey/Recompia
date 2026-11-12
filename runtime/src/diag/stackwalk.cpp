// SPDX-License-Identifier: GPL-3.0-or-later
// Pattern from smo-recomp runtime/core/hostrt.c crash_filter (RtlVirtualUnwind loop).
//
// Windows walks the stack with the unwind information RtlLookupFunctionEntry finds for each return address; there is no
// such table on macOS, and arm64 makes the frame pointer chain a first-class thing (the record is {saved fp, saved lr}
// at fp, and the ABI reserves x29 for it), so the walk follows that instead. The same sanity checks lldb applies keep
// a corrupted or absent chain from running off into unmapped memory: 16-byte aligned, at least one frame above sp,
// monotonically increasing, and within a bounded window of the stack.
#include "stackwalk.h"

namespace nxrt::detail {

#if defined(_WIN32)

size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max) {
    CONTEXT c = *ctx;
    size_t n = 0;
    while (n < max && c.Rip) {
        frames[n++] = c.Rip;
        DWORD64 image_base = 0;
        RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!rf) {  // leaf function without unwind info: the return address is at [rsp]
            if (c.Rsp < 0x10000) break;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            if (n > 1) break;   // only the first frame may be a leaf; anything else means we lost the chain
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
    }
    return n;
}

#else

size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max) {
    if (!ctx || !frames || max == 0) return 0;
    size_t n = 0;
    frames[n++] = ctx->pc;
    uint64_t sp = ctx->sp;
    uint64_t fp = ctx->fp;
    while (n < max) {
        // fp must sit above sp, stay aligned, and stay inside a plausible stack window. A frame record is two words.
        if ((fp & 15) != 0 || fp < sp + 16 || fp - sp > (8ull << 20)) break;
        const auto* record = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(fp));
        const uint64_t next_fp = record[0];
        const uint64_t lr = record[1];
        if (!lr) break;
        frames[n++] = lr;
        if (next_fp <= fp) break;   // no progress: the chain is not one
        sp = fp + 16;
        fp = next_fp;
    }
    return n;
}

#endif

}  // namespace nxrt::detail
