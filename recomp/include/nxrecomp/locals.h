// SPDX-License-Identifier: GPL-3.0-or-later
// NZCV in a local of the lifted function (nxrecomp/locals.py). Generated code only; the runtime never sees NxF, so the ABI
// (ctx.h, NXRECOMP_ABI_VERSION) is unchanged. Every flag definition writes the packed N Z C V (exactly as ctx.h's
// adds64 / logic64 / set_nzcv / fcmp do to the Ctx) and the operands of the add, subtract or logic it was; a reader the
// lifter proved to see only one kind of definition calls fx_cc_<kind>, which computes the condition from the operands,
// every other reader fx_cond. Everything is always inlined, so clang turns F into scalars and drops the unread parts.
// Conditions from operands: idea from valentderah/totk-pc-recomp recomp/flags.py (MIT License, see recomp/THIRD_PARTY.md).
#pragma once

#include "ctx.h"

#if defined(_MSC_VER)
#define NX_FX_INLINE __forceinline
#else
#define NX_FX_INLINE inline __attribute__((always_inline))
#endif

struct NxF {
    uint32_t N, Z, C, V;   // packed flags, each 0 or 1
    uint64_t a, b;         // operands of the last add / subtract (b as added: ~x for a subtract) or the logic result (a)
};

// ---- definitions (the packed part mirrors ctx.h) ------------------------------------------------------------------
NX_FX_INLINE uint64_t fx_adds64(NxF& f, uint64_t a, uint64_t b, uint32_t cin) {
    uint64_t r = a + b + cin;
    f.N = uint32_t(r >> 63);
    f.Z = r == 0;
    f.C = (r < a) || (cin && r == a);
    f.V = uint32_t(((a ^ r) & (b ^ r)) >> 63);
    f.a = a, f.b = b;
    return r;
}
NX_FX_INLINE uint32_t fx_adds32(NxF& f, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t wide = uint64_t(a) + b + cin;
    uint32_t r = uint32_t(wide);
    f.N = r >> 31;
    f.Z = r == 0;
    f.C = uint32_t(wide >> 32);
    f.V = ((a ^ r) & (b ^ r)) >> 31;
    f.a = a, f.b = b;
    return r;
}
NX_FX_INLINE uint64_t fx_subs64(NxF& f, uint64_t a, uint64_t b) { return fx_adds64(f, a, ~b, 1); }
NX_FX_INLINE uint32_t fx_subs32(NxF& f, uint32_t a, uint32_t b) { return fx_adds32(f, a, ~b, 1); }
NX_FX_INLINE uint64_t fx_logic64(NxF& f, uint64_t r) { f.N = uint32_t(r >> 63); f.Z = r == 0; f.C = 0; f.V = 0; f.a = r; return r; }
NX_FX_INLINE uint32_t fx_logic32(NxF& f, uint32_t r) { f.N = r >> 31; f.Z = r == 0; f.C = 0; f.V = 0; f.a = r; return r; }
NX_FX_INLINE void fx_set_nzcv(NxF& f, int v) { f.N = (v >> 3) & 1; f.Z = (v >> 2) & 1; f.C = (v >> 1) & 1; f.V = v & 1; }
NX_FX_INLINE uint64_t fx_get_nzcv(const NxF& f) {
    return (uint64_t(f.N) << 31) | (uint64_t(f.Z) << 30) | (uint64_t(f.C) << 29) | (uint64_t(f.V) << 28);
}
template <typename T>
NX_FX_INLINE void fx_fcmp(NxF& f, T a, T b) {   // ctx.h fcmp (double) and cxx.h nx::fcmp<T>: same result
    if (a != a || b != b) fx_set_nzcv(f, 0b0011);
    else if (a == b) fx_set_nzcv(f, 0b0110);
    else if (a < b) fx_set_nzcv(f, 0b1000);
    else fx_set_nzcv(f, 0b0010);
}

// ---- readers --------------------------------------------------------------------------------------------------
// The AArch64 condition from N Z C V (ctx.h cond); cc is always a constant, so the switch folds.
NX_FX_INLINE int fx_cc_nzcv(uint32_t N, uint32_t Z, uint32_t C, uint32_t V, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = Z; break;
    case 1: r = C; break;
    case 2: r = N; break;
    case 3: r = V; break;
    case 4: r = C && !Z; break;
    case 5: r = N == V; break;
    case 6: r = N == V && !Z; break;
    default: return 1;
    }
    return (cc & 1) ? !r : r;
}
NX_FX_INLINE int fx_cond(const NxF& f, int cc) { return fx_cc_nzcv(f.N, f.Z, f.C, f.V, cc); }

// a - x (SUBS / CMP / NEGS): the comparisons themselves
#define NX_FX_SUB(bits, U, S)                                                                                  \
    NX_FX_INLINE int fx_cc_sub##bits(const NxF& f, int cc) {                                                   \
        U a = U(f.a), x = U(~U(f.b)), r = U(a - x);                                                            \
        int v;                                                                                                 \
        switch (cc >> 1) {                                                                                     \
        case 0: v = a == x; break;                                                                             \
        case 1: v = a >= x; break;                                                                             \
        case 2: v = S(r) < 0; break;                                                                           \
        case 3: v = int(U((a ^ x) & (a ^ r)) >> (bits - 1)); break;                                            \
        case 4: v = a > x; break;                                                                              \
        case 5: v = S(a) >= S(x); break;                                                                       \
        case 6: v = S(a) > S(x); break;                                                                        \
        default: return 1;                                                                                     \
        }                                                                                                      \
        return (cc & 1) ? !v : v;                                                                              \
    }
NX_FX_SUB(64, uint64_t, int64_t)
NX_FX_SUB(32, uint32_t, int32_t)
#undef NX_FX_SUB

// a + b (ADDS / CMN, carry in 0)
NX_FX_INLINE int fx_cc_add64(const NxF& f, int cc) {
    uint64_t a = f.a, b = f.b, r = a + b;
    return fx_cc_nzcv(uint32_t(r >> 63), r == 0, r < a, uint32_t(((a ^ r) & (b ^ r)) >> 63), cc);
}
NX_FX_INLINE int fx_cc_add32(const NxF& f, int cc) {
    uint32_t a = uint32_t(f.a), b = uint32_t(f.b), r = a + b;
    return fx_cc_nzcv(r >> 31, r == 0, r < a, ((a ^ r) & (b ^ r)) >> 31, cc);
}

// logic result r (ANDS / TST / BICS): C = V = 0
#define NX_FX_LOGIC(bits, U, S)                                                                                \
    NX_FX_INLINE int fx_cc_logic##bits(const NxF& f, int cc) {                                                 \
        U r = U(f.a);                                                                                          \
        int v;                                                                                                 \
        switch (cc >> 1) {                                                                                     \
        case 0: v = r == 0; break;                                                                             \
        case 1: v = 0; break;                                                                                  \
        case 2: v = S(r) < 0; break;                                                                           \
        case 3: v = 0; break;                                                                                  \
        case 4: v = 0; break;                                                                                  \
        case 5: v = S(r) >= 0; break;                                                                          \
        case 6: v = S(r) > 0; break;                                                                           \
        default: return 1;                                                                                     \
        }                                                                                                      \
        return (cc & 1) ? !v : v;                                                                              \
    }
NX_FX_LOGIC(64, uint64_t, int64_t)
NX_FX_LOGIC(32, uint32_t, int32_t)
#undef NX_FX_LOGIC
