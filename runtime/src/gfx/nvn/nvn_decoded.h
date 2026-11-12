// SPDX-License-Identifier: GPL-3.0-or-later
// The "plain data" NVN objects: builders, state objects, samplers and texture views. The game allocates them (at the sizes
// of the real driver) and passes pointers; they never hold a host resource, so each family keeps its fields inside the guest
// object (a private layout, always at most the size of the NVN struct) and the back end reads them through the Info structs
// below. The functions in nvn_decoded.h are implemented next to the NVN entry points of each family (api_*.cpp).
//
// Rules shared by all families:
//   * A getter returns exactly what the matching setter stored; SetDefaults gives the NVN default (documented per family
//     in the family's .cpp, taken from the reference implementations that ran this API on a real game).
//   * decode() never fails: it reads whatever bytes the object holds. A zero-filled object must not crash anything (the
//     decode of it is the "all zero" value of every field).
//   * Every layout starts life from SetDefaults; objects that were not set up are the game's bug.
//   * Enum values are the NVN enum values of third_party/nvn/nvn.h, stored as given (the back end translates them).
#pragma once
#include <cstddef>
#include <cstdint>

#include <nvn/nvn.h>

namespace nxrt::nvn {

// ---- builders --------------------------------------------------------------------------------------------------------
struct DeviceBuilderInfo {
    int flags = 0;
};
DeviceBuilderInfo decode(const NVNdeviceBuilder* b);

struct QueueBuilderInfo {
    NVNdevice* device = nullptr;
    int flags = 0;
    size_t command_memory = 0, compute_memory = 0, control_memory = 0, flush_threshold = 0;
    void* queue_memory = nullptr;
    size_t queue_memory_size = 0;
};
QueueBuilderInfo decode(const NVNqueueBuilder* b);

struct MemoryPoolBuilderInfo {
    NVNdevice* device = nullptr;
    void* storage = nullptr;      // nvnMemoryPoolBuilderSetStorage
    size_t size = 0;
    int flags = 0;                // NVNmemoryPoolFlags
};
MemoryPoolBuilderInfo decode(const NVNmemoryPoolBuilder* b);

struct BufferBuilderInfo {
    NVNdevice* device = nullptr;
    NVNmemoryPool* pool = nullptr;
    ptrdiff_t offset = 0;
    size_t size = 0;
};
BufferBuilderInfo decode(const NVNbufferBuilder* b);

// NVN2 window HDR state: nvnWindowBuilderSetColorSpace / SetHdrDisplayMetadata / SetHdrContentMetadata and the NVNwindow setters. The sdk
// driver keeps, in the builder, the colour space (i32 at +0x24), ten display floats (+0x30..+0x54) and two content floats (+0x58, +0x5c);
// the meaning of the floats is inferred from their order and VkHdrMetadataEXT (primaries and white point x/y, then max and min luminance).
struct WindowHdrInfo {
    int color_space = -1;            // the game's NVN2 value as given (-1: never set)
    bool has_display = false, has_content = false;
    float display[10] = {};          // red x, y, green x, y, blue x, y, white x, y, max luminance, min luminance (nits)
    float content[2] = {};           // max content light level, max frame-average light level (nits)
};

struct WindowBuilderInfo {
    WindowHdrInfo hdr;               // kept beside the guest builder (the NVN1-sized layout has no room for it)
    NVNdevice* device = nullptr;
    NVNnativeWindow native_window = nullptr;
    int present_interval = 1;
    int texture_count = 0;
    NVNtexture* textures[4] = {};   // copied by SetTextures (at most 4 are kept)
};
WindowBuilderInfo decode(const NVNwindowBuilder* b);

struct EventBuilderInfo {
    NVNdevice* device = nullptr;
    NVNmemoryPool* pool = nullptr;
    ptrdiff_t offset = 0;
};
EventBuilderInfo decode(const NVNeventBuilder* b);

// ---- textures --------------------------------------------------------------------------------------------------------
struct TextureBuilderInfo {
    NVNdevice* device = nullptr;
    int flags = 0;                       // NVNtextureFlags
    int target = 0;                      // NVNtextureTarget
    int width = 0, height = 0, depth = 0;
    int levels = 1, samples = 0;
    int format = 0;                      // NVNformat
    int swizzle[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    int depth_stencil_mode = 0;          // NVNtextureDepthStencilMode
    NVNmemoryPool* pool = nullptr;
    ptrdiff_t offset = 0;
    ptrdiff_t stride = 0;                // pitch of linear textures
    const void* packaged_data = nullptr;
    uint8_t packaged_layout[8] = {};
    bool has_packaged_layout = false;
    uint32_t gl_texture_name = 0;
};
TextureBuilderInfo decode(const NVNtextureBuilder* b);
// nvnTextureGetRawStorageClass / nvnTextureBuilderGetRawStorageClass (NVN2), as the NVN2 driver answers them (sdk 0x5F1050 / 0x5EE9A0):
// LINEAR or LINEAR_RENDER_TARGET textures 1; else by format: NONE 0, STENCIL8 and DEPTH16 3, DEPTH24 and DEPTH24_STENCIL8 4,
// DEPTH32F_STENCIL8 5, every other format (DEPTH32F included) 2.
inline int raw_storage_class(int flags, int format) {
    if (flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) return 1;
    switch (format) {
        case NVN_FORMAT_NONE: return 0;
        case NVN_FORMAT_STENCIL8: case NVN_FORMAT_DEPTH16: return 3;
        case NVN_FORMAT_DEPTH24: case NVN_FORMAT_DEPTH24_STENCIL8: return 4;
        case NVN_FORMAT_DEPTH32F_STENCIL8: return 5;
        default: return 2;
    }
}

// What an NVNtextureView selects. A field that was not set is "the texture's own" (has_* false).
struct TextureViewInfo {
    bool has_levels = false, has_layers = false, has_format = false, has_swizzle = false, has_depth_stencil_mode = false,
         has_target = false;
    int min_level = 0, num_levels = 0;
    int min_layer = 0, num_layers = 0;
    int format = 0;
    int swizzle[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    int depth_stencil_mode = 0;
    int target = 0;
    bool operator==(const TextureViewInfo&) const = default;   // every field (padding is not compared)
};
TextureViewInfo decode(const NVNtextureView* v);

// ---- samplers --------------------------------------------------------------------------------------------------------
struct SamplerInfo {
    int min_filter = 0, mag_filter = 0;                  // NVNminFilter, NVNmagFilter
    int wrap_s = 0, wrap_t = 0, wrap_r = 0;              // NVNwrapMode
    float lod_min = 0, lod_max = 1000, lod_bias = 0, lod_snap = 0;
    int compare_mode = 0, compare_func = 0;              // NVNcompareMode, NVNcompareFunc
    float border_color[4] = {};                          // float form of the border colour
    int border_kind = 0;                                 // 0 float, 1 signed integers, 2 unsigned integers (bits in border_color)
    float max_anisotropy = 1;
    int reduction = 0;                                   // NVNsamplerReduction
    bool operator==(const SamplerInfo& o) const;
};
SamplerInfo decode(const NVNsamplerBuilder* b);
SamplerInfo decode(const NVNsampler* s);

// ---- fixed-function state objects ------------------------------------------------------------------------------------
struct BlendInfo {
    int target = 0;                                      // render target the blend applies to (nvnBlendStateSetBlendTarget)
    int src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0;  // NVNblendFunc
    int eq_rgb = 0, eq_a = 0;                            // NVNblendEquation
    int advanced_mode = 0, advanced_overlap = 0;
    bool advanced_premultiplied_src = false, advanced_normalized_dst = false;
};
BlendInfo decode(const NVNblendState* s);

struct ColorInfo {
    uint8_t blend_enable_mask = 0;                       // bit n: render target n has blending enabled
    int logic_op = 0;                                    // NVNlogicOp
    int alpha_test = 0;                                  // NVNalphaFunc
};
ColorInfo decode(const NVNcolorState* s);

struct ChannelMaskInfo {
    uint8_t mask[8] = {};                                // per render target: bit 0 R, 1 G, 2 B, 3 A write enabled
};
ChannelMaskInfo decode(const NVNchannelMaskState* s);

struct DepthStencilInfo {
    bool depth_test = false, depth_write = false;
    int depth_func = 0;                                  // NVNdepthFunc
    bool stencil_test = false;
    int stencil_func[2] = {};                            // [0] front, [1] back (NVNstencilFunc)
    int stencil_fail[2] = {}, depth_fail[2] = {}, depth_pass[2] = {};   // NVNstencilOp
};
DepthStencilInfo decode(const NVNdepthStencilState* s);

struct PolygonInfo {
    int cull_face = 0;                                   // NVNface
    int front_face = 0;                                  // NVNfrontFace
    int polygon_mode = 0;                                // NVNpolygonMode
    int offset_enables = 0;                              // NVNpolygonOffsetEnable bits
};
PolygonInfo decode(const NVNpolygonState* s);

struct MultisampleInfo {
    bool enable = false;
    int samples = 0, raster_samples = 0;
    bool alpha_to_coverage = false, alpha_to_coverage_dither = false;
    int coverage_modulation_mode = 0;
    bool coverage_to_color = false;
    int coverage_to_color_output = 0;
    bool sample_locations = false, sample_locations_grid = false;
};
MultisampleInfo decode(const NVNmultisampleState* s);

struct VertexAttribInfo {
    int format = 0;                                      // NVNformat
    ptrdiff_t relative_offset = 0;
    int stream = 0;
};
VertexAttribInfo decode(const NVNvertexAttribState* s);

struct VertexStreamInfo {
    ptrdiff_t stride = 0;
    int divisor = 0;
};
VertexStreamInfo decode(const NVNvertexStreamState* s);

}  // namespace nxrt::nvn
