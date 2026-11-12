// SPDX-License-Identifier: GPL-3.0-or-later
// Render passes of the replay: dynamic rendering begun lazily on the current targets, ended with or without a barrier (tracked barriers).
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
Pass P;

static VkImageLayout color_layout(uint8_t fbl, uint32_t slot) {
    return (fbl >> slot) & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
PassBarriers g_bar;
void PassBarriers::begin_submit(bool tracked_on) {
    tracked = tracked_on;
    set_narrow_barriers(tracked);
    pending.clear();
    pass_unsafe = false;
}

// ---- passes ----------------------------------------------------------------------------------------------------------------
// Ends transform feedback in the open pass, remembering the bytes written in the counter buffer (a pass break must not restart the buffer).
void xfb_end_in_pass() {
    if (!g_xfb.began) return;
    VkBuffer cbuf = g_xfb.counter.buffer;
    const VkDeviceSize off = static_cast<VkDeviceSize>(g_xfb.cur) * kXfbSlotBytes;
    vkCmdEndTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &off);
    g_xfb.began = false;
    g_xfb.have_counter = true;
    g_xfb.slot_written[g_xfb.cur] = true;
}

// Two texture objects over one image (as shares_image further down, which this code cannot see yet).
static bool same_image(Texture* a, Texture* b) { return a == b || (a && b && a->gpu && a->gpu == b->gpu); }

// to_pass: the caller only starts another pass or changes image layouts next (ensure_pass, prepare_textures); with tracked barriers the pass
// then ends without a barrier when it only wrote its attachments. Any other caller gets a full barrier, also for passes ended earlier
// without one.
void end_pass(bool to_pass) {
    rp_bump(ft::kCntEpEndPass);
    if (!P.open) {
        if (!to_pass && !g_bar.pending.empty()) {
            memory_barrier(F->cb);
            g_bar.pending.clear();
        }
        return;
    }
    xfb_end_in_pass();
    vkCmdEndRendering(F->cb);
    P.open = false;
    if (gpass_on()) {
        char b[112];
        std::snprintf(b, sizeof b, "pass %ux%u c%u:%d d%d%s", P.extent.width, P.extent.height, P.ncolor, static_cast<int>(P.color_fmt[0]), static_cast<int>(P.depth_fmt),
                      P.depth_ro ? " ro" : "");
        gpass_stamp(*F, b, P.draws);
        char vb[128];
        std::snprintf(vb, sizeof vb, "gv:%s", b);
        ft::mark(vb, static_cast<uint32_t>(std::min<uint64_t>(P.verts, 0xffffffffull)));
    }
    if (to_pass && g_bar.tracked && !g_bar.pass_unsafe) {
        auto remember = [](Texture* t, VkImageLayout l) {
            if (!t || !tex_gpu(t)) return;
            for (const PendingWrite& w : g_bar.pending) if (w.tex.get() == t) return;
            g_bar.pending.push_back({Ref<Texture>::share(t), l});
        };
        for (uint32_t i = 0; i < P.ncolor; ++i) remember(P.color[i].tex, color_layout(P.fbl, i));
        if (!P.depth_ro) remember(P.depth.tex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    } else {
        memory_barrier(F->cb);
        g_bar.pending.clear();
    }
    g_bar.pass_unsafe = false;
    g_cmd.bound_pipeline = VK_NULL_HANDLE;
    g_cmd.dirty = kAllDirty;
}

// A pass is about to use `t` as an attachment in `layout`. An earlier pass that ended without a barrier wrote it, and no layout change
// (which would carry the dependency) happens in between: the two passes need a barrier.
static bool pending_conflict(Texture* t, VkImageLayout layout) {
    if (!t || g_bar.pending.empty()) return false;
    for (const PendingWrite& w : g_bar.pending)
        if (same_image(w.tex.get(), t) && tex_layout(t) == w.layout && layout == w.layout) return true;
    return false;
}

static Target make_target(Texture* t, const TextureViewInfo& v, bool has_view) {
    Target tg;
    tg.tex = t;
    if (has_view) {
        if (v.has_levels) tg.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) {
            tg.layer = static_cast<uint32_t>(v.min_layer);
            tg.layers = std::max(1, v.num_layers);
        }
        else if (t && t->layers > 1 && t->b.target != NVN_TEXTURE_TARGET_3D) tg.layers = t->layers;   // a view without layers
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi && fi->vk != VK_FORMAT_UNDEFINED) tg.view_format = fi->vk;
        }
    } else if (t && t->layers > 1 && t->b.target != NVN_TEXTURE_TARGET_3D) {
        // no view: an array or cube map target is attached with all its layers (layered rendering: a geometry shader picks the layer with
        // gl_Layer; without it the shaders draw to layer 0)
        tg.layers = t->layers;
    }
    return tg;
}

static bool targets_match() {
    if (P.ncolor != S.ncolor) return false;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (!(P.color[i] == make_target(S.color_tex[i].get(), S.color_view[i], S.color_has_view[i]))) return false;
    return P.depth == make_target(S.depth_tex.get(), S.depth_view, S.depth_has_view);
}

// The current render targets get their images (scaled when they qualify, tex_gpu_attachment). With `fix` (no pass open), as Ryujinx does
// when it binds render targets: a target whose image was made unscaled (first used another way) is scaled now, and the targets of one
// pass are brought to one scale (the lowest when one of them cannot be scaled). Returns the scale of the targets.
float attach_targets(bool fix) {
    Texture* ts[9];
    uint32_t n = 0;
    for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (S.color_tex[i]) ts[n++] = S.color_tex[i].get();
    if (S.depth_tex) ts[n++] = S.depth_tex.get();
    float lo = 0.0f, hi = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (!tex_gpu_attachment(ts[i])) continue;
        if (fix) {
            const float want = tex_scale_wanted(ts[i]);
            if (want != tex_scale(ts[i])) tex_rescale(ts[i], *F, want, "used as a render target");
        }
        const float sc = tex_scale(ts[i]);
        lo = lo == 0.0f ? sc : std::min(lo, sc);
        hi = std::max(hi, sc);
    }
    if (lo == 0.0f) return 1.0f;
    if (lo != hi && fix) {
        for (uint32_t i = 0; i < n; ++i)
            if (tex_gpu(ts[i]) && tex_scale(ts[i]) != lo) tex_rescale(ts[i], *F, lo, "a render target of the same pass cannot be scaled");
    }
    return lo;
}

// Starts a pass on the current render targets if none is open for them. False when there is nothing to render to. fbl: the colour slots
// that are in the feedback loop layout (a pass in another layout mode is ended: the layouts and the pipelines' feedback flag go together).
bool ensure_pass(bool depth_ro, uint8_t fbl) {
    // Every draw comes through here. The targets are compared only after a SetRenderTargets (the game often sets the ones it has): a
    // target is made from S's target fields, which only that op writes, and from fields of the texture that never change after its init.
    if (P.open && P.depth_ro == depth_ro && P.fbl == fbl && (P.targets_gen == S.targets_gen || targets_match())) {
        P.targets_gen = S.targets_gen;
        return true;
    }
    end_pass(true);
    if (!g_bar.pending.empty()) {
        bool conflict = false;
        for (uint32_t i = 0; i < S.ncolor && !conflict; ++i) conflict = pending_conflict(S.color_tex[i].get(), color_layout(fbl, i));
        if (!conflict && S.depth_tex)
            conflict = pending_conflict(S.depth_tex.get(), depth_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (conflict) {
            memory_barrier(F->cb);
            g_bar.pending.clear();
        }
    }
    if (gpass_on()) gpass_stamp(*F, "gap", 0);      // copies, dispatches, barriers and layout changes since the previous pass
    const float pass_scale = attach_targets(true);
    P = Pass();
    P.depth_ro = depth_ro;
    P.scale = pass_scale;
    P.fbl = fbl;
    P.targets_gen = S.targets_gen;
    VkRenderingAttachmentInfo colors[8] = {};
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool any = false;
    VkExtent2D extent = {UINT32_MAX, UINT32_MAX};
    P.ncolor = S.ncolor;
    for (uint32_t i = 0; i < S.ncolor; ++i) {
        colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Texture* t = S.color_tex[i].get();
        P.color[i] = make_target(t, S.color_view[i], S.color_has_view[i]);
        if (!t || !tex_gpu(t)) continue;
        const bool fresh = tex_layout(t) == VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageLayout cl = color_layout(fbl, i);
        tex_transition_attach(t, *F, cl, P.color[i].level);   // the pass writes this level only (tex_write_gen: the feedback copies)
        tex_mark_gpu_written(t, static_cast<int>(P.color[i].level));
        VkImageView v = tex_attachment_view(t, P.color[i].level, P.color[i].layer, P.color[i].layers, P.color[i].view_format);
        if (!v) continue;
        colors[i].imageView = v;
        colors[i].imageLayout = cl;
        colors[i].loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        P.color_fmt[i] = tex_attachment_format(t, P.color[i].view_format);
        const VkExtent3D e = tex_image_extent(t, P.color[i].level);
        extent.width = std::min(extent.width, e.width);
        extent.height = std::min(extent.height, e.height);
        any = true;
    }
    Texture* dt = S.depth_tex.get();
    P.depth = make_target(dt, S.depth_view, S.depth_has_view);
    bool have_depth = false;
    if (dt && tex_gpu(dt)) {
        const bool fresh = tex_layout(dt) == VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageLayout dlayout = depth_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        tex_transition(dt, *F, dlayout);
        if (!depth_ro) tex_mark_gpu_written(dt);
        VkImageView v = tex_attachment_view(dt, P.depth.level, P.depth.layer, P.depth.layers, P.depth.view_format);
        if (v) {
            depth.imageView = stencil.imageView = v;
            depth.imageLayout = stencil.imageLayout = dlayout;
            depth.loadOp = stencil.loadOp = (fresh && !depth_ro) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            stencil.clearValue.depthStencil = {1.0f, 0};
            P.depth_fmt = tex_vk_format(dt);
            P.has_stencil = (tex_aspect(dt) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            const VkExtent3D e = tex_image_extent(dt, P.depth.level);
            extent.width = std::min(extent.width, e.width);
            extent.height = std::min(extent.height, e.height);
            have_depth = true;
            any = true;
        }
    }
    if (!any && g_xfb.active && device().features().transform_feedback) {   // a stream-out pass without attachments
        extent = {1, 1};
        any = true;
    }
    if (!any || extent.width == UINT32_MAX) return false;
    P.extent = extent;
    P.samples = std::max<uint32_t>(1, S.color_tex[0] ? static_cast<uint32_t>(S.color_tex[0]->b.samples) : 1);
    if (P.samples == 0) P.samples = 1;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    uint32_t layer_count = UINT32_MAX;     // layered rendering: the layers every attachment has in common
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) layer_count = std::min(layer_count, std::max<uint32_t>(1, P.color[i].layers));
    if (have_depth) layer_count = std::min(layer_count, std::max<uint32_t>(1, P.depth.layers));
    ri.layerCount = layer_count == UINT32_MAX ? 1 : layer_count;
    uint32_t last = 0;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) last = i + 1;
    ri.colorAttachmentCount = S.ncolor;
    ri.pColorAttachments = colors;
    if (have_depth) {
        ri.pDepthAttachment = &depth;
        if (P.has_stencil) ri.pStencilAttachment = &stencil;
    }
    (void)last;
    gstamp("pass setup");
    vkCmdBeginRendering(F->cb, &ri);
    P.open = true;
    rp_bump(ft::kCntEpBeginPass);
    counters().passes.fetch_add(1, std::memory_order_relaxed);
    g_cmd.bound_pipeline = VK_NULL_HANDLE;
    return true;
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
