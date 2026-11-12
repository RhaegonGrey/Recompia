// SPDX-License-Identifier: GPL-3.0-or-later
// <immintrin.h> for the POSIX build: the same subset as intrin.h, plus the compiler's own x86 header where there is one.
// The SHA-NI path in src/gfx/nvn/nvn_shaders.cpp is x86-64 only and needs __m128i and the _mm_sha1* intrinsics; on a Linux
// x86-64 build those come from the compiler's header, which this file shadows (src/os/compat precedes the system headers on
// the include path). Elsewhere -- arm64 macOS -- the compiler has no such header and only the pause hint is used.
#pragma once
#include "intrin.h"

#if defined(__x86_64__) || defined(__i386__)
#include_next <immintrin.h>
#endif
