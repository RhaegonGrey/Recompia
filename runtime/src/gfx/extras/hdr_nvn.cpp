// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/hdr_nvn.h"

#include <mutex>
#include <map>
#include <set>

#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/game_render.h"
#include "nxrt/log.h"
#include "extras/fsr1_spatial.h"
#include "extras/nis_spatial.h"
#include "extras/smaa_scene.h"
#include "extras/hdr_tonemap_patch.h"

namespace nxrt::extras {
namespace {
constexpr int kFlagDisplay = 0x1, kFlagCompressible = 0x8;   // NVN_TEXTURE_FLAGS_DISPLAY, NVN_TEXTURE_FLAGS_COMPRESSIBLE

// The game's tone-map shapes (game::Render::tonemaps), read once. None: no shader is the tone map.
struct ToneMaps {
    const game::ToneMapMatch* match = nullptr;
    uint32_t count = 0;
};
const ToneMaps& tonemaps() {
    static const ToneMaps t = [] {
        ToneMaps r;
        if (const game::Render* gr = game::info().render; gr && gr->tonemaps) {
            r.match = gr->tonemaps;
            r.count = gr->tonemap_count;
        }
        return r;
    }();
    return t;
}

// The first of the game's tone-map shapes, in its order, that `words` is: patched into `out` (the LUT kind with peak_ratio). Null: none.
const game::ToneMapMatch* tonemap_patch(const std::vector<uint32_t>& words, float peak_ratio, std::vector<uint32_t>& out) {
    const ToneMaps& t = tonemaps();
    for (uint32_t i = 0; i < t.count; ++i) {
        const game::ToneMapMatch& m = t.match[i];
        if (m.max_words && words.size() > m.max_words) continue;
        const bool ok = m.kind == game::ToneMapKind::Curve ? spirv_hdr_tonemap(words, out)
                                                            : spirv_hdr_tonemap_lut(words, m.shaper_scale, m.shaper_bias, peak_ratio, out);
        if (ok) return &m;
    }
    return nullptr;
}
}

VkFormat hdr_promote_target(VkFormat chosen, int flags, bool is_2d, uint32_t levels, uint32_t layers, uint32_t samples, bool nvn_srgb) {
    if (!nvn_srgb || chosen != VK_FORMAT_R8G8B8A8_SRGB || !is_2d || levels != 1 || layers != 1 || samples > 1) return VK_FORMAT_UNDEFINED;
    if (!(flags & (kFlagDisplay | kFlagCompressible))) return VK_FORMAT_UNDEFINED;
    if (!hdr_scene_active()) return VK_FORMAT_UNDEFINED;
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool hdr_tonemap_variant(const std::vector<uint32_t>& words, const std::string& hash, std::vector<uint32_t>& out) {
    if (!hdr_scene_active()) return false;
    static std::mutex mu;
    static std::set<std::string> tried;
    const game::ToneMapMatch* m = tonemap_patch(words, hdr_state().peak_ratio, out);
    const bool ok = m != nullptr;
    std::lock_guard<std::mutex> lk(mu);
    if (tried.insert(hash).second && ok)
        NXLOG_I("hdr", "tone-map shader %s: HDR variant made (%s, peak ratio %.2f)", hash.c_str(),
                m->kind == game::ToneMapKind::LutShaper ? "LUT highlights extended" : "curve",
                static_cast<double>(hdr_state().peak_ratio));
    return ok;
}

bool hdr_is_tonemap(const std::vector<uint32_t>& words, const std::string& hash) {
    static std::mutex mu;
    static std::map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(hash);
        if (it != cache.end()) return it->second;
    }
    std::vector<uint32_t> scratch;
    // the same match as hdr_tonemap_variant; the patch result is only used as a match test here (any peak ratio above 1 will do)
    const bool ok = tonemap_patch(words, 2.0f, scratch) != nullptr;
    std::lock_guard<std::mutex> lk(mu);
    cache[hash] = ok;
    return ok;
}

bool scene_upscale_wanted() {
    const UpscaleMode m = upscale_mode();
    return m == UpscaleMode::Fsr1 || m == UpscaleMode::Nis || m == UpscaleMode::Nearest;
}

bool is_scene_stretch(const std::string& hash) {
    static const char* const shader = [] {
        const game::Render* gr = game::info().render;
        return gr && gr->scene_stretch ? gr->scene_stretch->shader : nullptr;
    }();
    return shader && hash == shader;
}

SpatialUpscaler* scene_upscaler(void* gfx_device, VkExtent2D in, VkExtent2D out) {
    static Fsr1Spatial* fsr = nullptr;
    static NisSpatial* nis = nullptr;
    static bool tried_fsr = false, tried_nis = false, told_range = false;
    if (!gfx_device) return nullptr;
    gfx::Device& dev = *static_cast<gfx::Device*>(gfx_device);
    if (upscale_mode() == UpscaleMode::Nis) {
        if (!tried_nis) {
            tried_nis = true;
            auto* n = new NisSpatial();
            if (n->init(dev)) nis = n;
            else delete n;
        }
        if (nis && nis->supports(in, out)) return nis;
        if (nis && !told_range) {
            told_range = true;
            NXLOG_I("hdr", "scene upscale %ux%u -> %ux%u is beyond NIS's 2x: FSR 1 is used for it", in.width, in.height, out.width, out.height);
        }
    }
    if (!tried_fsr) {
        tried_fsr = true;
        auto* f = new Fsr1Spatial();
        if (f->init(dev)) fsr = f;
        else delete f;
    }
    return fsr;
}

SmaaScene* scene_smaa(void* gfx_device) {
    static SmaaScene* smaa = nullptr;
    static bool tried = false;
    if (!gfx_device) return nullptr;
    if (!tried) {
        tried = true;
        auto* s = new SmaaScene();
        if (s->init(*static_cast<gfx::Device*>(gfx_device))) smaa = s;
        else delete s;
    }
    return smaa;
}

float scene_upscale_prescale() { return 0.1f; }

}  // namespace nxrt::extras
