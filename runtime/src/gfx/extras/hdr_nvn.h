// SPDX-License-Identifier: GPL-3.0-or-later
// What the NVN back end asks of the HDR scene pipeline (S14). Two questions, both answered from the HdrState the renderer published:
//   hdr_promote_target   should this render target be kept as float16 instead of 8-bit sRGB?
//   hdr_tonemap_variant  the HDR variant of a fragment shader (the tone-map shader), or empty when the shader is not one
#pragma once
#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::extras {

// The format to use instead of `chosen` for a texture: VK_FORMAT_R16G16B16A16_SFLOAT for the colour render targets of the post chain (8-bit sRGB,
// created compressible or for display, single 2D level) while the HDR scene is active; VK_FORMAT_UNDEFINED otherwise.
VkFormat hdr_promote_target(VkFormat chosen, int nvn_texture_flags, bool is_2d, uint32_t levels, uint32_t layers, uint32_t samples, bool nvn_format_is_srgb);

// SPIR-V of the HDR variant of the fragment shader `words` (hash for the log), or false when it is not the tone-map shader / the HDR scene is off.
bool hdr_tonemap_variant(const std::vector<uint32_t>& words, const std::string& hash, std::vector<uint32_t>& out);

// True when `words` is the game's tone-map fragment shader (the structural match of hdr_tonemap_patch.h); cached by hash. Independent of the HDR scene.
bool hdr_is_tonemap(const std::vector<uint32_t>& words, const std::string& hash);

// Scene upscaler (S14, #571): the game renders the 3D scene below the output size ([upscale] scene_scale, the game's dynamic-resolution path)
// into the top-left part of its targets, and one copy draw the game names (game::Render::scene_stretch) stretches that part over the frame.
// With FSR 1 or NIS the replay runs that upscaler on the part first and the stretch draw reads the result; with Nearest it samples the part
// unfiltered (the game's bilinear tap is bypassed either way).
bool scene_upscale_wanted();                          // the upscale mode is FSR 1, NIS or Nearest
// True when the fragment shader of cache hash `hash` is the game's scene stretch shader (game::SceneStretch); false when the game names none.
bool is_scene_stretch(const std::string& hash);
class SpatialUpscaler;
// The upscaler of the current mode for in -> out (created on first use; gfx_device is the gfx::Device of the NVN back end). NIS covers 1x to 2x per side;
// beyond that, or when NIS failed to start, FSR 1 is used.
SpatialUpscaler* scene_upscaler(void* gfx_device, VkExtent2D in, VkExtent2D out);
// SMAA on the HDR scene ([upscale] aa, live): the replay runs it on the scene the tone-map draw reads, so only the 3D picture is filtered.
class SmaaScene;
SmaaScene* scene_smaa(void* gfx_device);              // created on first use; null when it failed to start
float scene_upscale_prescale();                       // pre-exposure scene colour is about 10x smaller than the display-referred range

}  // namespace nxrt::extras
