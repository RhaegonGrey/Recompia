// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime census ([diag] hle_census, default off): how much of each guest thread's time is spent inside the runtime, and through which
// door. Lifted code enters the host at a few choke points: the SVC dispatcher (kernel/svc.cpp svc_dispatch), the HLE thunks
// (core/dispatch.cpp dispatch_call for thunk addresses, core/abi_glue.cpp host_import for leaf thunks) and the lifter's hook sites
// (core/hooks.cpp). Each wraps its work in a census scope when the census is on; off, each pays one never-taken branch on a plain bool.
//
// A scope adds its EXCLUSIVE time (TSC ticks, less the time of the scopes nested in it) and one call to its entry point, in a block of
// the calling thread's own (plain stores, no cache line shared with another thread). So an HLE function that makes an SVC is charged
// without the SVC, and the SVC with its own time. Lifted code that a host function calls back into (a guest callback from a thunk or a
// hook) is not a scope: it counts as time of the host function that called it.
//
// Every 10 s, one line per thread that used more than 1 % of a processor (tag "hcensus"), see hle_census.cpp.
#pragma once
#include <cstdint>

namespace nxrt::diag::census {

extern bool g_on;                         // set once by init() before any guest code runs; never changes afterwards
inline bool on() { return g_on; }

void init();                              // reads [diag] hle_census (svc_init); calibrates the TSC when on

// Entry points: a kind and an index.
enum Kind : uint32_t { kSvc = 0, kHle = 1, kSite = 2, kHook = 3 };
constexpr uint32_t key(Kind k, uint32_t index) { return static_cast<uint32_t>(k) << 28 | (index & 0x0fffffffu); }

void enter(uint32_t key);
void leave();

struct Scope {
    explicit Scope(uint32_t k) { enter(k); }
    ~Scope() { leave(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// A longjmp across scopes (the C++ exception landing, eh/catch_call.cpp) skips their ends: the catch frame takes the depth before it
// runs the body and puts it back when it lands. 0 when the census is off.
uint32_t depth_now();
void restore_to(uint32_t depth);
inline uint32_t depth() { return g_on ? depth_now() : 0; }
inline void restore(uint32_t depth) {
    if (g_on) restore_to(depth);
}

// A hook's site address, for the report's name of kHook entries (hooks.cpp, before the hook's first scope).
void note_hook(uint32_t id, uint64_t addr, uint32_t kind);

}  // namespace nxrt::diag::census
