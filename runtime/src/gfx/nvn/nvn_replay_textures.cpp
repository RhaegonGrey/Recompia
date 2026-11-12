// SPDX-License-Identifier: GPL-3.0-or-later
// Texture bindings of the replay: handle resolution, sampled-texture preparation, feedback draws (loop layout or copy), 3D copies of 2D textures.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
ResolveCache g_rc;
TexCopies g_copies;
bool resolve_once_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "resolve_once", true);
        NXLOG_I(kTag, "texture bindings: %s ([nvn] resolve_once)", v ? "a handle is resolved once per pool state" : "resolved by every caller");
        return v;
    }();
    return on;
}

static TexResolved resolve_handle(uint64_t handle) {
    TexResolved r;
    ft::split_count(ft::kCntResolve);
    const uint32_t tid = handle_texture_id(handle), sid = handle_sampler_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    if (S.spool && static_cast<int>(sid) < S.spool->count) {
        std::shared_lock<std::shared_mutex> lk(S.spool->lock);
        if (S.spool->used[sid]) {
            r.smp = S.spool->entries[sid];
            r.has_sampler = true;
        }
    }
    return r;
}

// ImageBinding: an NVN2 compute texture binding over the program's image slot n (its size query, apply_nvn2_compute_table): the image
// bound there (an image handle names no sampler; image_as_texture binds the dummy sampler).
TexResolved resolve_texture(const BindingDesc& d) {
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::SamplerBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 64) handle = S.tex[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    } else if (b.nvn.source == NvnSource::ImageBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 16) handle = handle_texture_id(S.img[d.nvn_stage][idx]);   // texture id only (image_as_texture ignores the sampler)
    }
    static const bool on = resolve_once_on();
    if (!on) return resolve_handle(handle);
    ResolveSlot& sl = g_rc.slot[(handle * 0x9e3779b97f4a7c15ull) >> 55];
    const uint64_t gen = be::g_bind_gen.load(std::memory_order_acquire);   // before the resolve: a write after it is seen next time
    if (sl.pools == g_rc.pools && sl.gen == gen && sl.handle == handle) {
        ft::split_count(ft::kCntResolveHit);
        return sl.r;
    }
    sl.r = resolve_handle(handle);
    sl.gen = gen;
    sl.handle = handle;
    sl.pools = g_rc.pools;
    return sl.r;
}

ViewRequest make_request(const DescriptorBinding& b, const TexResolved& r) {
    ViewRequest q;
    q.type = view_type_for(b);
    q.depth_compare = b.image.depth;
    const Texture& t = *r.tex;
    int sw[4] = {t.b.swizzle[0], t.b.swizzle[1], t.b.swizzle[2], t.b.swizzle[3]};
    int dsmode = t.b.depth_stencil_mode;
    if (r.has_view) {
        const TextureViewInfo& v = r.view;
        if (v.has_levels) { q.base_level = static_cast<uint32_t>(v.min_level); q.level_count = static_cast<uint32_t>(v.num_levels); }
        if (v.has_layers) { q.base_layer = static_cast<uint32_t>(v.min_layer); q.layer_count = static_cast<uint32_t>(v.num_layers); }
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi) q.format = fi->vk;
        }
        if (v.has_swizzle) for (int i = 0; i < 4; ++i) sw[i] = v.swizzle[i];
        if (v.has_depth_stencil_mode) dsmode = v.depth_stencil_mode;
    }
    q.swizzle.r = vk_swizzle(sw[0]);
    q.swizzle.g = vk_swizzle(sw[1]);
    q.swizzle.b = vk_swizzle(sw[2]);
    q.swizzle.a = vk_swizzle(sw[3]);
    q.depth_aspect = dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    switch (b.image.component) {
        case ScalarType::Float: q.component = 1; break;
        case ScalarType::Sint: q.component = 2; break;
        case ScalarType::Uint: q.component = 3; break;
        default: break;
    }
    return q;
}

// The image a draw reads or writes through an image binding (nvnCommandBufferBindImage): the handle is a texture pool id.
TexResolved resolve_image(const BindingDesc& d) {
    TexResolved r;
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::ImageBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 16) handle = S.img[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    }
    const uint32_t tid = handle_texture_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    return r;
}

// True when a texture the program samples is the depth attachment of the current target set: the draw then runs in a pass whose depth
// attachment is read-only (DEPTH_STENCIL_READ_ONLY_OPTIMAL serves as attachment and as sampled layout).
bool samples_depth_target(const ProgLayout& pl) {
    if (!S.depth_tex) return false;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (shares_image(resolve_texture(d).tex, S.depth_tex.get())) return true;
    }
    return false;
}

// [nvn] feedback_loop: auto (default) = the feedback loop layout where the GPU has VK_EXT_attachment_feedback_loop_layout, off = the copy.
static bool fb_loop_on() {
    static const bool on = [] {
        const std::string v = nxrt::config_get_str("nvn", "feedback_loop", "auto");
        const bool want = v != "off" && v != "0" && v != "false";
        const bool have = device().features().attachment_feedback_loop;
        NXLOG_I(kTag, "feedback draws: %s ([nvn] feedback_loop = %s, VK_EXT_attachment_feedback_loop_layout %s)", want && have ? "feedback loop layout" : "copy",
                v.c_str(), have ? "yes" : "no");
        return want && have;
    }();
    return on;
}
// [nvn] feedback_copy_levels: auto (default) = the copy path copies only the levels that changed, off = every level each time.
static bool fb_levels_on() {
    static const bool on = [] {
        const std::string v = nxrt::config_get_str("nvn", "feedback_copy_levels", "auto");
        return v != "off" && v != "0" && v != "false";
    }();
    return on;
}

static Texture* feedback_clone(Texture* t) {
    FbClone& fe = g_copies.fb_clone[t];
    fe.last = F ? F->serial : 0;
    Ref<Texture>& c = fe.tex;
    g_copies.fb_clone_n.store(static_cast<int64_t>(g_copies.fb_clone.size()), std::memory_order_relaxed);
    if (c && (c->b.width != t->b.width || c->b.height != t->b.height || c->b.depth != t->b.depth || c->fmt != t->fmt || c->levels != t->levels || c->layers != t->layers ||
              (c->gpu && tex_scale(c.get()) != tex_scale(t))))
        c = Ref<Texture>();                                 // the game made another texture at this address
    if (!c) {
        Texture* n = new Texture();
        n->b = t->b;
        n->fmt = t->fmt;
        n->levels = t->levels;
        n->layers = t->layers;
        n->block_linear = t->block_linear;
        n->cpu_dirty.store(false);
        c = Ref<Texture>(n);
        static uint32_t next_id = 0x7f000000;
        n->id = next_id++;
        for (uint64_t& g : fe.gen) g = 0;
    }
    return tex_gpu_scaled(c.get(), tex_scale(t)) ? c.get() : nullptr;
}

// GPU pass profiler label: size, format, levels, the level the pass writes and the levels the draw samples.
static void feedback_label(const TexResolved& r, const char* what) {
    uint32_t att_lv = 0;
    for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (shares_image(r.tex, S.color_tex[i].get())) { att_lv = S.color_level[i]; break; }
    char fb[112];
    std::snprintf(fb, sizeof fb, "feedback %s %ux%u %s lv%u w%u s%d+%d", what, r.tex->b.width, r.tex->b.height, r.tex->fmt ? r.tex->fmt->name : "?",
                  tex_levels(r.tex), att_lv, r.has_view && r.view.has_levels ? r.view.min_level : 0,
                  r.has_view && r.view.has_levels ? r.view.num_levels : static_cast<int>(tex_levels(r.tex)));
    gpass_stamp(*F, fb, 0);
}

void feedback_prepare(const ProgLayout& pl) {
    if (!g_copies.fb_use.empty()) { g_copies.fb_use.clear(); rp_bump(ft::kCntEpFeedback); }
    g_copies.fbl_want = 0;
    bool any = false;
    TexResolved loop_label;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        const TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || !is_color_feedback(r.tex) || r.tex->b.samples > 1 || g_copies.fb_use.count(r.tex)) continue;
        any = true;
        if (fb_loop_on()) {
            uint8_t slots = 0;
            for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (shares_image(r.tex, S.color_tex[i].get())) slots |= static_cast<uint8_t>(1u << i);
            if (slots && (g_copies.fbl_want & slots) == slots) continue;          // another binding of the same image
            if (!tex_has_feedback_loop(r.tex)) {                          // the first feedback draw of this image: remade with the usage (once)
                end_pass();
                tex_enable_feedback_loop(r.tex, *F);
            }
            if (slots && tex_has_feedback_loop(r.tex)) {
                if (!tex_current_on_gpu(r.tex)) {
                    end_pass();
                    tex_ensure_current(r.tex, *F);
                }
                g_copies.fbl_want |= slots;
                if (gpass_on() && !loop_label.tex) loop_label = r;   // stamped after the pass ends below (the open pass's draws are its own interval)
                continue;
            }
        }
        Texture* c = feedback_clone(r.tex);
        if (!c) continue;
        if (!tex_current_on_gpu(r.tex)) {
            end_pass();
            tex_ensure_current(r.tex, *F);                                // storage the game wrote since the image was made reaches the copy too
        }
        FbClone& fe = g_copies.fb_clone[r.tex];
        const uint32_t nlv = tex_levels(r.tex);
        // the levels the draw's view covers (a level outside it may stay stale: its generation says so when a later view covers it)
        uint32_t lo = 0, hi = nlv;
        if (r.has_view && r.view.has_levels) {
            lo = std::min<uint32_t>(static_cast<uint32_t>(std::max(0, static_cast<int>(r.view.min_level))), nlv);
            if (r.view.num_levels > 0) hi = std::min<uint32_t>(nlv, lo + static_cast<uint32_t>(r.view.num_levels));
        }
        const bool by_level = fb_levels_on() && nlv <= 16;
        if (!by_level) { lo = 0; hi = nlv; }
        std::vector<VkImageCopy> regions;
        uint64_t gens[16] = {};
        for (uint32_t lv = lo; lv < hi; ++lv) {
            if (by_level) {
                gens[lv] = tex_write_gen(r.tex, lv);
                if (gens[lv] != 0 && gens[lv] == fe.gen[lv]) continue;  // not written since it was copied
            }
            VkImageCopy rc = {};
            rc.srcSubresource = {tex_aspect(r.tex), lv, 0, tex_layers(r.tex)};
            rc.dstSubresource = {tex_aspect(c), lv, 0, tex_layers(c)};
            rc.extent = tex_image_extent(r.tex, lv);
            regions.push_back(rc);
        }
        g_copies.fb_use[r.tex] = c;
        rp_bump(ft::kCntEpFeedback);   // [nvn] set_reuse: what this texture resolves to changed
        if (regions.empty()) continue;                                    // the copy is current for every level the draw samples (no pass break)
        end_pass();
        tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   // the next pass moves it back to the attachment layout and loads it
        tex_transition(c, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(F->cb, tex_image(r.tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(c), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        tex_mark_gpu_written(c);
        tex_transition(c, *F, tex_sampled_layout(c));
        if (by_level)
            for (const VkImageCopy& rc : regions) fe.gen[rc.srcSubresource.mipLevel] = gens[rc.srcSubresource.mipLevel];
        rp_bump(ft::kCntEpFeedback);
        counters().copies.fetch_add(1, std::memory_order_relaxed);
        if (gpass_on()) {
            char what[24];
            if (regions.size() == hi - lo) std::snprintf(what, sizeof what, "copy");
            else std::snprintf(what, sizeof what, "copy %zul", regions.size());   // only that many of the sampled levels were copied
            feedback_label(r, what);
        }
    }
    // The draws before this one wrote the attachment it reads through the feedback loop layout: the pass ends (with a barrier) and the draw's
    // pass begins in that layout (ensure_pass). Each such draw gets its own pass, as each copy did.
    if (g_copies.fbl_want) {
        end_pass();
        if (loop_label.tex) feedback_label(loop_label, "loop");
    }
    g_copies.fb_seen = any;
}

static void vol3d_prepare(const ProgLayout& pl) {
    g_copies.vol3d_use.clear();
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        const TexResolved r = resolve_texture(d);
        if (!wants_vol3d(b, r) || g_copies.vol3d_use.count(r.tex) || !tex_gpu(r.tex)) continue;
        const uint32_t layer = r.has_view && r.view.has_layers ? static_cast<uint32_t>(r.view.min_layer) : 0;
        if (layer >= tex_layers(r.tex)) continue;
        Vol3dCopy& e = g_copies.vol3d[r.tex];
        if (e.src.get() != r.tex) e.src = Ref<Texture>::share(r.tex);
        e.last = F ? F->serial : 0;
        Ref<Texture>& c = e.tex;
        if (c && (c->b.width != r.tex->b.width || c->b.height != r.tex->b.height || c->fmt != r.tex->fmt || c->levels != r.tex->levels || e.layer != layer ||
                  (c->gpu && tex_scale(c.get()) != tex_scale(r.tex))))
            c = Ref<Texture>();
        if (!c) {
            Texture* n = new Texture();
            n->b = r.tex->b;
            n->b.target = NVN_TEXTURE_TARGET_3D;
            n->b.depth = 1;
            n->fmt = r.tex->fmt;
            n->levels = r.tex->levels;
            n->layers = 1;
            n->block_linear = r.tex->block_linear;
            n->cpu_dirty.store(false);
            static uint32_t next_id = 0x7e000000;
            n->id = next_id++;
            c = Ref<Texture>(n);
            e.layer = layer;
            e.copied = UINT64_MAX;
        }
        if (!tex_gpu_scaled(c.get(), tex_scale(r.tex))) continue;
        const uint64_t serial = F ? F->serial : 0;
        if (e.copied != serial || r.tex->cpu_dirty.load()) {
            out_texture_read(r.tex);
            end_pass();
            tex_ensure_current(r.tex, *F);
            tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tex_transition(c.get(), *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkImageCopy> regions;
            for (uint32_t lv = 0; lv < tex_levels(r.tex); ++lv) {
                VkImageCopy rc = {};
                rc.srcSubresource = {tex_aspect(r.tex), lv, layer, 1};
                rc.dstSubresource = {tex_aspect(c.get()), lv, 0, 1};
                rc.extent = tex_image_extent(r.tex, lv);
                rc.extent.depth = 1;
                regions.push_back(rc);
            }
            vkCmdCopyImage(F->cb, tex_image(r.tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(c.get()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
            tex_mark_gpu_written(c.get());
            tex_transition(c.get(), *F, tex_sampled_layout(c.get()));
            tex_transition(r.tex, *F, tex_sampled_layout(r.tex));   // the same draw may sample it as 2D too
            e.copied = serial;
            counters().copies.fetch_add(1, std::memory_order_relaxed);
            gstamp("3D copy of a 2D texture");
            if (first_time_key(kFirstVol3d, r.tex->id))
                NXLOG_I(kTag, "texture %u (%s %dx%d, target %d) is sampled as 3D by %s binding %s: it gets a 3D copy one slice deep", r.tex->id,
                        r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.target, d.shader->hash.c_str(), b.name.c_str());
        }
        g_copies.vol3d_use[r.tex] = c.get();
        rp_bump(ft::kCntEpVol3d);
    }
}

// NVN2 compute: a texture binding over the program's image slot n (apply_nvn2_compute_table) is the shader's size query (TXQ) on the image it
// writes (255 compute blocks: volumetric fog, light injection). Before, it was Unknown and the query saw the 1x1 dummy.
// - real view: a sampled view of the image with the binding's type (the image always has SAMPLED usage), read in GENERAL: the same dispatch
//   binds the image for storage (prepare_textures leaves it in GENERAL);
// - stand-in: the translator types these handles 2D (StaticGpuAccessor answers 2D for every compute handle), and a 2D view of a 3D image is no
//   sampled view without VK_EXT_image_2d_view_of_3d. Such a binding gets a black 2D (or 2D array) texture of the image's level size: the size
//   query sees the real x/y (the depth needs the translator to type the handle 3D, which then takes the real view).
static bool image_as_texture_real(const DescriptorBinding& b, const TexResolved& r) {
    return r.tex->b.target != NVN_TEXTURE_TARGET_3D || view_type_for(b) == VK_IMAGE_VIEW_TYPE_3D;
}

void image_as_texture(const BindingDesc& d, const DescriptorBinding& b, const TexResolved& r, VkDescriptorImageInfo& ii) {
    if (!r.tex || !tex_gpu(r.tex)) return;
    const ViewRequest q = make_request(b, r);
    if (image_as_texture_real(b, r)) {
        if (VkImageView v = tex_sampled_view(r.tex, q)) {
            ii.imageView = v;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            return;
        }
    }
    const VkExtent3D e = tex_extent(r.tex, q.base_level);
    const uint32_t layers = r.tex->b.target == NVN_TEXTURE_TARGET_3D ? e.depth : q.layer_count ? q.layer_count : tex_layers(r.tex) - std::min(q.base_layer, tex_layers(r.tex));
    VkImageView v = dummy_view_sized(q.type, e.width, e.height, layers);
    if (v) ii.imageView = v;
    if (first_time_key(kFirstImageSize, r.tex->id, d.shader->hash, b.name))
        NXLOG_I(kTag, "compute %s binding %s: size query on image %u (%s, target %d, %ux%ux%u) through a %s stand-in of %ux%u (x%u)", d.shader->hash.c_str(),
                b.name.c_str(), r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, e.width, e.height, e.depth, v ? "black" : "missing (dummy)",
                e.width, e.height, layers);
}

// Makes the textures a draw samples current (uploaded, in their sampled layout) and the storage images it binds writable (GENERAL).
// Ends the pass when it has to touch images. The depth attachment itself is left to the pass.
void prepare_textures(const ProgLayout& pl) {
    vol3d_prepare(pl);
    std::vector<Texture*> need, need_storage, need_general;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            TexResolved r = resolve_image(d);
            if (!r.tex || !tex_gpu(r.tex) || !tex_has_storage(r.tex)) continue;
            if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != VK_IMAGE_LAYOUT_GENERAL || tex_scale(r.tex) != 1.0f) need_storage.push_back(r.tex);
            continue;
        }
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source == NvnSource::ImageBinding) {     // the size query over a compute image: read in GENERAL, as the image binding
            TexResolved r = resolve_texture(d);
            if (!r.tex || !tex_gpu(r.tex) || !image_as_texture_real(b, r)) continue;
            if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != VK_IMAGE_LAYOUT_GENERAL) need_general.push_back(r.tex);
            continue;
        }
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        if (!tex_gpu(r.tex)) continue;
        // owned by the pass; a compute dispatch runs outside every pass, so the graphics attachments do not own what it samples
        if (shares_image(r.tex, S.depth_tex.get()) || (d.nvn_stage != kCompute && is_color_feedback(r.tex))) continue;
        if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != tex_sampled_layout(r.tex)) {
            need.push_back(r.tex);
            if (r.tex->cpu_dirty.load() && ft::on()) ft::mark(b.nvn.source == NvnSource::SamplerBinding ? "tx.dirty_bind" : "tx.dirty_uniform", r.tex->id);
        }
    }
    if (need.empty() && need_storage.empty() && need_general.empty()) return;
    for (Texture* t : need) out_texture_read(t);           // an upload reads game memory: compute results held there go back first
    for (Texture* t : need_storage) out_texture_read(t);
    for (Texture* t : need_general) out_texture_read(t);
    end_pass(true);     // layout changes and uploads follow: each carries the dependency for its own image
    counters().pass_breaks.fetch_add(1, std::memory_order_relaxed);
    for (Texture* t : need) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, tex_sampled_layout(t));
    }
    for (Texture* t : need_storage) {
        if (tex_scale(t) != 1.0f) tex_rescale(t, *F, 1.0f, "bound as a storage image");   // imageStore coordinates are not scaled (Ryujinx: ResScaleUnsupported)
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_GENERAL);
        tex_mark_gpu_written(t);
    }
    for (Texture* t : need_general) {                      // after the storage images: the same image is left in GENERAL either way
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_GENERAL);
    }
    gstamp("uploads/layouts");
}

void TexCopies::end_submit(uint64_t serial) {
    if ((serial & 127) == 0 && !fb_clone.empty()) {
        fb_use.clear();       // raw pointers into the copies: none is in use between two submissions
        for (auto it = fb_clone.begin(); it != fb_clone.end();) {
            if (serial > it->second.last + kFbIdleSubmits) it = fb_clone.erase(it);
            else ++it;
        }
        fb_clone_n.store(static_cast<int64_t>(fb_clone.size()), std::memory_order_relaxed);
    }
    if ((serial & 127) == 0 && !vol3d.empty()) {   // 3D copies of 2D textures no submission used for kFbIdleSubmits are released
        vol3d_use.clear();
        for (auto it = vol3d.begin(); it != vol3d.end();) {
            if (serial > it->second.last + kFbIdleSubmits) it = vol3d.erase(it);
            else ++it;
        }
    }
}

void TexCopies::shutdown() {
    fb_use.clear();
    fb_clone.clear();
    vol3d_use.clear();
    vol3d.clear();
    fb_clone_n.store(0, std::memory_order_relaxed);
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
