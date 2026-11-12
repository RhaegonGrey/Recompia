// SPDX-License-Identifier: GPL-3.0-or-later
// Upscaler hooks of the replay: temporal upscaling (camera block, jitter, the tone-map draw), SMAA at the tone map and the scene upscaler at the
// game's scene stretch draw.
#include "nvn_replay_internal.h"
#include "extras/smaa_scene.h"

#include "nxrt/game_render.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
SceneOverride g_su;
TsrBlock g_tsr;
static Ref<Texture> g_tsr_depth;                   // temporal upscaling: the depth target of the G-buffer draw that gave this frame's camera
static uint64_t g_tsr_frame = 0;                    // temporal upscaling: tone-map draws so far (the jitter phase of the frame being drawn)
void tsr_resolve() {
    g_tsr.resolved = true;
    const game::CameraBlock* cb = extras::temporal_camera_block();
    if (!cb) return;
    g_tsr.stage = cb->stage;
    g_tsr.slot = cb->slot;
    g_tsr.bytes = extras::temporal_camera_bytes();
    g_tsr.colors = cb->gbuffer_colors;
}
// Temporal upscaling: a copy of the game's camera block with this frame's jitter added (extras::temporal_jitter_block). False: left alone.
bool tsr_jitter(uint32_t* words, size_t n_words) {
    Texture* d = S.depth_tex.get();
    if (!d || !tex_gpu(d)) return false;
    const VkExtent3D e = tex_image_extent(d, 0);
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);   // the scene's viewport is that part of the target: half a pixel of it
    const uint32_t w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(e.width * fx)));
    const uint32_t h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(e.height * fy)));
    return extras::temporal_jitter_block(words, n_words, w, h, g_tsr_frame);
}

// Scene upscaler (S14, #571, src/gfx/extras/hdr_nvn.h): before the game's scene stretch draw, FSR 1 / NIS enlarge the scene part of its
// input to the target's size and the draw samples that instead, or Nearest gives the input the nearest sampler (scene_stretch_for_draw).
// The override lives until the next draw.
// Temporal upscaling (FSR 3 / DLSS / DLAA, src/gfx/extras/temporal.h): a G-buffer draw (the game's count of colour targets and a depth target)
// offers its binding of the game's camera block; the first valid one of the frame is kept with its depth target. Called only while
// extras::temporal_wanted(), so the game names a block and g_tsr holds it.
void temporal_gbuffer_note() {
    if (S.ncolor < g_tsr.colors || !S.depth_tex || !S.stage[g_tsr.stage] || extras::temporal_camera_ready()) return;
    const GState::Bind& u = S.ubo[g_tsr.stage][g_tsr.slot];
    if (!u.addr || u.size < g_tsr.bytes) return;
    uint32_t w[256];                                                      // at most 1 KiB (temporal_camera_block checks the block's length)
    const size_t n = static_cast<size_t>(g_tsr.bytes / 4);
    std::memcpy(w, host_ptr(u.addr), n * 4);
    tsr_jitter(w, n);                                                     // the camera as the draw sees it (the bound copy is jittered the same way)
    extras::temporal_note_gbuffer(w, n);
    if (extras::temporal_camera_ready()) g_tsr_depth = S.depth_tex;
}

// The game's scene covers only part of its targets (extras::scene_fraction, Pokopia's [upscale] scene_scale, #116): the part of a scene
// image of size `scene` that holds it, and the size of the image the draw that reads it then needs, out / fraction, so that its own lookup
// at uv * fraction lands on the upscaled result in that image's top-left out-sized corner. False: the whole image is the scene.
static bool scene_part(VkExtent2D scene, VkExtent2D out, VkExtent2D* part, VkExtent2D* out_tex) {
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);
    if (fx >= 1.0f && fy >= 1.0f) return false;
    part->width = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(scene.width * fx)), 1, scene.width);
    part->height = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(scene.height * fy)), 1, scene.height);
    // rounded down: fraction * out_tex <= out, so the draw's lookups at uv * fraction stay inside the written out-sized corner
    out_tex->width = std::max<uint32_t>(out.width, static_cast<uint32_t>(out.width / fx));
    out_tex->height = std::max<uint32_t>(out.height, static_cast<uint32_t>(out.height / fy));
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "scene upscale: the scene covers %ux%u of its %ux%u target (fraction %.3f x %.3f): %ux%u -> %ux%u, handed to the draw in a "
                "%ux%u image", part->width, part->height, scene.width, scene.height, fx, fy, part->width, part->height, out.width, out.height,
                out_tex->width, out_tex->height);
    }
    return true;
}

// The tone-map draw with a temporal mode: the largest 2D colour input is the HDR scene; the SDK's result (target size) replaces it for this draw.
static void temporal_for_draw(const ProgLayout& pl) {
    ++g_tsr_frame;                                                        // one tone-map draw per frame: the next frame's jitter phase
    Texture* target = S.color_tex[0].get();
    Texture* depth = g_tsr_depth.get();
    static bool told_intercept = false, told_input = false, told_depth = false;
    if (!told_intercept) {
        told_intercept = true;
        NXLOG_I(kTag, "temporal: tone-map draw %s intercepted (target %ux%u)", S.stage[kFragment]->hash.c_str(), target ? target->b.width : 0,
                target ? target->b.height : 0);
    }
    if (!extras::temporal_camera_ready()) {                              // counts the frames without a camera (warns after 120)
        extras::temporal_evaluate(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, {}, {}, false);
        return;
    }
    if (!tex_gpu(target) || !depth || !tex_gpu(depth)) {
        if (!told_depth) {
            told_depth = true;
            NXLOG_W(kTag, "temporal: camera found but no usable target / G-buffer depth texture at the tone-map draw");
        }
        return;
    }
    const BindingDesc* best = nullptr;
    TexResolved best_r{};
    uint64_t best_area = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex) || shares_image(r.tex, depth)) continue;
        const VkExtent3D e = tex_image_extent(r.tex, 0);
        const uint64_t area = static_cast<uint64_t>(e.width) * e.height;
        if (area > best_area) { best_area = area; best = &d; best_r = r; }
    }
    if (!best) {
        if (!told_input) {
            told_input = true;
            NXLOG_W(kTag, "temporal: the tone-map draw samples no 2D colour texture to use as the scene");
        }
        return;
    }
    const DescriptorBinding& b = best->shader->refl.bindings[best->index];
    const VkExtent3D ce = tex_image_extent(best_r.tex, 0), de = tex_image_extent(depth, 0), te = tex_image_extent(target, 0);
    VkExtent2D render{std::min(ce.width, de.width), std::min(ce.height, de.height)}, out_tex{};
    if (VkExtent2D part{}; scene_part(render, VkExtent2D{te.width, te.height}, &part, &out_tex)) render = part;
    end_pass();                                                           // compute work cannot run inside a render pass
    tex_ensure_current(best_r.tex, *F);
    tex_transition(best_r.tex, *F, tex_sampled_layout(best_r.tex));
    tex_transition(depth, *F, tex_sampled_layout(depth));
    VkImageView src = tex_sampled_view(best_r.tex, make_request(b, best_r));
    ViewRequest dr;
    dr.depth_aspect = true;
    VkImageView dv = tex_sampled_view(depth, dr);
    if (!src || !dv) return;
    VkImageView out = extras::temporal_evaluate(&device(), F->cb, src, dv, tex_sampled_layout(depth), render, VkExtent2D{te.width, te.height},
                                                S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W, out_tex);
    g_cmd.dirty = kAllDirty;                                                  // the SDKs bind their own compute state
    shadow_invalidate();
    if (out) {
        g_su.tex = best_r.tex;
        g_su.view = out;
    }
}

// SMAA ([upscale] aa): the HDR scene the tone-map draw reads goes through extras::SmaaScene and the draw reads the result, so only the 3D picture is
// filtered (the HUD and the menus are drawn after the tone map). The scene is the first 2D colour input that is a float image (RGBA16F or R11G11B10F) of the target's size.
static void smaa_scene_for_draw(const ProgLayout& pl, VkExtent3D te, extras::AaMode aa) {
    struct Seen { uint32_t w, h; VkFormat f; } tried[8];
    uint32_t tried_n = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D || b.image.depth) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex) || shares_image(r.tex, S.depth_tex.get())) continue;
        const VkExtent3D se = tex_image_extent(r.tex, 0);
        const VkFormat sf = tex_vk_format(r.tex);
        if (static_cast<int>(tried_n) < 8) tried[tried_n++] = {se.width, se.height, sf};
        if (se.width != te.width || se.height != te.height || (sf != VK_FORMAT_R16G16B16A16_SFLOAT && sf != VK_FORMAT_B10G11R11_UFLOAT_PACK32)) continue;
        extras::SmaaScene* sm = extras::scene_smaa(&device());
        if (!sm) return;
        end_pass();                                                           // the passes cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
        VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
        if (!src) return;
        VkImageView out = sm->apply(F->cb, src, VkExtent2D{se.width, se.height}, aa);
        shadow_invalidate();
        if (out) {
            g_su.tex = r.tex;
            g_su.view = out;
        }
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "scene anti-aliasing: SMAA on the %ux%u scene (texture %llu)", se.width, se.height, static_cast<unsigned long long>(r.tex->id));
        }
        return;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        NXLOG_W(kTag, "scene anti-aliasing: no float scene input the size of the tone-map target (%ux%u): SMAA skipped", te.width, te.height);
        for (uint32_t i = 0; i < tried_n; ++i) NXLOG_W(kTag, "  tone-map input %u: %ux%u format %d", i, tried[i].w, tried[i].h, static_cast<int>(tried[i].f));
    }
}

void tonemap_for_draw(const ProgLayout& pl) {
    g_su.clear();
    g_su.stretch_armed = true;                                            // the frame's scene stretch comes after its tone map
    if (S.ncolor == 0 || !S.color_tex[0]) return;
    if (extras::temporal_wanted()) {
        temporal_for_draw(pl);
        return;
    }
    const extras::AaMode aa = extras::aa_mode();
    // SMAA runs when the scene is shown at its own size (an upscaled scene part keeps its upscaler; SMAA does not read a part of an image)
    if (aa != extras::AaMode::Off && (!extras::scene_upscale_wanted() || extras::upscale_scene_scale() >= 100)) {
        Texture* tgt = S.color_tex[0].get();
        if (tex_gpu(tgt)) smaa_scene_for_draw(pl, tex_image_extent(tgt, 0), aa);
    }
}

// The game's scene stretch (game::SceneStretch, #571): the first such draw after the tone map that enlarges, its one 2D colour input no
// larger than its target. Copies of the same shader that shrink (the game's own reductions) and later copies are left alone.
// Nearest (#232): the input is sampled with the nearest sampler (clamp to edge), so the stretch shows the scene's own pixels.
// FSR 1 / NIS: the scene part of the input (scene_part) goes through the upscaler to the target's size, and the draw reads the result in an
// image sized so that its own lookup at uv * fraction lands on it. The input is display-referred colour in [0,1] (8-bit sRGB, read linear).
void scene_stretch_for_draw(const ProgLayout& pl) {
    g_su.clear();
    if (!g_su.stretch_armed || !extras::scene_upscale_wanted() || S.ncolor == 0 || !S.color_tex[0]) return;
    Texture* target = S.color_tex[0].get();
    if (!tex_gpu(target)) return;
    const VkExtent3D te = tex_image_extent(target, 0);    // image sizes: with a render scale the upscaler works on the real pixels
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D || b.image.depth) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex)) continue;
        const VkExtent3D se = tex_image_extent(r.tex, 0);
        if (se.width > te.width || se.height > te.height) return;          // a reduction, not the stretch
        g_su.stretch_armed = false;
        float fx = 1.0f, fy = 1.0f;
        extras::scene_fraction(&fx, &fy);
        if (extras::upscale_mode() == extras::UpscaleMode::Nearest) {
            const bool smaller = se.width < te.width || se.height < te.height;
            // a scene_scale below 100 always puts the scene in the top-left of full-size targets, published fraction or not
            if (!smaller && fx >= 1.0f && fy >= 1.0f && extras::upscale_scene_scale() >= 100) return;   // a 1:1 copy
            g_su.near[g_su.near_n++] = r.tex;
            static Texture* told = nullptr;
            if (told != r.tex) {
                told = r.tex;
                NXLOG_I(kTag, "scene upscale: nearest on the scene stretch %s (%ux%u input, fraction %.3f x %.3f, scene_scale %d%%, target %ux%u)",
                        S.stage[kFragment]->hash.c_str(), se.width, se.height, fx, fy, extras::upscale_scene_scale(), te.width, te.height);
            }
            return;
        }
        // the scene is either a smaller image or the top-left part of a full-size one (scene_part)
        VkExtent2D part{se.width, se.height}, out_tex{};
        const VkExtent2D out{te.width, te.height};
        if (!scene_part(part, out, &part, &out_tex) && se.width >= te.width && se.height >= te.height) {
            static bool told_whole = false;
            if (!told_whole) {
                told_whole = true;
                NXLOG_I(kTag, "scene upscale: the scene stretch reads its whole %ux%u input at the target's size: nothing to upscale", se.width, se.height);
            }
            return;
        }
        if (part.width >= te.width && part.height >= te.height) return;
        extras::SpatialUpscaler* su = extras::scene_upscaler(&device(), part, out);
        if (!su) return;
        end_pass();                                                           // compute work cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
        VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
        if (!src) return;
        VkImageView up = su->upscale(F->cb, src, part, VkExtent2D{se.width, se.height}, out, false, 1.0f, extras::upscale_sharpness(), out_tex);
        shadow_invalidate();
        if (up) {
            g_su.tex = r.tex;
            g_su.view = up;
        }
        static bool told = false;
        if (!told) {
            told = true;
            NXLOG_I(kTag, "scene upscale: %s on the scene stretch %s, %ux%u -> %ux%u", extras::upscale_mode() == extras::UpscaleMode::Nis ? "NIS" : "FSR 1",
                    S.stage[kFragment]->hash.c_str(), part.width, part.height, out.width, out.height);
        }
        return;
    }
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
