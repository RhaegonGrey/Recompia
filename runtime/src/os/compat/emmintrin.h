// SPDX-License-Identifier: GPL-3.0-or-later
// <emmintrin.h> for the POSIX build: _mm_pause (src/gfx/frame_limiter.cpp spins with it), plus the compiler's own x86 header
// where there is one. The real <immintrin.h> includes <emmintrin.h> for __m128i, and this file shadows it, so without the
// forward the SSE types would be missing from every x86-64 build (nvn_shaders.cpp's SHA-NI path).
#pragma once
#include "intrin.h"

#if defined(__x86_64__) || defined(__i386__)
#include_next <emmintrin.h>
#endif
