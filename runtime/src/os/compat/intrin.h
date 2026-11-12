// SPDX-License-Identifier: GPL-3.0-or-later
// <intrin.h> for the POSIX build: the handful of intrinsics the runtime uses.
//
// YieldProcessor/_mm_pause are the spin hint (kernel/busy_mutex.cpp, kernel/precise_wait.cpp): `yield` on arm64.
// __rdtsc is the cycle counter diag/ftrace.cpp stamps events with: mach_absolute_time, which is the same kind of
// thing (a monotonic tick counter, not a cycle count). __cpuid has no arm64 counterpart and is defined to zero the
// registers, so a caller that probes for an x86 feature sees "no" -- which is the correct answer here.
#pragma once
#include "nx_os.h"

#if defined(__aarch64__) || defined(__arm64__)
static inline void nxos_spin_hint(void) { __asm__ __volatile__("yield" ::: "memory"); }
#else
static inline void nxos_spin_hint(void) { __asm__ __volatile__("pause" ::: "memory"); }
#endif

static inline void YieldProcessor(void) { nxos_spin_hint(); }
static inline void _ReadWriteBarrier(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }

#if defined(__x86_64__) || defined(__i386__)
// The compiler's own headers declare these, with the target attribute each instruction needs. Including them keeps the Linux
// x86-64 build on the same names as a native one instead of a second definition (ours were redefinitions of GCC's).
#include_next <xmmintrin.h>    // _mm_pause
#include_next <emmintrin.h>   // _mm_mfence, _mm_sfence, _mm_lfence
#include_next <x86gprintrin.h>   // __rdtsc (ia32intrin.h refuses direct inclusion)
#else
static inline void _mm_pause(void) { nxos_spin_hint(); }
static inline void _mm_mfence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void _mm_sfence(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }
static inline void _mm_lfence(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }

static inline unsigned long long __rdtsc(void) {
#if defined(__APPLE__)
    extern unsigned long long mach_absolute_time(void);
    return mach_absolute_time();
#else
    unsigned long long t;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#endif
}
#endif

// x86-64: the real instruction, so a feature probe (nvn_shaders.cpp) answers for this CPU. Elsewhere the registers read
// zero, so a caller probing for an x86 feature sees "no" -- which is the correct answer there.
#if defined(__x86_64__) || defined(__i386__)
static inline void nxos_cpuid(int out[4], int leaf, int sub) {
    __asm__ __volatile__("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3]) : "a"(leaf), "c"(sub));
}
#else
static inline void nxos_cpuid(int out[4], int, int) { out[0] = out[1] = out[2] = out[3] = 0; }
#endif

static inline void __cpuid(int out[4], int leaf) { nxos_cpuid(out, leaf, 0); }

static inline void __cpuidex(int out[4], int leaf, int sub) { nxos_cpuid(out, leaf, sub); }

static inline unsigned char _BitScanForward(unsigned long* index, unsigned long mask) {
    if (!mask) return 0;
    *index = static_cast<unsigned long>(__builtin_ctzl(mask));
    return 1;
}

static inline unsigned char _BitScanReverse(unsigned long* index, unsigned long mask) {
    if (!mask) return 0;
    *index = static_cast<unsigned long>(63 - __builtin_clzl(mask));
    return 1;
}
