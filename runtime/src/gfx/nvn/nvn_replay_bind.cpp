// SPDX-License-Identifier: GPL-3.0-or-later
// Descriptor sets of the replay: the set memo, driver banks, set building per binding kind, render-scale support buffers, set binding.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
SetState g_sets;

void SetState::begin_submit(Frame& f) {
    cache.clear();
    // one support buffer per submission (Ryujinx SupportBuffer: render scales 1.0, no BGRA remap)
    support = f.upload(sizeof(SupportBuffer), 256);
    support_scaled.clear();
    driver_banks.clear();
    draw_support = UploadAlloc();
    if (support) {
        SupportBuffer sb = support_buffer_defaults();
        std::memcpy(support.ptr, &sb, sizeof sb);
    }
}

// The scale nxrt/gfx_tuning.h asks for on uniform block `idx` of this binding's shader (1 = none): only the chosen fragment shader's block.
static float ufs_scale_for(const BindingDesc& d, int idx) {
    const nxrt::gfx::Tuning& t = nxrt::gfx::tuning();
    const uint64_t want = t.ufs_hash.load(std::memory_order_relaxed);
    if (!want || d.nvn_stage != kFragment || idx != t.ufs_block.load(std::memory_order_relaxed)) return 1.0f;
    const float sc = t.ufs_scale.load(std::memory_order_relaxed);
    if (sc == 1.0f || !d.shader || std::strtoull(d.shader->hash.c_str(), nullptr, 16) != want) return 1.0f;
    return sc;
}

// Descriptor set number `set` of the bound program: the one of the last draw (SetMemo below), a set of this submission with the same contents (g_sets.cache), or a new one.
// Render thread only (scratch arrays below are reused).
// [nvn] set_reuse: the inputs of the last set built for each set number. A set whose bindings are all sampled textures (combined image
// samplers by binding or by handle in a uniform block) resolves to the same descriptors as long as the program, the handles, the pools,
// the replay epoch (g_sets.epoch: passes, render targets, feedback and 3D copies, GPU writes, submissions), be::g_bind_gen (pool
// registrations, texture images made, replaced or released) and the scene upscaler's override are the same: such a draw takes the set of
// the one before without resolving every texture again (about 45 % of the texture sets of a frame are the one before, typically a
// per-object uniform block changed between two draws of one material). Texel buffer, storage and uniform bindings are never reused
// (their resolution has side effects or reads pool copies that change inside a submission).
struct SetMemo {
    const ProgLayout* pl = nullptr;
    uint64_t epoch = 0, gen = 0;
    const void* tpool = nullptr;
    const void* spool = nullptr;
    const void* su_tex = nullptr;
    VkImageView su_view = VK_NULL_HANDLE;
    uint64_t su_near = 0;
    uint32_t n = 0;
    uint64_t h[32] = {};
    VkDescriptorSet ds = VK_NULL_HANDLE;
};
static SetMemo g_set_memo[4];
// The handle a sampled texture binding names: S.tex of its stage, or the word in a uniform block's memory. False for other sources.
static bool sampler_handle(const BindingDesc& d, const DescriptorBinding& b, uint64_t* handle) {
    if (b.nvn.source == NvnSource::SamplerBinding) {
        *handle = b.nvn.index >= 0 && b.nvn.index < 64 ? S.tex[d.nvn_stage][b.nvn.index] : 0;
        return true;
    }
    if (b.nvn.source == NvnSource::HandleInUniform) {
        *handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
        return true;
    }
    return false;
}
// The Nearest part of the scene upscaler's override as one word (count and textures).
static uint64_t su_near_key() {
    uint64_t k = g_su.near_n;
    for (uint32_t i = 0; i < g_su.near_n; ++i) k = fnv_step(k, reinterpret_cast<uint64_t>(g_su.near[i]));
    return k;
}
// Fills m with the current inputs of set set of pl; false when the set cannot be reused (a binding of another kind, too many bindings).
static bool set_memo_inputs(const ProgLayout& pl, uint32_t set, SetMemo& m) {
    m.pl = &pl;
    m.epoch = g_sets.epoch;
    m.gen = be::g_bind_gen.load(std::memory_order_relaxed);
    m.tpool = S.tpool.get();
    m.spool = S.spool.get();
    m.su_tex = g_su.tex;
    m.su_view = g_su.view;
    m.su_near = su_near_key();
    m.n = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || m.n >= 32) return false;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        uint64_t handle = 0;
        if (b.nvn.source == NvnSource::ImageBinding) {
            if (b.nvn.index >= 0 && b.nvn.index < 16) handle = S.img[d.nvn_stage][b.nvn.index];
        } else if (!sampler_handle(d, b, &handle)) {
            return false;
        }
        m.h[m.n++] = handle;
    }
    return m.n > 0;
}
static bool set_memo_same(const SetMemo& a, const SetMemo& b) {
    return a.pl == b.pl && a.epoch == b.epoch && a.gen == b.gen && a.tpool == b.tpool && a.spool == b.spool && a.su_tex == b.su_tex &&
           a.su_view == b.su_view && a.su_near == b.su_near && a.n == b.n && std::memcmp(a.h, b.h, sizeof(uint64_t) * a.n) == 0;
}
// c[0] of a stage that reads it, filled from the current bindings (nvn_driver_bank.h): a Switch 2 compute program's resource table
// (storage buffer sizes for the bounds checks of the light culling, GPU particle and volume field kernels); a graphics stage's storage
// buffer table, point size and the draw's base instance. Copied into the upload ring once per distinct content per submission. A stage
// the fill does not cover (Switch 1 compute) gets the shared bank (zeros, point size).
static VkDescriptorBufferInfo driver_bank_for(const BindingDesc& d) {
    VkDeviceSize range = 0;
    VkBuffer shared = driver_constants_buffer(&range);
    VkDescriptorBufferInfo bi = {shared ? shared : dummy_buffer(), 0, shared ? range : 65536};
    const ShaderControl& c = d.shader->control;
    const bool compute = d.nvn_stage == kCompute;
    if (compute && (!c.nvn2 || c.stage != 5)) return bi;
    if (d.nvn_stage < 0 || d.nvn_stage > kCompute) return bi;
    uint8_t bank[kDriverBankBytes];
    DriverRange ubo[16], ssbo[16];
    for (int n = 0; n < 16; ++n) {
        ubo[n] = {S.ubo[d.nvn_stage][n].addr, S.ubo[d.nvn_stage][n].size};
        ssbo[n] = {S.ssbo[d.nvn_stage][n].addr, S.ssbo[d.nvn_stage][n].size};
    }
    const uint32_t used = compute ? fill_driver_bank_nvn2_compute(bank, c, ubo, ssbo, S.tex[kCompute], S.img[kCompute])
                                  : fill_driver_bank_graphics(bank, d.nvn_stage, ssbo, g_sets.draw_base_instance);
    uint64_t key = kFnvBasisShort ^ used;
    for (uint32_t i = 0; i < used; ++i) key = fnv_step(key, bank[i]);
    auto it = g_sets.driver_banks.find(key);
    if (it == g_sets.driver_banks.end()) {
        const VkDeviceSize align = std::max<VkDeviceSize>(device().properties().limits.minUniformBufferOffsetAlignment, 256);
        UploadAlloc u = F->upload(kDriverBankBytes, align);
        if (!u) return bi;
        std::memcpy(u.ptr, bank, kDriverBankBytes);
        it = g_sets.driver_banks.emplace(key, u).first;
    }
    return {it->second.buffer, it->second.offset, kDriverBankBytes};
}

// The descriptor of a sampled texture binding (a combined image sampler) from the current state: the view, layout and sampler of the
// texture its handle names, after the feedback copy, the 3D copy and the scene upscaler's override; the dummy where it has none.
// *plain: it is the texture's own view in its sampled layout (no copy stands in, the texture is no attachment of the pass, no depth
// stand-in, no dummy): the case TexDescCache keeps.
static VkDescriptorImageInfo sampled_descriptor(const BindingDesc& d, const DescriptorBinding& b, bool* plain) {
    *plain = false;
    bool own = true;                                       // r.tex is the texture the handle names
    VkDescriptorImageInfo ii = {dummy_sampler(), dummy_view(view_type_for(b), b.image.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    TexResolved r = resolve_texture(d);
    if (b.nvn.source == NvnSource::ImageBinding) {
        image_as_texture(d, b, r, ii);
        r.tex = nullptr;                                   // none of the texture paths below apply
    }
    if (r.tex) {
        const auto fb = g_copies.fb_use.find(r.tex);
        if (fb != g_copies.fb_use.end()) {                 // a feedback draw samples the copy made before it
            r.tex = fb->second;
            own = false;
        }
    }
    if (r.tex && wants_vol3d(b, r)) {                      // a sampler3D over 2D storage samples the 3D copy (vol3d_prepare)
        own = false;
        const auto vc = g_copies.vol3d_use.find(r.tex);
        if (vc != g_copies.vol3d_use.end()) {
            r.tex = vc->second;
            r.view.has_layers = false;                     // the copy is the chosen layer, one slice deep
        }
    }
    // [nvn] debug_null_format = "BPTC_UFLOAT" or "RGBA8:256x64": every sampled texture of that NVN format (and size) gets the dummy view
    // (term isolation, debugging only)
    static const std::string null_fmt = nxrt::config_get_str("nvn", "debug_null_format", "");
    if (r.tex && !null_fmt.empty() && r.tex->fmt) {
        const std::string sized = std::string(r.tex->fmt->name) + ":" + std::to_string(r.tex->b.width) + "x" + std::to_string(r.tex->b.height);
        if (null_fmt == r.tex->fmt->name || null_fmt == sized) r.tex = nullptr;
    }
    // a compute dispatch is never inside the graphics pass: a colour attachment of the last pass is an ordinary texture to it
    const bool feedback = r.tex && d.nvn_stage != kCompute && is_color_feedback(r.tex);
    const bool loop = feedback && fbl_sampled(r.tex);      // sampled in the feedback loop layout of the open pass (feedback_prepare)
    // a shader that samples with depth comparison (sampler2DShadow) needs a depth-format image: the game leaves colour textures in some
    // of those slots, which Vulkan does not allow (VUID-vkCmdDrawIndexed-None-06479). They get the depth dummy instead.
    const bool shadow_mismatch = r.tex && b.image.depth && !tex_is_depth(r.tex);
    if (shadow_mismatch) {   // the stand-in holds the texture's red value (dcmp_view), the console compares against it
        log_once(kOnceShadowColour, "a depth-comparison sampler is bound to a colour texture: a depth stand-in with its red value is bound");
        if (VkImageView sv = dcmp_view(r.tex, view_type_for(b))) ii.imageView = sv;
        if (r.has_sampler) ii.sampler = sampler_for(r.smp);
    }
    if (r.tex && tex_gpu(r.tex) && (!feedback || loop) && !shadow_mismatch) {
        VkImageView v = tex_sampled_view(r.tex, make_request(b, r));
        if (v) {
            ii.imageView = v;
            ii.imageLayout = loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : tex_sampled_layout(r.tex);
            if (r.has_sampler) ii.sampler = sampler_for(r.smp, r.tex);
            if (r.tex == g_su.tex && g_su.view) {            // the upscaled scene replaces the small one for this draw (S14)
                ii.imageView = g_su.view;
                ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            } else if (g_su.nearest(r.tex)) {                 // Nearest: the small scene is enlarged unfiltered (nearest, clamp to edge)
                ii.sampler = dummy_sampler();
            }
            *plain = own && !feedback;
        } else if (first_time_key(kFirstDummyView, r.tex->id, d.shader->hash, b.name)) {
            NXLOG_W(kTag, "sampled texture %u (%s, target %d, %dx%dx%d) has no view of the shader's type %d: shader %s binding %s (NVN slot %d, set %u binding %u) gets the dummy view",
                    r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, r.tex->b.width, r.tex->b.height, r.tex->b.depth,
                    static_cast<int>(view_type_for(b)), d.shader->hash.c_str(), b.name.c_str(), b.nvn.index, d.set, d.binding);
        }
    } else if (feedback) {
        log_once(kOnceColorFeedback, "a texture is sampled while it is a colour attachment of the pass: the dummy texture is bound");
        static int reported = 0;
        if (reported < 40) {   // which draw: the texture, the attachment's subresource, the sampled view, the shaders (S4g)
            ++reported;
            char tb[96];
            uint32_t lvl = 0, lay = 0, nlay = 0;
            for (uint32_t i = 0; i < S.ncolor; ++i) if (S.color_tex[i].get() == r.tex) { lvl = S.color_level[i]; lay = S.color_layer[i]; nlay = S.color_layers[i]; }
            NXLOG_I("nvn.replay", "feedback: %s binding %s vs %s fs %s | attachment level %u layer %u layers %u | sampled view levels %d+%d layers %d+%d", tex_desc(r.tex, tb, sizeof tb),
                    b.name.c_str(), S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-", lvl, lay, nlay,
                    r.has_view && r.view.has_levels ? r.view.min_level : -1, r.has_view && r.view.has_levels ? r.view.num_levels : -1,
                    r.has_view && r.view.has_layers ? r.view.min_layer : -1, r.has_view && r.view.has_layers ? r.view.num_layers : -1);
        }
    }
    return ii;
}

// [nvn] set_reuse, per binding: the descriptor of a plain sampled texture, by handle and binding shape. sampled_descriptor is a function
// of the handle, the binding's shape (view type, depth comparison, component type, compute stage or not) and the inputs of the SetMemo
// above: the pools and their entries and the texture images (be::g_bind_gen), the render targets, the open pass and the feedback and 3D
// copies (g_sets.epoch), the scene upscaler's override. While those stand, a binding whose handle and shape were resolved before takes
// that descriptor without resolving the texture, its view and its sampler again: a material that comes back after another one (the
// SetMemo only knows the set before), and the textures many materials share. Only the plain case is kept (sampled_descriptor), so a
// slot never depends on which copies a draw uses; a slot is written only when the state and the handle are still what they were before
// the resolve (making an image inside it raises g_bind_gen).
struct TexDescSlot {
    uint64_t stamp = 0, handle = 0;
    uint32_t shape = 0;
    VkDescriptorImageInfo ii = {};
};
struct TexDescCache {
    static constexpr size_t kSlots = 512;                  // direct mapped
    TexDescSlot slot[kSlots];
    uint64_t stamp = 1;                                    // slots of another stamp are empty
    uint64_t epoch = 0, gen = 0, su_near = 0;              // the state the slots of `stamp` were filled under
    const void* tpool = nullptr;
    const void* spool = nullptr;
    const void* su_tex = nullptr;
    VkImageView su_view = VK_NULL_HANDLE;
};
static TexDescCache g_tdc;
// The stamp of the slots that hold for the current state (a new one when it changed).
static uint64_t tdc_stamp() {
    TexDescCache& c = g_tdc;
    const uint64_t gen = be::g_bind_gen.load(std::memory_order_acquire), su_near = su_near_key();
    if (c.epoch != g_sets.epoch || c.gen != gen || c.tpool != S.tpool.get() || c.spool != S.spool.get() || c.su_tex != g_su.tex || c.su_view != g_su.view ||
        c.su_near != su_near) {
        c.epoch = g_sets.epoch;
        c.gen = gen;
        c.tpool = S.tpool.get();
        c.spool = S.spool.get();
        c.su_tex = g_su.tex;
        c.su_view = g_su.view;
        c.su_near = su_near;
        ++c.stamp;
    }
    return c.stamp;
}
static VkDescriptorImageInfo sampled_descriptor_cached(const BindingDesc& d, const DescriptorBinding& b) {
    static const bool reuse_on = set_reuse_on();
    bool plain = false;
    uint64_t handle = 0;
    if (!reuse_on || g_trace || !sampler_handle(d, b, &handle)) return sampled_descriptor(d, b, &plain);
    const uint32_t shape = static_cast<uint32_t>(view_type_for(b)) | (b.image.depth ? 8u : 0u) | (d.nvn_stage == kCompute ? 16u : 0u) |
                           static_cast<uint32_t>(b.image.component) << 8;
    const uint64_t stamp = tdc_stamp();
    TexDescSlot& sl = g_tdc.slot[((handle ^ static_cast<uint64_t>(shape) << 40) * 0x9e3779b97f4a7c15ull) >> 55];
    if (sl.stamp == stamp && sl.handle == handle && sl.shape == shape) return sl.ii;
    const VkDescriptorImageInfo ii = sampled_descriptor(d, b, &plain);
    uint64_t again = 0;
    if (plain && tdc_stamp() == stamp && sampler_handle(d, b, &again) && again == handle) sl = {stamp, handle, shape, ii};
    return ii;
}

// [nvn] storage_ro (default on): a storage buffer bound to a block that no instruction of its shader can write
// (DescriptorBinding::may_write, proved on the SPIR-V by reflect_spirv) is bound as before but not treated as a GPU write: no
// gpu_wrote (the record a copy pool lands in guest memory at completion, the replay epoch, the stale pool copy pages), and a compute
// shadow of it is not copied back. false: every storage buffer binding counts as written, as before.
static bool storage_ro_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "storage_ro", true);
        NXLOG_I(kTag, "storage buffers: %s ([nvn] storage_ro)", v ? "a block its shader never writes is not recorded as a GPU write" : "every binding is recorded as a GPU write");
        return v;
    }();
    return on;
}
static nxrt::GaugeCounter g_gauge_storage_ro("nvn.cnt.storage_ro_binds");   // storage buffer bindings not recorded as writes (cumulative)

static VkDescriptorSet build_set_resolved(const ProgLayout& pl, uint32_t set);
static VkDescriptorSet build_set(const ProgLayout& pl, uint32_t set) {
    static const bool reuse_on = set_reuse_on();
    if (!reuse_on || g_trace || g_shadowing) return build_set_resolved(pl, set);
    SetMemo now;
    if (!set_memo_inputs(pl, set, now)) return build_set_resolved(pl, set);
    SetMemo& last = g_set_memo[set & 3];
    if (last.ds && set_memo_same(now, last)) {
        rp_count(kRpReuse);
        ft::split_count(ft::kCntSetHit);
        return last.ds;
    }
    ft::split_count(ft::kCntSetMiss);
    now.ds = build_set_resolved(pl, set);
    last = now;
    return now.ds;
}

// The set resolved from the current state: a set of this submission with the same contents (g_sets.cache), or a new one.
static VkDescriptorSet build_set_resolved(const ProgLayout& pl, uint32_t set) {
    RpScope rp_(kRpSetBuild);
    RpScope rps_(kRpSet0 + static_cast<int>(set & 3));
    // reused across calls (no heap allocation per draw); the pointers the writes keep stay valid: capacity is reserved for every binding
    static std::vector<VkWriteDescriptorSet> writes;
    static std::vector<VkDescriptorBufferInfo> binfo;
    static std::vector<VkDescriptorImageInfo> iinfo;
    static std::vector<VkBufferView> tinfo;
    writes.clear();
    binfo.clear();
    iinfo.clear();
    tinfo.clear();
    const size_t nb = pl.bindings.size();
    writes.reserve(nb);
    binfo.reserve(nb);
    iinfo.reserve(nb);
    tinfo.reserve(nb);
    uint64_t key = kFnvBasisShort + set;
    auto mix = [&](uint64_t v) { key = fnv_step(key, v); };
    mix(reinterpret_cast<uint64_t>(pl.layout));
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = d.binding;
        w.descriptorCount = 1;
        w.descriptorType = d.type;
        PoolReasonScope pr_(d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? kReasonUniform : d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? kReasonStorage :
                            d.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ? kReasonTexel : pool_reason());
        if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            RpScope rpb_(kRpBuf, g_rp.fine);
            VkDescriptorBufferInfo bi = {dummy_buffer(), 0, 65536};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer:
                    bi = g_sets.draw_support ? VkDescriptorBufferInfo{g_sets.draw_support.buffer, g_sets.draw_support.offset, sizeof(SupportBuffer)}
                                        : VkDescriptorBufferInfo{g_sets.support.buffer, g_sets.support.offset, sizeof(SupportBuffer)};
                    break;
                case NvnSource::DriverConstants:
                    bi = driver_bank_for(d);
                    break;
                case NvnSource::ConstantData: {
                    VkDeviceSize range = 0;
                    VkBuffer buf = constant_data_buffer(d.shader, &range);
                    if (buf) bi = {buf, 0, range};
                    break;
                }
                case NvnSource::UniformBlock: {
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        if (d.nvn_stage == kCompute || !out_lazy()) {                   // draws: checked before the op (out_before_op)
                            const VkDeviceSize ua = device().properties().limits.minUniformBufferOffsetAlignment;
                            out_draw_read(bind.addr, bind.size ? bind.size : 65536, ua && (bind.addr % ua) == 0, kOutDrawBuffer);   // resolve_read serves the copy
                        }
                        BufRef br = resolve_read(bind.addr, bind.size, kReadUniform);
                        const VkDeviceSize align = device().properties().limits.minUniformBufferOffsetAlignment;
                        const float ufs = ufs_scale_for(d, idx);
                        const bool tsr = d.nvn_stage == g_tsr.stage && idx == g_tsr.slot && bind.size >= g_tsr.bytes && extras::temporal_wanted() && S.depth_tex;
                        if (br.ok && (ufs != 1.0f || tsr)) {
                            // the game's scale on some floats of this block (nxrt/gfx_tuning.h): bound from a patched copy in the upload ring
                            const VkDeviceSize n = std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536});
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                if (tsr && n >= g_tsr.bytes) tsr_jitter(reinterpret_cast<uint32_t*>(u.ptr), static_cast<size_t>(n / 4));   // temporal upscaling
                                const uint32_t mask = ufs != 1.0f ? nxrt::gfx::tuning().ufs_mask.load(std::memory_order_relaxed) : 0u;
                                for (uint32_t i = 0; i < 32; ++i) {
                                    if (!(mask & (1u << i)) || (i + 1) * 4 > n) continue;
                                    float v;
                                    std::memcpy(&v, static_cast<uint8_t*>(u.ptr) + i * 4, 4);
                                    v *= ufs;
                                    std::memcpy(static_cast<uint8_t*>(u.ptr) + i * 4, &v, 4);
                                }
                                bi = {u.buffer, u.offset, n};
                            }
                        } else if (br.ok && (br.offset % align) == 0) {
                            bi = {br.buffer, br.offset, std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536})};
                        } else if (br.ok) {
                            // unaligned binding: copy into the upload ring
                            const VkDeviceSize n = std::min<VkDeviceSize>(bind.size ? bind.size : 65536, 65536);
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                bi = {u.buffer, u.offset, n};
                            }
                        }
                    }
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);   // NVN2 compute: index from the c[0] table
                    if (slot >= 0 && slot < 16 && S.ssbo[d.nvn_stage][slot].addr) {
                        const GState::Bind& bind = S.ssbo[d.nvn_stage][slot];
                        BufRef br = resolve_buffer(bind.addr, bind.size);
                        bi = {br.buffer, br.offset, std::min<VkDeviceSize>(bind.size ? bind.size : br.avail, br.avail)};
                        const bool writes = b.may_write || !storage_ro_on();        // false: no instruction of the shader can write the block
                        if (!writes && br.ok) g_gauge_storage_ro.add(1);
                        if (!g_shadowing && br.ok && out_lazy() && out_overlaps(bind.addr, bi.range)) {   // a draw over a held compute result: the copy
                            const BufRef hb = held_or_pool(bind.addr, bi.range, kOutStorageXfb, false);
                            if (hb.buffer != br.buffer) {
                                bi.buffer = hb.buffer;
                                bi.offset = hb.offset;
                                if (writes) {
                                    gpu_wrote(bind.addr, bi.range, kWriteStorage);
                                    out_add(bind.addr, bi.range, hb.buffer, hb.offset, br);   // what the draw writes there is held like a dispatch's
                                }
                                break;
                            }
                        }
                        if (g_shadowing && br.ok) {                                  // before gpu_wrote: the shadow reads the pool copy while valid
                            const BufRef sh = shadow_of(bind.addr, bi.range, br, writes);
                            bi.buffer = sh.buffer;
                            bi.offset = sh.offset;
                        }
                        if (br.ok && writes) gpu_wrote(bind.addr, bi.range, kWriteStorage);   // the shader may write the block
                    }
                    break;
                }
                case NvnSource::UniformAsStorage: {
                    // a compute shader that reads uniform block n through its c[0] descriptor: the block's buffer as a storage buffer
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        const BufRef br0 = resolve_buffer(bind.addr, bind.size);
                        const VkDeviceSize range = std::min<VkDeviceSize>(bind.size ? bind.size : br0.avail, br0.avail);
                        const BufRef br = held_or_pool(bind.addr, range, kOutDispatch, true);
                        bi = {br.buffer, br.offset, range};
                    }
                    break;
                }
                default:
                    note_unsupported("shader buffer binding of an unknown NVN source");
                    break;
            }
            binfo.push_back(bi);
            w.pBufferInfo = &binfo.back();
            mix(reinterpret_cast<uint64_t>(bi.buffer)); mix(bi.offset); mix(bi.range);
        } else if (d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            RpScope rpt_(kRpTex, g_rp.fine);
            const VkDescriptorImageInfo ii = sampled_descriptor_cached(d, b);
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView));
            mix(reinterpret_cast<uint64_t>(ii.sampler));
            mix(static_cast<uint64_t>(ii.imageLayout));
        } else if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const VkFormat shader_fmt = image_format_vk(b.image.format);
            VkDescriptorImageInfo ii = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            TexResolved r = resolve_image(d);
            if (r.tex && tex_gpu(r.tex) && tex_has_storage(r.tex)) {
                ViewRequest q = make_request(b, r);
                q.swizzle = {};
                if (shader_fmt != VK_FORMAT_UNDEFINED) q.format = shader_fmt;
                q.format = tex_storage_format(r.tex, q.format);   // an sRGB image is stored to through its UNORM view (raw bits, as on the console)
                ii.imageView = tex_sampled_view(r.tex, q);
            }
            if (!ii.imageView) {
                if (r.tex && first_time_key(kFirstStorageView, r.tex->id))   // once per texture, not per draw
                    NXLOG_W(kTag, "storage image: texture %u (%s %dx%dx%d) has no storage view (gpu %d, storage usage %d)", r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?",
                            r.tex->b.width, r.tex->b.height, r.tex->b.depth, tex_gpu(r.tex) ? 1 : 0, tex_has_storage(r.tex) ? 1 : 0);
                log_once(kOnceStorageImageDummy, "a storage image binding has no usable texture: a dummy storage image is bound");
                ii.imageView = dummy_storage_view(view_type_for(b), shader_fmt);
            }
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView));
        } else if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            RpScope rpx_(kRpTexel, g_rp.fine);
            // NVN BUFFER-target textures (samplerBuffer / imageBuffer): a texel buffer view over the pool memory that holds the texels
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const VkFormat shader_fmt = storage ? image_format_vk(b.image.format) : VK_FORMAT_UNDEFINED;
            const int component = storage ? 0 : b.image.component == ScalarType::Float ? 1 : b.image.component == ScalarType::Sint ? 2
                                  : b.image.component == ScalarType::Uint ? 3 : 0;
            TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
            uint64_t addr = 0, bytes = 0;
            VkFormat vfmt = VK_FORMAT_UNDEFINED;
            VkBufferView v = r.tex ? tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, component, shader_fmt, storage, &addr, &bytes, &vfmt) : VK_NULL_HANDLE;
            // a 2D/3D texture the game left in a samplerBuffer slot (e.g. KTGL's 8x8 default texture 3 in the foliage wind shader's
            // interaction buffers): read as a run of texels over its storage, as the console does, instead of the dummy
            if (v && r.tex->b.target != NVN_TEXTURE_TARGET_BUFFER && first_time_key(kFirstTexelNonBuf, r.tex->id, d.shader->hash, b.name))
                NXLOG_I(kTag, "texture %u (%s %dx%dx%d, target %d) is read as a texel buffer by %s binding %s: it gets a buffer view over its storage (%llu bytes)",
                        r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.depth, r.tex->b.target,
                        d.shader->hash.c_str(), b.name.c_str(), static_cast<unsigned long long>(bytes));
            if (v && g_shadowing) {                                        // before gpu_wrote: the shadow reads the pool copy while valid
                const BufRef host = resolve_buffer(addr, bytes);
                const BufRef sh = shadow_of(addr, bytes, host, storage);
                if (sh.buffer != host.buffer)
                    if (VkBufferView sv = make_buffer_view(sh.buffer, sh.offset, bytes, vfmt)) v = sv;   // offset: non-zero in the pool copy
            }
            if (v && !g_shadowing && out_lazy() && out_overlaps(addr, bytes)) {   // a draw reads a held compute result as texels: from the copy
                const MirrorSpan ms = out_servable(addr, bytes) ? mirror_span(addr, bytes) : MirrorSpan{};
                VkBufferView mv = ms.buf ? make_buffer_view(ms.buf, ms.offset, bytes, vfmt) : VK_NULL_HANDLE;
                if (mv) v = mv;
                else log_once(kOnceOutTexel, "a draw reads a compute result held in the pool copy as a texel buffer the copy cannot serve: it reads the pool");
            }
            if (v && storage) gpu_wrote(addr, bytes, kWriteStorage);      // an image buffer may be written by the shader
            if (!v) {
                if (r.tex && first_time_key(kFirstTexelBuffer, r.tex->id, d.shader->hash, b.name))
                    NXLOG_W(kTag, "texel buffer: texture %u (%s, target %d, %dx%dx%d, storage %llu bytes at %p) has no buffer view: shader %s binding %s (NVN slot %d, set %u binding %u)",
                            r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, r.tex->b.width, r.tex->b.height, r.tex->b.depth,
                            static_cast<unsigned long long>(r.tex->storage_size), static_cast<void*>(r.tex->mem), d.shader->hash.c_str(), b.name.c_str(),
                            b.nvn.index, d.set, d.binding);
                log_once(kOnceTexelBufferDummy, "a texel buffer binding has no usable buffer texture: a dummy buffer view is bound");
                v = dummy_buffer_view(shader_fmt, storage);
            }
            if (!v) {
                note_unsupported("texel buffer descriptor without any view (the binding stays unwritten)");
                continue;
            }
            tinfo.push_back(v);
            w.pTexelBufferView = &tinfo.back();
            mix(reinterpret_cast<uint64_t>(v));
        } else {
            note_unsupported("descriptor of an unhandled type (the binding stays unwritten)");
            continue;
        }
        writes.push_back(w);
    }
    if (ft::on()) {   // profile: the same contents as the last build of this set number
        static uint64_t last[4];
        if (last[set & 3] == key) rp_count(kRpSame0 + static_cast<int>(set & 3));
        last[set & 3] = key;
    }
    auto it = g_sets.cache.find(key);
    if (it != g_sets.cache.end()) return it->second;
    RpScope rpw_(kRpSetWrite);
    rp_count(kRpMiss0 + static_cast<int>(set & 3));
    VkDescriptorSet ds = F->alloc_set(pl.sets[set]);
    if (!ds) return VK_NULL_HANDLE;
    for (auto& w : writes) w.dstSet = ds;
    vkUpdateDescriptorSets(vkdev(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    g_sets.cache[key] = ds;
    return ds;
}

// ---- resolution scale: the support buffer's render_scale for a draw or dispatch (Ryujinx TextureBindingsManager.UpdateRenderScale) ----
// The translator scales texelFetch / imageLoad coordinates by render_scale[1 + i] and divides textureSize and gl_FragCoord.xy by the scale
// of texture i and of the render target ([0]). Texture i is the shader's i-th texture descriptor: the harness numbers single textures
// with one counter per shader (binding = stage slot * 128 + counter, + 64 for texel buffers), images follow the textures. Vertex shaders
// read their scales after the fragment shader's (frag_scale_count). Storage images are never scaled (tex_rescale), so images read 1.
void support_for_draw(const ProgLayout& pl, bool compute) {
    g_sets.draw_support = UploadAlloc();
    if (render_scale() == 1.0f && gfx::tuning().shadow_scale.load(std::memory_order_relaxed) == 1.0f) return;
    float sc[73];
    for (float& v : sc) v = 1.0f;
    int frag_count = 0;
    bool any = false;
    if (!compute && P.scale != 1.0f) { sc[0] = P.scale; any = true; }
    // the fragment shader's textures first, then the vertex shader's after them
    const ShaderInfo* fs = compute ? nullptr : S.stage[kFragment];
    const ShaderInfo* vs = compute ? S.stage[kCompute] : S.stage[kVertex];
    auto tex_count = [](const ShaderInfo* sh) {
        int n = 0;
        for (const DescriptorBinding& b : sh->refl.bindings)
            if (b.count == 1 && (b.type == DescriptorType::CombinedImageSampler || b.type == DescriptorType::SampledImage || b.type == DescriptorType::UniformTexelBuffer ||
                                 b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer))
                ++n;
        return n;
    };
    if (fs) frag_count = tex_count(fs);
    // the native size of the render target, for the fragment rule below
    Texture* rt = S.ncolor && S.color_tex[0] ? S.color_tex[0].get() : S.depth_tex.get();
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.count != 1 || b.binding >= 640) continue;
        const bool is_fs = d.shader == fs;
        if (!is_fs && d.shader != vs) continue;
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        const auto fb = g_copies.fb_use.find(r.tex);
        if (fb != g_copies.fb_use.end()) r.tex = fb->second;
        float s = tex_scale(r.tex);
        if (s == 1.0f) continue;
        const int idx = static_cast<int>(b.binding % 64u);
        const int slot = is_fs ? 1 + idx : (compute ? 1 + idx : 1 + frag_count + idx);
        if (slot >= 73) continue;
        if (is_fs && rt) {
            // Ryujinx: a texture whose size is the render target's size times one factor on both axes interpolates the scaled pixels
            // with gl_FragCoord (a negative scale tells TexelFetchScale to)
            const float rw = static_cast<float>(rt->b.width), rh = static_cast<float>(rt->b.height);
            if (r.tex->b.width > 0 && r.tex->b.height > 0 && rw / static_cast<float>(r.tex->b.width) == rh / static_cast<float>(r.tex->b.height)) s = -s;
        }
        sc[slot] = s;
        any = true;
    }
    if (!any) return;
    uint64_t key = kFnvBasisShort + static_cast<uint64_t>(frag_count);
    for (float v : sc) { uint32_t u; std::memcpy(&u, &v, 4); key = fnv_step(key, u); }
    auto it = g_sets.support_scaled.find(key);
    if (it != g_sets.support_scaled.end()) { g_sets.draw_support = it->second; return; }
    UploadAlloc u = F->upload(sizeof(SupportBuffer), 256);
    if (!u) return;
    SupportBuffer sb = support_buffer_defaults();
    sb.frag_scale_count[0] = frag_count;
    for (int i = 0; i < 73; ++i) sb.render_scale[i][0] = sc[i];
    std::memcpy(u.ptr, &sb, sizeof sb);
    g_sets.support_scaled[key] = u;
    g_sets.draw_support = u;
}

void bind_sets(const ProgLayout& pl, VkPipelineBindPoint bp) {
    if (!bind_coalesce_on()) {
        for (uint32_t set = 0; set < 4; ++set) {
            if (!pl.has_set[set]) continue;
            VkDescriptorSet ds = build_set(pl, set);
            RpScope rp_(kRpSetBind);
            if (ds) vkCmdBindDescriptorSets(F->cb, bp, pl.layout, set, 1, &ds, 0, nullptr);
        }
        return;
    }
    // one call per run of consecutive sets
    VkDescriptorSet run[4];
    uint32_t first = 0, n = 0;
    auto flush = [&] {
        if (!n) return;
        RpScope rp_(kRpSetBind);
        vkCmdBindDescriptorSets(F->cb, bp, pl.layout, first, n, run, 0, nullptr);
        if (n > 1) ft::split_count(ft::kCntMerged, n - 1);
        n = 0;
    };
    for (uint32_t set = 0; set < 4; ++set) {
        if (!pl.has_set[set]) { flush(); continue; }
        VkDescriptorSet ds = build_set(pl, set);
        if (!ds) { flush(); continue; }
        if (!n) first = set;
        run[n++] = ds;
    }
    flush();
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
