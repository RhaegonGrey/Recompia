// SPDX-License-Identifier: GPL-3.0-or-later
// Texture uploads: tex_ensure_current, the GPU untile of block-linear storage, promoted-format uploads, zero fill.
#include "nvn_gpu_texture_internal.h"

namespace nxrt::nvn::be {

namespace gpu {

namespace tex {
// The image gets zeros (transparent black). Block formats are filled from a zeroed staging buffer; uncompressed ones are cleared.
static void fill_zero(Frame& f, TexGpu& g) {
    if (!(g.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    if (is_block_format(g.format)) {
        uint32_t bw = 0, bh = 0, bytes = 0;
        if (g.transcoded) {
            bw = 4; bh = 4; bytes = 16;
        } else if (g.fmt && g.format == g.fmt->vk) {
            bw = g.fmt->block_w; bh = g.fmt->block_h; bytes = g.fmt->bytes;
        } else {
            return;
        }
        std::vector<VkBufferImageCopy> regions;
        uint64_t total = 0;
        for (uint32_t l = 0; l < g.levels; ++l) {
            const VkExtent3D e = level_extent(g.base, l);
            const uint64_t size = div_up(e.width, bw) * div_up(e.height, bh) * bytes * e.depth * g.layers;
            VkBufferImageCopy r = {};
            r.bufferOffset = total;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g.layers};
            r.imageExtent = e;
            regions.push_back(r);
            total += size;
        }
        UploadAlloc st = f.staging(total);
        if (!st) return;
        std::memset(st.ptr, 0, static_cast<size_t>(total));
        for (VkBufferImageCopy& r : regions) r.bufferOffset += st.offset;
        copy_regions_to_image(f, g, st.buffer, regions);
        return;
    }
    move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = {};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, g.levels, 0, g.layers};
    vkCmdClearColorImage(f.cb, g.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
}  // namespace tex

// ---- GPU untile ([nvn] gpu_untile, default on) ----------------------------------------------------------------------------------
// A block-linear texture the CPU rewrites every frame (Pokopia: three 1024x1024 RGBA8 textures in turn) cost the render thread ~3 ms
// per upload: untile 4 MiB on the CPU, copy it into staging, copy that to the image. Instead a compute shader untiles straight from
// the pool (the game's memory, imported) into a device-local linear buffer, which is copied to the image: no CPU work. Like a sampler
// on the console, it reads the storage when the GPU runs the command. One level, one layer, 2D, block depth 1, rows of whole 16-byte
// pieces, the image in the storage's own format, 64 KiB and more; anything else takes the CPU path.

namespace tex {
#include "nvn_untile_shader.inc"
struct UntilePush { uint32_t base, ppr, total, gobs_x, ys, sw2; };
struct Untiler {
    bool tried = false, ok = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    static constexpr uint32_t kRing = 16;
    struct Entry { VkDescriptorSet set = VK_NULL_HANDLE; gfx::Buffer scratch; uint64_t serial = 0; };
    Entry ring[kRing];
    uint32_t next = 0;
    VkDeviceSize ssbo_align = 256;
};
static Untiler g_untile;

static bool untiler_init() {
    Untiler& u = g_untile;
    if (u.tried) return u.ok;
    u.tried = true;
    VkDevice d = vkdev();
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(device().physical(), &props);
    u.ssbo_align = std::max<VkDeviceSize>(16, props.limits.minStorageBufferOffsetAlignment);
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &u.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UntilePush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &u.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &u.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kUntileComp;
    mi.pCode = kUntileComp;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = u.layout;
    const VkResult pr = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &u.pipe);
    vkDestroyShaderModule(d, m, nullptr);
    if (pr != VK_SUCCESS) return false;
    const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * Untiler::kRing};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = Untiler::kRing;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &u.pool) != VK_SUCCESS) return false;
    for (Untiler::Entry& e : u.ring) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = u.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &u.set_layout;
        if (vkAllocateDescriptorSets(d, &ai, &e.set) != VK_SUCCESS) return false;
    }
    u.ok = true;
    NXLOG_I(kTag, "GPU untile on ([nvn] gpu_untile)");
    return true;
}

// A pitch-linear texture whose rows are whole texels: the copy to the image reads the pool directly (no CPU copy, no staging).
static bool gpu_pitch_copy(Texture* t, TexGpu& g, Frame& f, const TexLayout& L) {
    const uint32_t bpb = L.fmt->bytes;
    if (!bpb || L.pitch % bpb || L.fmt->block_w != 1 || L.fmt->block_h != 1 || g.base.depth != 1) return false;
    const uint64_t bytes = L.pitch * g.base.height;
    if (bytes > t->storage_size) return false;
    const BufRef br = resolve_buffer(reinterpret_cast<uint64_t>(t->mem), bytes);
    if (!br.ok || br.offset % std::max<uint32_t>(4, bpb)) return false;
    memory_barrier(f.cb);                          // earlier GPU writes to the storage
    VkBufferImageCopy r = {};
    r.bufferOffset = br.offset;
    r.bufferRowLength = static_cast<uint32_t>(L.pitch / bpb);
    r.bufferImageHeight = g.base.height;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {g.base.width, g.base.height, 1};
    copy_regions_to_image(f, g, br.buffer, {r});
    return true;
}

// True when the image now gets the storage's content from a GPU untile (or pitch copy) recorded into f.
static bool gpu_untile_impl(Texture* t, TexGpu& g, Frame& f, const char** why);
static bool gpu_untile(Texture* t, TexGpu& g, Frame& f) {
    const char* why = "";
    if (gpu_untile_impl(t, g, f, &why)) return true;
    static int logged = 0;
    if (t->storage_size >= (1u << 20) && logged < 12) {
        ++logged;
        NXLOG_I(kTag, "texture %u: GPU untile not used (%s): CPU upload", t->id, why);
    }
    return false;
}

static bool gpu_untile_impl(Texture* t, TexGpu& g, Frame& f, const char** why) {
    static const bool on = nxrt::config_get_bool("nvn", "gpu_untile", true);
    if (!on) return *why = "off", false;
    if (!t->mem || t->vpool || t->levels != 1 || t->layers != 1 || t->storage_size < (64u << 10)) return *why = "storage, levels or layers", false;
    // [nvn] pool_lazy: storage no bind has imported is untiled on the CPU (the upload path after this) instead of importing a chunk for it
    if (pool_lazy_wants_cpu(reinterpret_cast<uint64_t>(t->mem), t->storage_size)) return *why = "storage not imported (pool_lazy)", false;
    if (g.levels != 1 || g.layers != 1 || g.transcoded || !g.fmt || g.format != g.fmt->vk || !(g.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return *why = "image format or usage", false;
    const TexLayout L = compute_texture_layout(t->b);
    if (!L.valid || !L.fmt || L.fmt->astc || L.levels != 1 || L.layers != 1) return *why = "layout", false;
    if (!L.block_linear) return gpu_pitch_copy(t, g, f, L) || (*why = "pitch", false);
    if (!t->block_linear || L.layout.levels.size() != 1) return *why = "layout levels", false;
    const tegra::LevelLayout& lv = L.layout.levels[0];
    const uint64_t row_bytes = static_cast<uint64_t>(lv.blocks_x) * L.desc.bytes_per_block;
    if (lv.depth != 1 || lv.gob_blocks_z != 1 || row_bytes % 16 || lv.tiled_offset != 0) return *why = "3D or row size", false;
    const uint64_t lin = row_bytes * lv.blocks_y;
    const uint64_t total = lin / 16;
    if (total == 0 || total > 64ull * 65535) return false;
    const uint32_t gobs_x = static_cast<uint32_t>((row_bytes + 63) / 64);
    uint32_t ys = 0;
    while ((1u << ys) < lv.gob_blocks_y) ++ys;
    // the GOBs of every block row, whole blocks: what the shader may read
    const uint64_t tiled = static_cast<uint64_t>((lv.blocks_y + 8 * lv.gob_blocks_y - 1) / (8 * lv.gob_blocks_y)) * gobs_x * lv.gob_blocks_y * 512;
    if (tiled > t->storage_size) return *why = "storage size", false;
    const BufRef br = [&] { PoolReasonScope pr_(kReasonUntile); return resolve_buffer(reinterpret_cast<uint64_t>(t->mem), tiled); }();
    if (!br.ok) return *why = "not in a pool", false;
    if (!untiler_init()) return false;
    Untiler& u = g_untile;
    const VkDeviceSize boff = br.offset & ~(u.ssbo_align - 1);
    const VkDeviceSize delta = br.offset - boff;
    if (delta % 16) return false;
    Untiler::Entry& e = u.ring[u.next];
    if (e.serial && completed_serial() < e.serial) return false;     // its set and scratch may still be in use
    u.next = (u.next + 1) % Untiler::kRing;
    if (!e.scratch || e.scratch.size < lin) {
        if (e.scratch) {
            gfx::Buffer old = e.scratch;
            defer_destroy([old]() mutable { device().destroy_buffer(old); });
            e.scratch = gfx::Buffer{};
        }
        if (device().create_buffer(lin, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryUse::GpuOnly, &e.scratch, "nvn untile") != VK_SUCCESS) {
            e.scratch = gfx::Buffer{};
            return false;
        }
    }
    const VkDescriptorBufferInfo bi[2] = {{br.buffer, boff, delta + tiled}, {e.scratch.buffer, 0, lin}};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = e.set;
        w[i].dstBinding = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vkdev(), 2, w, 0, nullptr);
    const UntilePush pc = {static_cast<uint32_t>(delta / 16), static_cast<uint32_t>(row_bytes / 16), static_cast<uint32_t>(total), gobs_x, ys,
                           L.desc.gob_switch2 ? 1u : 0u};
    memory_barrier(f.cb);                          // earlier writes to the storage, and earlier reads of the scratch
    vkCmdBindPipeline(f.cb, VK_PIPELINE_BIND_POINT_COMPUTE, u.pipe);
    vkCmdBindDescriptorSets(f.cb, VK_PIPELINE_BIND_POINT_COMPUTE, u.layout, 0, 1, &e.set, 0, nullptr);
    vkCmdPushConstants(f.cb, u.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(f.cb, static_cast<uint32_t>((total + 63) / 64), 1, 1);
    memory_barrier(f.cb);
    VkBufferImageCopy r = {};
    r.bufferRowLength = lv.blocks_x * L.desc.block_w;
    r.bufferImageHeight = lv.blocks_y * L.desc.block_h;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {g.base.width, g.base.height, 1};
    copy_regions_to_image(f, g, e.scratch.buffer, {r});
    e.serial = f.serial;
    return true;
}
// The CPU wrote an HDR-promoted texture (RGBA16F image, 8-bit sRGB storage): the storage is prepared in its NVN format and goes in
// through tex_copy_buffer_converted, which blits it into the float16 image (sRGB decoded to linear, as the game's views read it).
// False when that is not possible: the caller clears the image as before.
static bool upload_promoted(Texture* t, TexGpu& g, Frame& f) {
    if (!g.fmt || t->levels != 1 || g.layers != 1 || !(g.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
    TexCaps caps;
    caps.image_format = g.fmt->vk;
    PreparedTexture prep;
    if (!prepare_texture(*t, caps, prep) || prep.format != g.fmt->vk || prep.level.empty()) return false;
    UploadAlloc st = f.staging(prep.data.size());
    if (!st) return false;
    std::memcpy(st.ptr, prep.data.data(), prep.data.size());
    const PreparedLevel& pl = prep.level[0];
    VkBufferImageCopy r = {};
    r.bufferOffset = st.offset + pl.offset;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {pl.width, pl.height, pl.depth};
    move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const char* why = nullptr;
    const bool ok = tex_copy_buffer_converted(t, f, st.buffer, r, true, &why);
    move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!ok) {
        static int logged = 0;
        if (logged++ < 10) NXLOG_W(kTag, "texture %u: the CPU wrote an HDR float16 target, not uploaded (%s): it is cleared", t->id, why ? why : "?");
        return false;
    }
    t->cpu_dirty.exchange(false);
    g.uploaded = true;
    g.gpu_written = false;
    counters().texture_uploads.fetch_add(1);
    return true;
}
}  // namespace tex

void tex_ensure_current(Texture* t, Frame& f) {
    NXFT_MIN("tex.ensure", 30000);
    TexGpu* g = tex_or_null(t);
    if (!g || g->depth) return;
    if (t->b.samples > 1 || target_is_multisample(t->b.target)) return;   // multisampled images are only ever rendered
    // A coherent texture the CPU rewrites every frame (Pokopia: three 1024x1024 RGBA8 textures in turn) cost one write-watch fault per 4 KiB
    // page per rewrite (~3500 a second on the main thread, inside the game's writes) because each upload armed its pages again. Once its CPU
    // uploads come kStreamGap presents apart or closer kStreamAfter times in a row, and the GPU untile path takes it, it is "streamed": never
    // armed again, and read from the pool once per present at its first use (a compute untile; the bytes the CPU wrote that frame, as a
    // console GPU sampling the memory would see them). [nvn] tex_stream = false: off.
    constexpr uint32_t kStreamGap = 4, kStreamAfter = 8;   // gap 4: Pokopia rewrites its three in turn (every third present each)
    static const bool stream_on = nxrt::config_get_bool("nvn", "tex_stream", true);
    const uint32_t now = static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed));
    if (g->streamed) {
        if (g->streamed_at == now && g->uploaded) return;
        t->cpu_dirty.exchange(false);
        if (gpu_untile(t, *g, f)) {
            g->uploaded = true;
            g->gpu_written = false;
            g->streamed_at = now;
            return;
        }
        g->streamed = false;                     // the untile path no longer takes it: back to write-watched uploads below
        t->cpu_dirty.store(true);
    }
    const bool dirty = t->cpu_dirty.load();
    if (g->uploaded && !dirty) return;           // the image already holds the storage's content
    nxrt::HitchTimer ht_upload(nxrt::kHpRenderUpload);   // the hitch line: an upload (or its GPU untile) on the render thread
    if (dirty && g->uploaded && stream_on && t->cpu_coherent) {   // an upload for a CPU write (not the first one)
        g->cpu_streak = now - g->cpu_upload_at <= kStreamGap ? g->cpu_streak + 1 : 1;
        g->cpu_upload_at = now;
        if (g->cpu_streak >= kStreamAfter && g->untiled) {
            g->streamed = true;
            g->streamed_at = now;
            NXLOG_I(kTag, "texture %u (%ux%u, %llu KiB): rewritten by the CPU every frame: read once per present, no longer write-watched", t->id,
                    g->base.width, g->base.height, static_cast<unsigned long long>(t->storage_size >> 10));
            t->cpu_dirty.exchange(false);
            if (gpu_untile(t, *g, f)) { g->uploaded = true; g->gpu_written = false; return; }
            g->streamed = false;
            t->cpu_dirty.store(true);
        }
    }
    if (g->gpu_written && !dirty) return;        // the GPU rendered it and the CPU did not write since
    if (g->sparse) {                             // a sparse-residency image: the dirty tiles only
        t->cpu_dirty.exchange(false);
        sparse_update(t, *g, f);
        return;
    }
    if (g->scale != 1.0f && dirty && (g->uploaded || g->gpu_written)) {
        static int logged = 0;
        if (logged++ < 20) NXLOG_W(kTag, "texture %u: the CPU wrote a scaled render target: the write is not uploaded", t->id);
    }
    // A float16 render target of the HDR scene, or a scaled one: the NVN bytes in memory do not fit the image. It starts cleared.
    // A CPU write into an HDR float16 one goes in through a converting blit (the album decodes its photos into one, #229).
    if (g->promoted && dirty && upload_promoted(t, *g, f)) return;
    if (g->promoted || g->scale != 1.0f) {
        t->cpu_dirty.exchange(false);
        if (!g->uploaded) fill_zero(f, *g);
        g->uploaded = true;
        return;
    }
    t->cpu_dirty.exchange(false);
    // before the storage is read: a write from now on makes it dirty again. Off the render thread (tex_ww_arm_later): the content hash of what
    // the upload reads is taken here instead, and the worker compares it after arming.
    const bool arm_later = tex_ww_async_on() && tex_ww_wanted(*t, *g);
    if (!arm_later) tex_ww_arm(*t, *g);
    uint64_t content_hash = 0;
    bool have_hash = false;
    if (g->gpu_written) {
        static int logged = 0;
        if (logged++ < 20) NXLOG_W(kTag, "texture %u: the GPU wrote it, the game flushed memory it overlaps: its storage replaces the image content", t->id);
    }

    g->untiled = false;
    // the untile reads the pool when the GPU runs it: hashed now, armed after (only when the untile can take it: the image in the storage's format)
    if (arm_later && t->levels == 1 && t->layers == 1 && t->storage_size >= (64u << 10) && !g->transcoded && g->fmt && g->format == g->fmt->vk)
        have_hash = texprep::hash_storage(*t, content_hash);
    if (gpu_untile(t, *g, f)) {
        if (arm_later) tex_ww_arm_later(t, *g, have_hash, content_hash);
        g->untiled = true;
        ft::mark("tex.untile", t->id);
        be::tx_mark(be::TxMark::Use, t);
        g->uploaded = true;
        g->gpu_written = false;
        texprep::note_uploaded(t);
        counters().texture_uploads.fetch_add(1);
        return;
    }

    const gfx::DeviceFeatures& ft = device().features();
    TexCaps caps;
    caps.bc = ft.bc;
    caps.astc = ft.astc_ldr;
    caps.bc7_transcode = true;
    caps.upgrade = true;
    caps.image_format = g->format;
    PreparedTexture prep;
    bool ok = false;
    bool from_job = false;
    const uint64_t t_prep0 = ft::now_ns();
    gfx::Buffer staged;                        // a big background result comes with its upload buffer already filled
    uint64_t take_hash = 0;
    if (texprep::take(t, prep, &staged, &take_hash)) {     // prepared in the background from the current storage content (nvn_texprep.h)
        if (prep.format == g->format) {
            content_hash = take_hash;
            have_hash = true;
            ok = from_job = true;
        } else {                               // another image format than the one it was made for (a fallback format): prepare it again
            prep = PreparedTexture();
            if (staged) device().destroy_buffer(staged);
            staged = gfx::Buffer{};
        }
    }
    if (!ok) {
        static const uint16_t zid = ft::zone_id("tex.prepare");
        ft::Scope zs(zid, 30000, t->id);
        if (arm_later && !have_hash) have_hash = texprep::hash_storage(*t, content_hash);   // before the read the upload uses
        // ASTC without a cache entry, needed now: a quick stand-in (preset 0) instead of the full transcode; the background job makes the real data
        caps.provisional = texprep::wants_provisional(t);
        ok = prepare_texture(*t, caps, prep);
    }
    be::tx_mark(be::TxMark::Use, t);
    {
        static const bool log_uploads = nxrt::config_get_bool("nvn", "tex_prep_log", false);      // [nvn] tex_prep_log = 1: one line per upload with where the data came from
        if (log_uploads)
            NXLOG_I(kTag, "upload t%u %s %ux%ux%u L%u storage %llu KB%s sched 0x%x gen %u: %s %.2f ms", t->id, t->fmt ? t->fmt->name : "?", t->b.width, t->b.height, t->b.depth, t->levels,
                    static_cast<unsigned long long>(t->storage_size >> 10), t->vpool ? " sparse" : "", static_cast<unsigned>(t->prep_sched.load()), static_cast<unsigned>(t->prep_gen.load()),
                    from_job ? "background" : "render thread", static_cast<double>(ft::now_ns() - t_prep0) * 1e-6);
    }
    if (!ok || prep.format != g->format || !(g->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        NXLOG_E(kTag, "texture %u: not uploaded (%s): the image is cleared", t->id,
                !ok ? prep.error.c_str() : "the image format differs from the storage format or the image cannot be written");
        if (staged) device().destroy_buffer(staged);
        if (arm_later) tex_ww_arm_later(t, *g, have_hash, content_hash);   // as the synchronous arm: a later write tries again
        fill_zero(f, *g);
        g->uploaded = true;
        g->gpu_written = false;
        return;
    }

    {
        uint64_t w0 = 0;
        if (!prep.data.empty()) std::memcpy(&w0, prep.data.data(), std::min<size_t>(8, prep.data.size()));
        tev(t, "upload", w0, dirty ? 1 : 0);
    }
    const uint64_t bytes = staged ? staged.size : prep.data.size();
    UploadAlloc st = staged ? f.adopt_staging(staged) : f.staging(bytes);
    if (!st) {
        t->cpu_dirty.store(true);                // try again next time
        return;
    }
    if (!staged) std::memcpy(st.ptr, prep.data.data(), static_cast<size_t>(bytes));
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(prep.level.size());
    for (uint32_t i = 0; i < prep.level.size(); ++i) {
        const PreparedLevel& pl = prep.level[i];
        VkBufferImageCopy r = {};
        r.bufferOffset = st.offset + pl.offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, g->layers};
        r.imageExtent = {pl.width, pl.height, pl.depth};
        regions.push_back(r);
    }
    copy_regions_to_image(f, *g, st.buffer, regions);
    if (arm_later) tex_ww_arm_later(t, *g, have_hash, content_hash);
    g->uploaded = true;
    g->gpu_written = false;
    texprep::note_uploaded(t);
    if (prep.provisional) texprep::provisional_uploaded(t);
    else texprep::final_uploaded(t);
    counters().texture_uploads.fetch_add(1);
    counters().texture_upload_bytes.fetch_add(bytes);
    if (prep.transcoded) counters().bc7_transcodes.fetch_add(1);
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
