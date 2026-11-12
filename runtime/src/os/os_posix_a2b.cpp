// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_a2b.cpp: astc2bc7 for the POSIX builds (macOS, Linux).
//
// The Windows transcoder is an x86 build: the ASTC decode side is ARM astcenc compiled once per ISA into
// astcdec-{avx2,sse4.1,sse2} shared libraries and picked at run time by CPUID, and the BC7 encode side is bc7e.ispc emitted for
// `--target=sse4-i32x4,avx2-i32x8,avx512skx-x16 --arch=x86-64` (tools/textures/CMakeLists.txt:15 pins the Windows ISPC). Neither
// artifact exists for the POSIX builds, so this file provides the same entry points on top of what those builds can compile:
//
//   * ASTC decode: ARM astcenc (Apache-2.0), statically linked decompressor-only on Linux (runtime/CMakeLists.txt). macOS has no
//     decoder at all and does not need one: Metal samples ASTC natively, so nvn_texdata.cpp takes Mode::Passthrough for every ASTC
//     texture when the device reports the format (docs/macos-port.md, D4), and nothing ever asks for a transcode there.
//   * BC7 encode: bc7enc (MIT, third_party/bc7enc). Lower quality than bc7e's ISPC kernels at equal speed, and the effort knobs are
//     its own (partitions per block, uber level), so a2b_options::quality is mapped onto those instead of bc7e's presets.
//
// The size/format helpers are the same formulas as tools/textures/src/a2b.cpp. On macOS the three kernel entry points still answer
// A2B_ERR_UNSUPPORTED (honest failure, which the callers log) rather than uploading garbage.
#include "astc2bc7.h"

#if !defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <thread>
#include <vector>

#include "bc7enc.h"   // third_party/bc7enc: the BC7 encoder the texture patch needs (MIT)

#if defined(NXRT_HAVE_ASTCENC)
#include "astcenc.h"   // tools/textures/third_party/astc-encoder: the ASTC decoder (Apache-2.0)
#endif

namespace {

uint64_t div_up(uint64_t v, uint64_t d) { return (v + d - 1) / d; }

int worker_count(int threads, uint32_t bands) {
    if (threads <= 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return static_cast<int>(std::min<size_t>(static_cast<size_t>(threads), std::max(1u, bands)));
}

// Splits [0, bands) between `threads` workers (0: one per hardware thread). fn(first, last) is called once per worker, with the
// calling thread taking the first band, so a single-threaded request runs inline with no thread created at all.
template <typename Fn>
void parallel_bands(uint32_t bands, int threads, Fn&& fn) {
    const int n = worker_count(threads, bands);
    if (n <= 1) {
        fn(0u, bands);
        return;
    }
    const auto cut = [bands, n](int t) { return static_cast<uint32_t>(static_cast<uint64_t>(bands) * t / n); };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n) - 1);
    for (int t = 1; t < n; ++t) {
        const uint32_t y0 = cut(t), y1 = cut(t + 1);
        pool.emplace_back([&fn, y0, y1] { fn(y0, y1); });
    }
    fn(0u, cut(1));
    for (std::thread& th : pool) th.join();
}

void bc7enc_init_once() {
    static std::once_flag once;
    std::call_once(once, [] { bc7enc_compress_block_init(); });
}

// One 4x4 block per BC7 block, edges clamped (BC7 blocks are independent, so the split across workers cannot change the output).
void encode_bc7_blocks(uint32_t w, uint32_t h, const uint8_t* rgba, const bc7enc_compress_block_params& params, uint8_t* out,
                       int threads) {
    const uint32_t bx = static_cast<uint32_t>(div_up(w, 4)), by = static_cast<uint32_t>(div_up(h, 4));
    parallel_bands(by, threads, [&](uint32_t y0, uint32_t y1) {
        uint8_t block[64];
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < bx; ++x) {
                for (uint32_t py = 0; py < 4; ++py)
                    for (uint32_t px = 0; px < 4; ++px) {
                        const uint32_t sx = std::min(w - 1, x * 4 + px), sy = std::min(h - 1, y * 4 + py);
                        std::memcpy(block + (py * 4 + px) * 4, rgba + (static_cast<size_t>(sy) * w + sx) * 4, 4);
                    }
                bc7enc_compress_block(out + (static_cast<size_t>(y) * bx + x) * 16, block, &params);
            }
    });
}

// a2b_options::quality (bc7e presets 0..6) mapped onto bc7enc's search effort. bc7enc tops out at uber level 4; its defaults
// (partitions 64, uber 0, least squares on) are the "slow" preset, so 0..3 are the ones that actually trade quality for speed.
bc7enc_compress_block_params params_for_quality(int quality, int perceptual) {
    bc7enc_compress_block_params p;
    bc7enc_compress_block_params_init(&p);
    if (perceptual)
        bc7enc_compress_block_params_init_perceptual_weights(&p);
    else
        bc7enc_compress_block_params_init_linear_weights(&p);
    switch (std::clamp(quality < 0 ? A2B_DEFAULT_QUALITY : quality, 0, 6)) {
        case 0:   // ultrafast: mode 6 only, no partition search (the texture-patch setting)
            p.m_max_partitions_mode = 0;
            p.m_try_least_squares = BC7ENC_FALSE;
            p.m_uber_level = 0;
            break;
        case 1:   // veryfast
            p.m_max_partitions_mode = 0;
            p.m_try_least_squares = BC7ENC_FALSE;
            p.m_uber_level = 1;
            break;
        case 2: p.m_max_partitions_mode = 8; p.m_uber_level = 1; break;
        case 3: p.m_max_partitions_mode = 32; p.m_uber_level = 2; break;
        case 4: p.m_max_partitions_mode = 64; p.m_uber_level = 2; p.m_mode_partition_estimation_filterbank = BC7ENC_FALSE; break;
        case 5: p.m_max_partitions_mode = 64; p.m_uber_level = 3; p.m_mode_partition_estimation_filterbank = BC7ENC_FALSE; break;
        default: p.m_max_partitions_mode = 64; p.m_uber_level = 4; p.m_mode_partition_estimation_filterbank = BC7ENC_FALSE; break;
    }
    return p;
}

double now_s() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

}  // namespace

extern "C" {

a2b_status a2b_init(const char* dll_dir) {
    (void)dll_dir;   // nothing to locate: no per-ISA decoder libraries on this platform
#if defined(NXRT_HAVE_ASTCENC)
    return A2B_OK;
#else
    return A2B_ERR_UNSUPPORTED;
#endif
}

const char* a2b_status_string(a2b_status s) {
    switch (s) {
        case A2B_OK: return "ok";
        case A2B_ERR_ARGS: return "bad arguments";
        case A2B_ERR_SIZE: return "buffer size does not match dimensions";
        case A2B_ERR_UNSUPPORTED:
#if defined(NXRT_HAVE_ASTCENC)
            return "unsupported format (HDR ASTC or a bad block size)";
#else
            return "unsupported format (no ASTC transcoder on this platform: Metal samples ASTC)";
#endif
        case A2B_ERR_NO_ASTC_LIB: return "astcdec-*-shared.dll not found";
        case A2B_ERR_DECODE: return "astcenc decode error";
    }
    return "?";
}

int a2b_is_astc(a2b_format f) { return f == A2B_ASTC_UNORM || f == A2B_ASTC_SRGB; }

int a2b_is_srgb(a2b_format f) {
    return f == A2B_ASTC_SRGB || f == A2B_BC1_SRGB || f == A2B_BC2_SRGB || f == A2B_BC3_SRGB || f == A2B_BC7_SRGB;
}

uint32_t a2b_bcn_block_bytes(a2b_format f) {
    switch (f) {
        case A2B_BC1_UNORM:
        case A2B_BC1_SRGB:
        case A2B_BC4_UNORM:
        case A2B_BC4_SNORM: return 8;
        case A2B_BC2_UNORM:
        case A2B_BC2_SRGB:
        case A2B_BC3_UNORM:
        case A2B_BC3_SRGB:
        case A2B_BC5_UNORM:
        case A2B_BC5_SNORM:
        case A2B_BC6H_UF16:
        case A2B_BC6H_SF16:
        case A2B_BC7_UNORM:
        case A2B_BC7_SRGB: return 16;
        default: return 0;
    }
}

uint64_t a2b_astc_size(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    if (!w || !h || bw < 4 || bw > 12 || bh < 4 || bh > 12) return 0;
    return div_up(w, bw) * div_up(h, bh) * 16;
}

uint64_t a2b_output_size(const a2b_input* in) {
    if (!in || !in->width || !in->height) return 0;
    if (a2b_is_astc(in->format)) return div_up(in->width, 4) * div_up(in->height, 4) * 16;
    const uint32_t bb = a2b_bcn_block_bytes(in->format);
    return bb ? div_up(in->width, 4) * div_up(in->height, 4) * bb : 0;
}

a2b_status a2b_decode_astc_rgba8(const a2b_input* in, const void* data, size_t data_size, int threads, uint8_t* rgba,
                                 a2b_stats* stats) {
    if (!in || !data || !rgba || !a2b_is_astc(in->format)) return A2B_ERR_ARGS;
    const uint64_t expect = a2b_astc_size(in->width, in->height, in->block_w, in->block_h);
    if (!expect) return A2B_ERR_ARGS;
    if (data_size != expect) return A2B_ERR_SIZE;
#if defined(NXRT_HAVE_ASTCENC)
    astcenc_config cfg;
    const astcenc_profile profile = in->format == A2B_ASTC_SRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    if (astcenc_config_init(profile, in->block_w, in->block_h, 1, 0.0f, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS)
        return A2B_ERR_ARGS;

    const double t0 = now_s();
    // Decompression is a lookup walk over the block stream: one context, and every worker thread calls the codec once with its own
    // thread index (astcenc splits the image between them). No banding needed, the context is what holds the per-thread state.
    const unsigned nthreads = static_cast<unsigned>(worker_count(threads, 16));
    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, nthreads, &ctx, nullptr) != ASTCENC_SUCCESS) return A2B_ERR_DECODE;
    void* slices[1] = {rgba};
    astcenc_image img{in->width, in->height, 1, ASTCENC_TYPE_U8, slices};
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const uint8_t* src = static_cast<const uint8_t*>(data);

    astcenc_error err = ASTCENC_SUCCESS;
    if (nthreads == 1) {
        err = astcenc_decompress_image(ctx, src, data_size, &img, &swz, 0);
    } else {
        std::vector<astcenc_error> errs(nthreads, ASTCENC_SUCCESS);
        std::vector<std::thread> pool;
        pool.reserve(nthreads - 1);
        for (unsigned t = 1; t < nthreads; ++t)
            pool.emplace_back([&, t] { errs[t] = astcenc_decompress_image(ctx, src, data_size, &img, &swz, t); });
        errs[0] = astcenc_decompress_image(ctx, src, data_size, &img, &swz, 0);
        for (std::thread& th : pool) th.join();
        for (astcenc_error e : errs)
            if (e != ASTCENC_SUCCESS) err = e;
    }
    astcenc_context_free(ctx);
    if (stats) {
        stats->decode_seconds = now_s() - t0;
        stats->astc_isa = "sse4.1";
    }
    return err == ASTCENC_SUCCESS ? A2B_OK : A2B_ERR_DECODE;
#else
    (void)data_size;
    (void)threads;
    (void)stats;
    return A2B_ERR_UNSUPPORTED;
#endif
}

a2b_status a2b_encode_bc7(uint32_t w, uint32_t h, const uint8_t* rgba, const a2b_options* opt, void* out, size_t out_size,
                          a2b_stats* stats) {
    if (!rgba || !out || !w || !h) return A2B_ERR_ARGS;
    const uint32_t bx = static_cast<uint32_t>(div_up(w, 4)), by = static_cast<uint32_t>(div_up(h, 4));
    if (out_size < static_cast<size_t>(bx) * by * 16) return A2B_ERR_ARGS;
    // The texture patch (src/gfx/nvn/nvn_tex_patch.cpp, nvn_tex_override.cpp) wants speed, not PSNR: mode 6 only (no partitions),
    // which is what carries a flat-coloured glyph with alpha exactly and keeps an 84x84 prompt rectangle around 20 ms.
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_max_partitions_mode = 0;
    params.m_try_least_squares = BC7ENC_FALSE;
    if (opt && opt->quality < 50) params.m_uber_level = 0;
    bc7enc_init_once();
    const double t0 = now_s();
    encode_bc7_blocks(w, h, rgba, params, static_cast<uint8_t*>(out), opt ? opt->threads : 0);
    if (stats) stats->encode_seconds = now_s() - t0;
    return A2B_OK;
}

a2b_status a2b_transcode(const a2b_input* in, const void* data, size_t data_size, const a2b_options* opt, a2b_format* out_format,
                         void* out, size_t out_capacity, a2b_stats* stats) {
    if (!in || !data || !out) return A2B_ERR_ARGS;
    if (!a2b_is_astc(in->format)) {
        // Block-compressed input passes through byte for byte under its own format.
        const uint32_t bb = a2b_bcn_block_bytes(in->format);
        if (!bb || !in->width || !in->height) return A2B_ERR_UNSUPPORTED;
        const uint64_t size = div_up(in->width, 4) * div_up(in->height, 4) * bb;
        if (data_size != size || out_capacity < size) return A2B_ERR_SIZE;
        std::memcpy(out, data, static_cast<size_t>(size));
        if (out_format) *out_format = in->format;
        return A2B_OK;
    }
#if defined(NXRT_HAVE_ASTCENC)
    const uint64_t bc7_size = div_up(in->width, 4) * div_up(in->height, 4) * 16;
    if (out_capacity < bc7_size) return A2B_ERR_SIZE;
    std::vector<uint8_t> rgba(static_cast<size_t>(in->width) * in->height * 4);
    a2b_status st = a2b_decode_astc_rgba8(in, data, data_size, opt ? opt->threads : 0, rgba.data(), stats);
    if (st != A2B_OK) return st;
    const bc7enc_compress_block_params params = params_for_quality(opt ? opt->quality : -1, opt ? opt->perceptual : 0);
    bc7enc_init_once();
    const double t0 = now_s();
    encode_bc7_blocks(in->width, in->height, rgba.data(), params, static_cast<uint8_t*>(out), opt ? opt->threads : 0);
    if (stats) stats->encode_seconds = now_s() - t0;
    if (out_format) *out_format = in->format == A2B_ASTC_SRGB ? A2B_BC7_SRGB : A2B_BC7_UNORM;
    return A2B_OK;
#else
    (void)data_size;
    (void)opt;
    (void)out_capacity;
    (void)stats;
    if (out_format) *out_format = in->format;
    return A2B_ERR_UNSUPPORTED;
#endif
}

}  // extern "C"

#endif  // !_WIN32
