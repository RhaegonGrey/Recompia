// SPDX-License-Identifier: GPL-3.0-or-later
// Texture images of the NVN back end (render thread only). Each be::Texture gets one VkImage on first use (tex_gpu). The image is
// created without a view: every view (sampled, attachment, present alias) is made here and cached on the TexGpu. Formats: the NVN
// format when the GPU has it, its vk_fallback when the GPU refuses it, ASTC as BC7 when the GPU has no ASTC.
// Uploads: prepare_texture (nvn_texdata.cpp) gives the bytes, they go through a staging buffer into one buffer-to-image copy per
// level. Layouts are tracked per image (all levels and layers share one layout). Samplers are cached by their NVN state.
// Readback (download_texture) copies a render target back and tiles it into the game's storage.
#include "nvn_gpu_texture_internal.h"
#include "nvn_thread.h"   // BigThread: the texture-arming worker gets a stack of its own (the guest code cannot exhaust it)

namespace nxrt::nvn::be {
static uint64_t g_wgen_next = 0;                        // render thread only (move_layout, the replay)

// The Vulkan side of a texture: Texture::gpu points to it (created by gpu::tex_gpu, freed by gpu::tex_destroy).
static std::mutex g_live_mu;
static std::set<struct TexGpu*> g_live_set;

Texture::~Texture() {
    if (gpu) gpu::destroy_texture_gpu(gpu);
    gpu = nullptr;
}

namespace gpu {

namespace tex {
std::atomic<uint64_t> g_live_images{0};
static nxrt::GaugeCounter g_gauge_textures_live("nvn.textures_live");               // TexGpu objects alive (g_live_set)
nxrt::GaugeCounter g_gauge_texture_snapshots("nvn.texture_snapshots");       // debug snapshots kept (g_snaps; the list never shrinks)

// Logs a message only the first time its key is seen (render thread only).
bool first_time(const std::string& key) {
    static std::set<std::string> seen;
    return seen.insert(key).second;
}

// A promoted image answers the 8-bit RGBA formats the game asks for (its sRGB and UNORM views) with its own format: the values are linear floats.
VkFormat promoted_view_format(const TexGpu& g, VkFormat f) {
    // An image made in its format's fallback (D24S8 as D32_SFLOAT_S8_UINT on GPUs without D24S8, e.g. AMD) answers views of the
    // NVN format with its own: before, every shader sampling such a depth buffer got the dummy view.
    if (g.fmt && f == g.fmt->vk && g.format == g.fmt->vk_fallback && f != g.format) return g.format;
    if (g.promoted && (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM)) return g.format;
    // Views in another NVN format of an image made in its fallback (AMD has no 24-bit depth formats) take the image's own format too:
    // depth reads as the same 0..1 values. Before, those views got the dummy, and on the Steam Deck the fog and soft particles that read
    // the depth buffer covered the ground in white.
    if (f == VK_FORMAT_D24_UNORM_S8_UINT && g.format == VK_FORMAT_D32_SFLOAT_S8_UINT) return g.format;   // a view in another NVN format
    if (f == VK_FORMAT_X8_D24_UNORM_PACK32 && g.format == VK_FORMAT_D32_SFLOAT) return g.format;
    // An ASTC view (a texture view naming its ASTC format, sRGB or not) of an image stored transcoded (BC7, or the disk cache's RGBA8/BC5)
    // reads the image's own format, as its sRGB or UNORM partner like the view asks. Mario Kart World's character and course materials
    // name their format in the view; before, they sampled the black dummy.
    if (g.transcoded && f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        const bool want_srgb = ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1) != 0;
        const VkFormat partner = srgb_partner(g.format);
        if (partner == VK_FORMAT_UNDEFINED) return g.format;
        const bool img_srgb = is_srgb(g.format);
        return want_srgb == img_srgb ? g.format : partner;
    }
    return f;
}

static VkImageAspectFlags aspect_bits(const FormatInfo* f) {
    switch (f->aspect) {
        case kAspectDepth: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case kAspectStencil: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case kAspectDepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static Shape shape_of(const Texture& t) {
    const TextureBuilderInfo& b = t.b;
    Shape s;
    const bool is1d = b.target == NVN_TEXTURE_TARGET_1D || b.target == NVN_TEXTURE_TARGET_1D_ARRAY;
    const bool is3d = b.target == NVN_TEXTURE_TARGET_3D;
    s.type = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    s.base.width = static_cast<uint32_t>(std::max(1, b.width));
    s.base.height = is1d ? 1u : static_cast<uint32_t>(std::max(1, b.height));
    s.base.depth = is3d ? static_cast<uint32_t>(std::max(1, b.depth)) : 1u;
    s.levels = std::max<uint32_t>(1, t.levels);
    s.layers = std::max<uint32_t>(1, t.layers);
    s.cube = target_is_cube(b.target);
    if (target_is_multisample(b.target) || b.samples > 1) {
        s.samples = static_cast<uint32_t>(vk_sample_count(b.samples));
        s.levels = 1;
    }
    return s;
}

static gfx::ImageDesc make_desc(const Shape& s, VkFormat fmt, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    gfx::ImageDesc d;
    d.type = s.type;
    d.format = fmt;
    d.width = s.base.width;
    d.height = s.base.height;
    d.depth = s.base.depth;
    d.mip_levels = s.levels;
    d.array_layers = s.layers;
    d.usage = usage;
    d.flags = flags;
    d.samples = static_cast<VkSampleCountFlagBits>(s.samples);
    d.create_view = false;
    d.name = "nvn texture";
    return d;
}

// ---- creation -------------------------------------------------------------------------------------------------------------
// Makes the image of t (or the failed marker when no format works). Stores the result in t->gpu, which is never null afterwards.
// A new write generation for one level (level >= 0) or for all of them (level < 0).
static void bump_gen(TexGpu& g, int level) {
    if (level >= 0) { g.wgen[std::min<uint32_t>(static_cast<uint32_t>(level), kGenLevels - 1)] = ++g_wgen_next; return; }
    for (uint64_t& w : g.wgen) w = ++g_wgen_next;
}

// Layouts nothing writes the image in: a move into any other one counts as a write of every level (or of `level` alone).
static bool read_only_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
}

TexGpu* create_texture_gpu(Texture* t, float want_scale) {
    auto* g = new TexGpu();
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live_set.insert(g); }
    g_gauge_textures_live.add(1);
    t->gpu = g;
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);
    const Shape s = shape_of(*t);
    g->fmt = t->fmt;
    g->type = s.type;
    g->base = s.base;
    g->levels = s.levels;
    g->layers = s.layers;
    g->cube = s.cube;
    auto fail = [&](const std::string& why) -> TexGpu* {
        NXLOG_E(kTag, "texture %u: no image (%s): the texture is not usable on the GPU", t->id, why.c_str());
        g->failed = true;
        g_bind_gen.fetch_add(1, std::memory_order_relaxed);
        counters().textures_failed.fetch_add(1);
        return g;
    };
    if (!g->fmt) return fail("no format");
    // A texture of a virtual (sparse) pool would be a fully backed image here: Mario Kart World's virtual texture caches are 8192x8192 arrays
    // of 68 layers (5.8 GB each as BC7) of which the console maps a few hundred MB of tiles. Until sparse residency images exist, such a
    // texture over [nvn] sparse_image_max_mb gets no image (it samples as missing) instead of exhausting the host's memory.
    if (t->vpool) {
        static const uint64_t max_mb = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "sparse_image_max_mb", 1024)));
        if (max_mb && (t->storage_size >> 20) > max_mb && s.type == VK_IMAGE_TYPE_2D && !s.cube && s.samples == 1 &&
            device().features().sparse_binding && device().features().sparse_residency_image2d && config_get_bool("nvn", "sparse_residency", true)) {
            std::string why;
            if (create_sparse_texture(t, g, s, &why)) return g;
            return fail("sparse residency image: " + why);
        }
        if (max_mb && (t->storage_size >> 20) > max_mb)
            return fail("sparse texture of " + std::to_string(t->storage_size >> 20) + " MB (" + std::to_string(s.base.width) + "x" + std::to_string(s.base.height) +
                        ", " + std::to_string(s.layers) + " layers, " + std::to_string(s.levels) + " levels, NVN format " + std::to_string(t->b.format) +
                        ") is over [nvn] sparse_image_max_mb; needs sparse residency");
    }

    const gfx::DeviceFeatures& ft = device().features();
    const bool to_bc7 = g->fmt->astc && !ft.astc_ldr;
    if (to_bc7 && !ft.bc) return fail("ASTC is stored as BC7 and the GPU has no BC compression");
    const VkFormat bc7_format = g->fmt->srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    // A disk cache entry that is not BC7 (RGBA8 for a texture BC7 cannot reproduce, BC5 for a two-channel normal map) makes the image in the
    // entry's format; prepare_texture is then told the image's format and delivers the entry's data (docs/S4f.md).
    VkFormat entry = VK_FORMAT_UNDEFINED;
    if (to_bc7 && !texprep::peek_entry_format(t, entry)) entry = cache_entry_format(*t);   // the background job already found the cache entry
    const VkFormat want = to_bc7 ? (entry != VK_FORMAT_UNDEFINED ? entry : bc7_format) : g->fmt->vk;
    const VkFormat alt = to_bc7 ? (entry != VK_FORMAT_UNDEFINED ? bc7_format : VK_FORMAT_UNDEFINED) : g->fmt->vk_fallback;

    std::string why = std::string("no Vulkan format for NVN format ") + g->fmt->name;
    VkFormat chosen = VK_FORMAT_UNDEFINED;
    // [nvn] test_no_d24 = 1 (testing): behave like a GPU without 24-bit depth formats (AMD): DEPTH24 and DEPTH24_STENCIL8 get their fallback.
    static const bool no_d24 = config_get_bool("nvn", "test_no_d24", false);
    for (VkFormat c : {want, alt}) {
        if (c == VK_FORMAT_UNDEFINED) continue;
        if (no_d24 && (c == VK_FORMAT_D24_UNORM_S8_UINT || c == VK_FORMAT_X8_D24_UNORM_PACK32)) {
            if (first_time("test_no_d24")) NXLOG_I(kTag, "texture %u: %s made in its fallback format ([nvn] test_no_d24)", t->id, g->fmt->name);
            continue;
        }
        std::string r;
        if (device().image_supported(make_desc(s, c, VK_IMAGE_USAGE_SAMPLED_BIT, base_flags(s, c)), &r)) {
            chosen = c;
            break;
        }
        why = r;
    }
    if (chosen == VK_FORMAT_UNDEFINED) return fail(why);

    if (const VkFormat hf = extras::hdr_promote_target(chosen, t->b.flags, s.type == VK_IMAGE_TYPE_2D, s.levels, s.layers, s.samples, g->fmt->srgb)) {
        if (device().image_supported(make_desc(s, hf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, base_flags(s, hf)))) {
            NXLOG_I(kTag, "texture %u: %s %ux%u (flags 0x%x) kept as float16 for the HDR scene", t->id, g->fmt->name, s.base.width, s.base.height, static_cast<unsigned>(t->b.flags));
            chosen = hf;
            g->promoted = true;
        }
    }

    g->format = chosen;
    g->transcoded = to_bc7;
    if (to_bc7 && (chosen == VK_FORMAT_R8G8B8A8_UNORM || chosen == VK_FORMAT_R8G8B8A8_SRGB) && astc_quality() == AstcQuality::Uncompressed) note_uncompressed_image(*t);
    g->mutable_fmt = srgb_partner(chosen) != VK_FORMAT_UNDEFINED || bit_alias_class(chosen);
    g->aspect = aspect_bits(g->fmt);
    g->depth = (g->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;

    // Required usage is sampling. Each optional usage is kept only when the GPU accepts the image with it.
    VkImageCreateFlags flags = base_flags(s, chosen);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto add = [&](VkImageUsageFlags bit) {
        if (device().image_supported(make_desc(s, chosen, usage | bit, flags))) usage |= bit;
    };
    add(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    add(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!g->depth && !is_block_format(chosen)) add(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (g->depth) add(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    // The console reads and writes any colour texture through an image handle; the builder's IMAGE flag is a hint the game does not always
    // set (the froxel volume of the fog is written by a vertex shader without it). Every uncompressed colour image therefore keeps the
    // storage usage when the GPU accepts the combination.
    if (!g->depth && !is_block_format(chosen)) {
        add(VK_IMAGE_USAGE_STORAGE_BIT);
        // sRGB formats have no storage support. The console's image stores write the raw bits (no sRGB encode), which is what a UNORM view
        // of the same image does: storage usage is then allowed through EXTENDED_USAGE when the UNORM partner can be stored to.
        const VkFormat unorm = is_srgb(chosen) ? srgb_partner(chosen) : VK_FORMAT_UNDEFINED;
        if (!(usage & VK_IMAGE_USAGE_STORAGE_BIT) && unorm != VK_FORMAT_UNDEFINED && (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
            device().image_supported(make_desc(s, unorm, usage | VK_IMAGE_USAGE_STORAGE_BIT, flags))) {
            flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        if ((t->b.flags & NVN_TEXTURE_FLAGS_IMAGE) && !(usage & VK_IMAGE_USAGE_STORAGE_BIT))
            NXLOG_W(kTag, "texture %u (%s, image flag set): the GPU refuses storage usage with the other usages (flags 0x%x, usage 0x%x)", t->id, t->fmt ? t->fmt->name : "?",
                    static_cast<unsigned>(flags), static_cast<unsigned>(usage));
    }
    g->usage = usage;
    g->cflags = flags;
    bump_gen(*g, -1);

    // Resolution scale: the image is the NVN size times the scale. Other modules keep the NVN size (tex_extent) and ask tex_scale /
    // tex_image_extent where the image size matters (render area, viewports, copies, the support buffer's render_scale).
    Shape is = s;
    if (want_scale != 1.0f && !g->transcoded && !is_block_format(chosen)) {
        const uint32_t lim = device().properties().limits.maxImageDimension2D;
        const uint32_t w = static_cast<uint32_t>(std::lround(s.base.width * want_scale)), h = static_cast<uint32_t>(std::lround(s.base.height * want_scale));
        if (w >= 1 && h >= 1 && w <= lim && h <= lim) {
            is.base.width = w;
            is.base.height = h;
            g->scale = want_scale;
            g->base = is.base;
            static int logged = 0;
            if (logged++ < 60)
                NXLOG_I(kTag, "texture %u: %s %ux%u (flags 0x%x, %u levels, %u layers, %u samples) rendered at %ux%u (scale %.3f)", t->id, g->fmt->name, s.base.width,
                        s.base.height, static_cast<unsigned>(t->b.flags), s.levels, s.layers, s.samples, w, h, want_scale);
        }
    }
    std::string why2;
    if (device().create_image(make_desc(is, chosen, usage, flags), &g->img, &why2) != VK_SUCCESS)
        return fail("create_image: " + why2);
    g_live_images.fetch_add(1);
    counters().textures_created.fetch_add(1);
    return g;
}

// The resolution scale: the game's play settings ([display] render_scale, turned into a factor by the game module and handed over in
// gfx::tuning) or, for tests, [gfx] render_scale (a number, NXRT_GFX_RENDER_SCALE), which wins. Fixed once known: images keep their size.
// Stays 1 (not fixed) until one of them is there.
static float g_render_scale = 0.0f;
static float render_scale_now() {
    if (g_render_scale > 0.0f) return g_render_scale;
    const std::string v = nxrt::config_get_str("gfx", "render_scale", "");
    float sc = 1.0f;
    if (!v.empty()) {
        sc = static_cast<float>(std::atof(v.c_str()));
    } else {
        sc = nxrt::gfx::tuning().render_scale.load();
        if (!(sc > 0.0f)) return 1.0f;
    }
    if (!(sc > 0.0f)) sc = 1.0f;
    sc = std::clamp(sc, 0.25f, 4.0f);
    sc = std::round(sc * 1080.0f) / 1080.0f;   // a whole number of lines at 1080 (1.333 -> 4/3: 1440, not 1439)
    if (std::fabs(sc - 1.0f) < 0.01f) sc = 1.0f;
    g_render_scale = sc;
    NXLOG_I(kTag, "render scale %.3f (%s)", sc, v.empty() ? "play settings" : "[gfx] render_scale");
    return sc;
}

// The scale a texture gets when its image is made for a render target (after Ryujinx's TextureScaling rules): 2D colour or depth targets
// of at least 32x32, not block-compressed, not pitch-linear, and not square powers of two (shadow
// maps, LUTs). Everything else, and every texture first used another way, keeps the NVN size.
// A shadow map: a square power-of-two depth target. The render scale leaves it alone; the shadow quality setting sizes it on its own.
static bool is_shadow_map(const Texture& t) {
    return t.fmt && t.fmt->aspect != kAspectColor && t.b.target == NVN_TEXTURE_TARGET_2D && t.b.width == t.b.height && t.b.width >= 256 &&
           (t.b.width & (t.b.width - 1)) == 0 && !(t.b.flags & NVN_TEXTURE_FLAGS_LINEAR);
}

static float attachment_scale(const Texture& t) {
    if (is_shadow_map(t)) {
        const float ss = gfx::tuning().shadow_scale.load(std::memory_order_relaxed);   // live: tex_scale_wanted differs from the image's scale, the next pass remakes it
        static int logged = 0;
        if (logged++ < 8) NXLOG_I(kTag, "shadow map candidate: texture %u %s %ux%u (shadow scale %.0f)", t.id, t.fmt->name, t.b.width, t.b.height, ss);
        return ss;
    }
    const float sc = render_scale_now();
    if (sc == 1.0f) return 1.0f;
    const TextureBuilderInfo& b = t.b;
    if (!t.fmt || t.fmt->compressed || t.fmt->block_w != 1 || t.fmt->block_h != 1) return 1.0f;
    if (b.target != NVN_TEXTURE_TARGET_2D && b.target != NVN_TEXTURE_TARGET_2D_ARRAY && b.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE &&
        b.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY && b.target != NVN_TEXTURE_TARGET_RECTANGLE)
        return 1.0f;
    // not the IMAGE flag: the game sets it on every render target; storage use brings a scaled image back instead (tex_rescale)
    if (b.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_VIDEO_DECODE)) return 1.0f;
    const int w = b.width, h = b.height;
    if (w < 32 || h < 32) return 1.0f;
    if (w == h && (w & (w - 1)) == 0) return 1.0f;
    return sc;
}

// ---- barriers and copies ----------------------------------------------------------------------------------------------------
void move_layout(Frame& f, TexGpu& g, VkImageLayout to, int write_level) {
    if (g.layout == to) return;
    if (!read_only_layout(to)) bump_gen(g, write_level);
    // A 3D image created with 2D_ARRAY_COMPATIBLE: without maintenance9 the layer range of a barrier means all depth slices.
    const uint32_t layer_count = g.type == VK_IMAGE_TYPE_3D ? VK_REMAINING_ARRAY_LAYERS : g.layers;
    image_barrier(f.cb, g.img.image, g.layout, to, g.aspect, 0, g.levels, 0, layer_count);
    g.layout = to;
}

// Copies buffer regions (already offset to the staging buffer) into every level of the image, then leaves it readable by shaders.
void copy_regions_to_image(Frame& f, TexGpu& g, VkBuffer buf, const std::vector<VkBufferImageCopy>& regions) {
    move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(f.cb, buf, g.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
    move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
// ---- CPU writes without a flush --------------------------------------------------------------------------------------------
// A texture in a CPU_UNCACHED pool is coherent on the console: the game writes it and the GPU sees the bytes without FlushMappedRange.
// ACNH's text renderer rasterizes the glyphs it needs into such a texture (an R8 2048x4096 glyph cache) as new characters come up; an
// image uploaded once kept the old glyph set and every later new glyph drew as nothing. The storage pages of an uploaded coherent texture
// are therefore armed in one write-watch region over the heap (nxrt/write_watch.h), before the storage is read for the upload. At the start
// of every submission the pages the CPU wrote since are handed to textures_cpu_wrote, as a flush of them would be: the textures there are
// uploaded again before their next use (and armed again). A texture the GPU renders to is not armed. [nvn] tex_write_watch = auto (off on Linux) | true | false.
static mem::ww::Region* g_tex_ww = nullptr;
static bool g_tex_ww_tried = false;
static std::vector<uint64_t> g_tex_ww_pages;

static mem::ww::Region* tex_ww() {
    if (!g_tex_ww_tried) {
        g_tex_ww_tried = true;
        // auto (default): off on Linux (Proton or native). There the heap is private memory imported as amdgpu userptr BOs, and
        // making those pages read-only invalidates the BOs: the next submission loses the device (#169, Legion Go S and Steam Deck).
        const std::string v = nxrt::config_get_str("nvn", "tex_write_watch", "auto");
        const bool on = v == "auto" ? !nxrt::host_is_linux() : (v == "on" || v == "1" || v == "true");
        NXLOG_I(kTag, "tex_write_watch %s (%s%s)", on ? "on" : "off", v.c_str(),
                v == "auto" ? (nxrt::host_is_linux() ? ", Linux" : ", not Linux") : "");
        if (on && mem::heap_exists())
            g_tex_ww = mem::ww::region_create(mem::kHeapBase, mem::heap_reserved());
    }
    return g_tex_ww;
}

bool tex_ww_wanted(const Texture& t, const TexGpu& g) { return t.cpu_coherent && t.mem && t.storage_size && !g.gpu_written && tex_ww(); }

void tex_ww_arm(const Texture& t, const TexGpu& g) {
    if (!tex_ww_wanted(t, g)) return;
    mem::ww::Region* r = tex_ww();
    const uint64_t a = reinterpret_cast<uint64_t>(t.mem), base = mem::ww::region_base(r);
    if (a < base) return;
    const uint64_t first = (a - base) >> 12, last = (a + t.storage_size - 1 - base) >> 12;
    mem::ww::arm(r, first, last - first + 1);   // false: a page it cannot protect (another view, a file read in progress): not watched
}

// ---- arming off the render thread ([nvn] tex_ww_async, default on) ---------------------------------------------------------------
// arm() is a VirtualQuery walk and a VirtualProtect under the region's exclusive lock. While the game streams an area those calls took the
// render thread 60 to 490 ms a frame (the hitch line's "write-watch arming", inside "uploads"). Here the render thread hashes the storage it
// uploads from and hands the arm to one worker thread. The worker arms first and hashes the storage again: a write after the arm faults and
// is reported as before (tex_poll_cpu_writes); a write between the upload's read and the arm shows as a different hash and is reported the
// same way (textures_cpu_wrote), so the texture is uploaded again. Nothing the CPU writes is missed; the only cost is that write's re-upload.
struct ArmJob {
    Ref<Texture> t;
    uint64_t first = 0, count = 0, hash = 0, seq = 0;
};
struct ArmQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<ArmJob> q;
    std::unordered_map<Texture*, uint64_t> latest;     // newest job of a texture (older ones are skipped)
    uint64_t seq = 0;
    bool started = false;
};
static ArmQueue* g_armq = nullptr;     // never destroyed: the worker runs until the process ends
static std::mutex g_arm_import_mu;     // tex_arm_import_mutex: the worker's arms against pool_lazy's imports
static std::atomic<uint64_t> g_arm_async_jobs{0}, g_arm_async_rewritten{0};
// the hitch log: arms the worker made, and uploads it found written before its arm (uploaded again), during a slow frame
static const bool g_arm_hitch_registered =
    (nxrt::hitch_add_counter("tex_arms_async", []() -> uint64_t { return g_arm_async_jobs.load(std::memory_order_relaxed); }),
     nxrt::hitch_add_counter("tex_rewritten_before_arm", []() -> uint64_t { return g_arm_async_rewritten.load(std::memory_order_relaxed); }), true);

bool tex_ww_async_on() {
    static const bool on = nxrt::config_get_bool("nvn", "tex_ww_async", true);
    return on;
}

static void arm_worker() {
    log_set_thread_name("nvn-ww-arm");
    ArmQueue& aq = *g_armq;
    std::unique_lock<std::mutex> lk(aq.mu);
    for (;;) {
        aq.cv.wait(lk, [&] { return !aq.q.empty(); });
        ArmJob j = std::move(aq.q.front());
        aq.q.pop_front();
        Texture* t = j.t.get();
        auto it = aq.latest.find(t);
        if (it == aq.latest.end() || it->second != j.seq) continue;   // a newer upload queued its own arm
        lk.unlock();
        if (!t->destroyed.load()) {
            {
                std::lock_guard<std::mutex> ilk(g_arm_import_mu);   // not between a lazy import's forget and the import (tex_arm_import_mutex)
                mem::ww::arm(g_tex_ww, j.first, j.count);   // false: a page it cannot protect: not watched (as the synchronous arm)
            }
            uint64_t h = 0;
            const bool same = texprep::hash_storage(*t, h) && h == j.hash;   // unreadable now: reported, as a write would be
            lk.lock();
            const bool newest = aq.latest.count(t) && aq.latest[t] == j.seq;
            if (newest) aq.latest.erase(t);
            lk.unlock();
            if (!same && newest) {      // written since the upload read it (or not readable): upload it again
                g_arm_async_rewritten.fetch_add(1, std::memory_order_relaxed);
                textures_cpu_wrote(t->mem, t->storage_size);
            }
        } else {
            lk.lock();
            if (aq.latest.count(t) && aq.latest[t] == j.seq) aq.latest.erase(t);
            lk.unlock();
        }
        g_arm_async_jobs.fetch_add(1, std::memory_order_relaxed);
        j.t = Ref<Texture>();     // the texture may be destroyed here
        lk.lock();
    }
}
}  // namespace tex

std::mutex& tex_arm_import_mutex() { return g_arm_import_mu; }

namespace tex {
// Render thread, after the storage was read for an upload of content `hash` (have_hash false: unknown). Arms the pages on the worker.
void tex_ww_arm_later(Texture* t, const TexGpu& g, bool have_hash, uint64_t hash) {
    if (!tex_ww_wanted(*t, g)) return;
    if (!have_hash) { tex_ww_arm(*t, g); return; }      // nothing to compare with after the arm (storage not readable): as before
    mem::ww::Region* r = tex_ww();
    const uint64_t a = reinterpret_cast<uint64_t>(t->mem), base = mem::ww::region_base(r);
    if (a < base) return;
    if (!g_armq) g_armq = new ArmQueue();
    ArmQueue& aq = *g_armq;
    std::lock_guard<std::mutex> lk(aq.mu);
    if (!aq.started) {
        aq.started = true;
        BigThread t;
        t.start("nvn-tex-arm", 16, arm_worker);
        t.detach();
    }
    ArmJob j;
    j.t = Ref<Texture>::share(t);
    j.first = (a - base) >> 12;
    j.count = ((a + t->storage_size - 1 - base) >> 12) - j.first + 1;
    j.hash = hash;
    j.seq = ++aq.seq;
    aq.latest[t] = j.seq;
    aq.q.push_back(std::move(j));
    aq.cv.notify_one();
}
}  // namespace tex

// ---- texture interface (gpu::*, internal.h) -----------------------------------------------------------------------------
TexGpu* tex_gpu_attachment(Texture* t) {
    if (!t || t->gpu || !active()) return tex_or_null(t);
    if (texture_registry_find_alias(t)) return tex_or_null(t);   // shares the image (and its scale) of the texture it aliases
    return tex_or_null(t, attachment_scale(*t));
}

TexGpu* tex_gpu_scaled(Texture* t, float scale) { return tex_or_null(t, scale); }

float render_scale() { return render_scale_now(); }

float tex_scale(Texture* t) {
    return (t && t->gpu && !t->gpu->failed) ? t->gpu->scale : 1.0f;
}

VkExtent3D tex_image_extent(Texture* t, uint32_t level) {
    TexGpu* g = tex_or_null(t);
    return g ? level_extent(g->base, level) : tex_extent(t, level);
}

float tex_scale_wanted(Texture* t) {
    TexGpu* g = tex_or_null(t);
    if (!g || g->scale_blocked || g->transcoded || is_block_format(g->format)) return g ? g->scale : 1.0f;
    return attachment_scale(*t);
}

// Remakes the image at another scale (Ryujinx scales a texture when it becomes a render target and blacklists one that cannot be
// scaled). The content is blitted over when the format allows it (otherwise it starts undefined). Going to 1 is for good. The old image
// and its views are destroyed after the frames that use them.
bool tex_rescale(Texture* t, Frame& f, float to, const char* why) {
    TexGpu* g = tex_or_null(t);
    if (!g || g->scale == to) return false;
    if (to != 1.0f && (g->scale_blocked || g->transcoded || is_block_format(g->format))) return false;
    Shape s = shape_of(*t);
    if (to != 1.0f) {
        s.base.width = static_cast<uint32_t>(std::lround(s.base.width * to));
        s.base.height = static_cast<uint32_t>(std::lround(s.base.height * to));
    }
    gfx::Image ni;
    std::string err;
    // the flags the image was made with: base_flags alone lacks EXTENDED_USAGE, and an sRGB image with storage usage was refused at every
    // rescale (once a frame, as the texture kept wanting its scale)
    if (device().create_image(make_desc(s, g->format, g->usage, g->cflags ? g->cflags : base_flags(s, g->format)), &ni, &err) != VK_SUCCESS) {
        NXLOG_W(kTag, "texture %u: cannot remake it at %ux%u (%s): it keeps scale %.3f", t->id, s.base.width, s.base.height, err.c_str(), g->scale);
        if (to == 1.0f || is_shadow_map(*t)) g->scale_blocked = true;   // not retried every pass (a shadow map too large for the GPU stays as it is)
        return false;
    }
    static int logged = 0;
    if (logged++ < 60)
        NXLOG_I(kTag, "texture %u: %s %dx%d now rendered at %ux%u (%s)", t->id, g->fmt ? g->fmt->name : "?", t->b.width, t->b.height, s.base.width, s.base.height, why);
    VkFormatProperties fp = {};
    vkGetPhysicalDeviceFormatProperties(device().physical(), g->format, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool can_blit = g->layout != VK_IMAGE_LAYOUT_UNDEFINED && s.samples <= 1 && (fp.optimalTilingFeatures & need) == need &&
                          (g->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (g->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageLayout nl = VK_IMAGE_LAYOUT_UNDEFINED;
    if (can_blit) {
        move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(f.cb, ni.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g->aspect, 0, s.levels, 0, s.layers);
        std::vector<VkImageBlit> regions;
        for (uint32_t l = 0; l < s.levels; ++l) {
            const VkExtent3D a = level_extent(g->base, l), b = level_extent(s.base, l);
            VkImageBlit r = {};
            r.srcSubresource = {g->aspect, l, 0, s.layers};
            r.dstSubresource = {g->aspect, l, 0, s.layers};
            r.srcOffsets[1] = {static_cast<int32_t>(a.width), static_cast<int32_t>(a.height), 1};
            r.dstOffsets[1] = {static_cast<int32_t>(b.width), static_cast<int32_t>(b.height), 1};
            regions.push_back(r);
        }
        vkCmdBlitImage(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ni.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                       regions.data(), g->depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        nl = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    gfx::Image old = g->img;
    std::vector<VkImageView> views;
    for (auto& kv : g->views) views.push_back(kv.second);
    g->views.clear();
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);   // the views of the old image go
    defer_destroy([old, views]() mutable {
        for (VkImageView v : views) { vkDestroyImageView(vkdev(), v, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(old);
    });
    g->img = ni;
    g->base = s.base;
    g->layout = nl;
    bump_gen(*g, -1);
    g->scale = to;
    if (to == 1.0f && !is_shadow_map(*t)) g->scale_blocked = true;   // a shadow map goes back to 1 by choice and can be scaled again
    return true;
}

// Why the polls run at the start of every replay call (contract): NVN pool memory is shared with the GPU on the console; a CPU write the
// game made before nvnQueueSubmit (and, for a cached pool, nvnMemoryPoolFlushMappedRange) is seen by the commands of that submission. Here
// the GPU reads the game's memory through device-local copies (pool mirror) and uploaded images (textures of coherent pools), so writes since
// the last replay call must invalidate them before this call's commands are recorded. The game writes pool memory between the submissions
// of one frame (uniform/vertex rings, CPU-written textures), so one poll per frame would bind stale copies to later submissions. The
// poll itself stays per call; setup_coalesce only makes it cheaper: an empty poll reads the summary (~220 words for an 8 GiB region)
// instead of every word of the bitmap.
bool setup_coalesce_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "setup_coalesce", true);
        mem::ww::set_summary_scan(v);
        NXLOG_I(kTag, "setup_coalesce %s ([nvn] setup_coalesce): write-watch polls read %s; the mirror walks %s", v ? "on" : "off",
                v ? "the summary bitmap" : "every bitmap word", v ? "the wanted page range" : "every page");
        return v;
    }();
    return on;
}

void tex_poll_cpu_writes() {
    (void)setup_coalesce_on();
    if (!g_tex_ww) return;
    g_tex_ww_pages.clear();
    const uint64_t w0 = ft::g_split_on ? mem::ww::take_stats().words : 0;
    mem::ww::take_written(g_tex_ww, g_tex_ww_pages);
    if (ft::g_split_on) {
        ft::split_count(ft::kCntTexPoll);
        if (g_tex_ww_pages.empty()) ft::split_count(ft::kCntTexPollEmpty);
        ft::split_add(ft::kCntTexPages, g_tex_ww_pages.size());
        ft::split_add(ft::kCntWwWords, mem::ww::take_stats().words - w0);
    }
    if (g_tex_ww_pages.empty()) return;
    std::sort(g_tex_ww_pages.begin(), g_tex_ww_pages.end());
    const uint64_t base = mem::ww::region_base(g_tex_ww);
    for (size_t i = 0; i < g_tex_ww_pages.size();) {
        size_t j = i + 1;
        while (j < g_tex_ww_pages.size() && g_tex_ww_pages[j] <= g_tex_ww_pages[j - 1] + 1) ++j;   // a run of written pages: one call
        textures_cpu_wrote(reinterpret_cast<const void*>(base + (g_tex_ww_pages[i] << 12)), (g_tex_ww_pages[j - 1] - g_tex_ww_pages[i] + 1) << 12);
        i = j;
    }
}

void tex_transition(Texture* t, Frame& f, VkImageLayout layout) {
    TexGpu* g = tex_or_null(t);
    if (g) move_layout(f, *g, layout);
}

VkImageLayout tex_layout(Texture* t) {
    return (t && t->gpu && !t->gpu->failed) ? t->gpu->layout : VK_IMAGE_LAYOUT_UNDEFINED;
}

void tex_sparse_before_copy(Texture* t, uint32_t layer, uint32_t level) {
    TexGpu* g = tex_or_null(t);
    if (g && g->sparse) sparse_bind_for_copy(t, *g, layer, level);
}

void tex_transition_attach(Texture* t, Frame& f, VkImageLayout layout, uint32_t level) {
    TexGpu* g = tex_or_null(t);
    if (g) move_layout(f, *g, layout, static_cast<int>(level));
}

bool tex_current_on_gpu(Texture* t) {
    const TexGpu* g = (t && t->gpu && !t->gpu->failed) ? t->gpu : nullptr;
    return g && !t->cpu_dirty.load() && !g->streamed && (g->gpu_written || g->uploaded);
}

bool tex_has_feedback_loop(Texture* t) {
    const TexGpu* g = (t && t->gpu && !t->gpu->failed) ? t->gpu : nullptr;
    return g && (g->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) != 0;
}

// Only images that show a feedback draw get the usage: on some GPUs it costs the colour compression of the image (RADV turns DCC off for
// feedback loop images on older chips), so it is not given to every render target at creation. Same shape, format, scale and flags; every
// level and layer is copied over.
bool tex_enable_feedback_loop(Texture* t, Frame& f) {
    TexGpu* g = tex_or_null(t);
    if (!g) return false;
    if (g->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) return true;
    if (g->fb_loop_tried) return false;
    g->fb_loop_tried = true;
    Shape s = shape_of(*t);
    s.base = g->base;
    if (!device().features().attachment_feedback_loop || g->sparse || g->depth || s.samples > 1 || g->type != VK_IMAGE_TYPE_2D ||
        !(g->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !(g->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    const VkImageUsageFlags nu = g->usage | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    gfx::Image ni;
    std::string err;
    if (!device().image_supported(make_desc(s, g->format, nu, g->cflags), &err) ||
        device().create_image(make_desc(s, g->format, nu, g->cflags), &ni, &err) != VK_SUCCESS) {
        NXLOG_W(kTag, "texture %u (%s %ux%u): no feedback loop usage (%s): its feedback draws keep the copy", t->id, g->fmt ? g->fmt->name : "?", g->base.width,
                g->base.height, err.c_str());
        return false;
    }
    static int logged = 0;
    if (logged++ < 40)
        NXLOG_I(kTag, "texture %u: %s %ux%u (%u levels) remade with feedback loop usage (a draw samples it while rendering to it)", t->id, g->fmt ? g->fmt->name : "?",
                g->base.width, g->base.height, g->levels);
    VkImageLayout nl = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(f.cb, ni.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g->aspect, 0, g->levels, 0, g->layers);
        std::vector<VkImageCopy> regions;
        for (uint32_t l = 0; l < g->levels; ++l) {
            VkImageCopy r = {};
            r.srcSubresource = {g->aspect, l, 0, g->layers};
            r.dstSubresource = {g->aspect, l, 0, g->layers};
            r.extent = level_extent(g->base, l);
            regions.push_back(r);
        }
        vkCmdCopyImage(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ni.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                       regions.data());
        nl = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    gfx::Image old = g->img;
    std::vector<VkImageView> views;
    for (auto& kv : g->views) views.push_back(kv.second);
    g->views.clear();
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);   // the views of the old image go (set_reuse memos must not outlive them)
    defer_destroy([old, views]() mutable {
        for (VkImageView v : views) { vkDestroyImageView(vkdev(), v, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(old);
    });
    g->img = ni;
    g->layout = nl;
    g->usage = nu;
    bump_gen(*g, -1);
    return true;
}

uint64_t tex_write_gen(Texture* t, uint32_t level) {
    TexGpu* g = (t && t->gpu && !t->gpu->failed) ? t->gpu : nullptr;
    return g ? g->wgen[std::min(level, kGenLevels - 1)] : 0;
}

void tex_mark_gpu_written(Texture* t, int level) {
    TexGpu* g = tex_or_null(t);
    if (!g) return;
    bump_gen(*g, level);
    g->gpu_written = true;
    t->cpu_dirty.store(false);                   // the GPU data is the newer one until the CPU writes again
}

VkImage tex_image(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->img.image : VK_NULL_HANDLE;
}

VkFormat tex_vk_format(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->format : VK_FORMAT_UNDEFINED;
}

bool tex_promoted(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g && g->promoted;
}

// ---- converting transfers: an image whose format is not the NVN one (an HDR-promoted target, a wider fallback) holds the same texels in
// another layout. A buffer copy addresses the buffer in the image's texel size, so a copy straight into or out of it reads or writes past
// the game's bytes (issue #76: RGBA16F for RGBA8 is twice the bytes, VK_ERROR_DEVICE_LOST on NVIDIA). These go through a transient image
// in the other format and a blit, which converts on the GPU. The transient images are made per transfer (rare) and freed after the frame.

namespace tex {
static bool format_has(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties fp = {};
    vkGetPhysicalDeviceFormatProperties(device().physical(), f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

// A blit can convert between a and b: colour, not block compressed, not integer (a blit does not convert integers to or from floats).
static bool blit_convertible(VkFormat a, VkFormat b) {
    for (VkFormat f : {a, b})
        if (f == VK_FORMAT_UNDEFINED || is_block_format(f) || vk_format_is_integer(f)) return false;
    return true;
}

static bool make_transient(VkImageType type, VkFormat fmt, VkExtent3D e, uint32_t layers, gfx::Image* out, std::string* why) {
    gfx::ImageDesc d;
    d.type = type;
    d.format = fmt;
    d.width = e.width;
    d.height = e.height;
    d.depth = type == VK_IMAGE_TYPE_3D ? e.depth : 1;
    d.array_layers = type == VK_IMAGE_TYPE_3D ? 1 : layers;
    d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.create_view = false;
    d.name = "nvn converting transfer";
    return device().create_image(d, out, why) == VK_SUCCESS;
}

static void free_transient(gfx::Image img) {
    defer_destroy([img]() mutable { device().destroy_image(img); });
}
}  // namespace tex

bool tex_copy_buffer_converted(Texture* t, Frame& f, VkBuffer buffer, const VkBufferImageCopy& c, bool to_texture, const char** why) {
    TexGpu* g = tex_or_null(t);
    *why = nullptr;
    if (!g || !g->fmt) { *why = "no image"; return false; }
    const VkFormat nf = g->fmt->vk;
    if (g->depth || c.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) { *why = "depth/stencil"; return false; }
    if (!blit_convertible(g->format, nf)) { *why = "no blit between the formats"; return false; }
    if (shape_of(*t).samples > 1) { *why = "multisampled"; return false; }
    if (!(g->usage & (to_texture ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT))) { *why = "image lacks transfer usage"; return false; }
    // the texture blits to the transient (to a buffer) or from it (from a buffer); the transient is copied to or from the buffer
    const VkFormatFeatureFlags img_need = to_texture ? VK_FORMAT_FEATURE_BLIT_DST_BIT : VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    const VkFormatFeatureFlags tmp_need = to_texture ? (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
                                                     : (VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    if (!format_has(g->format, img_need) || !format_has(nf, tmp_need)) { *why = "format lacks blit support"; return false; }
    const uint32_t layers = c.imageSubresource.layerCount;
    if (!c.imageExtent.width || !c.imageExtent.height || !c.imageExtent.depth || !layers) { *why = "empty region"; return false; }

    // the region in the image: the NVN texels times the image's scale, clipped to the level
    const VkExtent3D le = level_extent(g->base, c.imageSubresource.mipLevel);
    const float sc = g->scale;
    const auto scaled = [sc](int32_t v, uint32_t lim) { return std::clamp<int32_t>(static_cast<int32_t>(std::lround(v * sc)), 0, static_cast<int32_t>(lim)); };
    const int32_t w = static_cast<int32_t>(c.imageExtent.width), h = static_cast<int32_t>(c.imageExtent.height), dp = static_cast<int32_t>(c.imageExtent.depth);
    VkOffset3D i0 = {scaled(c.imageOffset.x, le.width), scaled(c.imageOffset.y, le.height), c.imageOffset.z};
    VkOffset3D i1 = {scaled(c.imageOffset.x + w, le.width), scaled(c.imageOffset.y + h, le.height), c.imageOffset.z + dp};
    VkOffset3D t1 = {w, h, dp};
    if (g->type != VK_IMAGE_TYPE_3D) { i0.z = 0; i1.z = 1; t1.z = 1; }
    if (i1.x <= i0.x || i1.y <= i0.y) { *why = "region outside the image"; return false; }

    gfx::Image tmp;
    std::string err;
    if (!make_transient(g->type, nf, c.imageExtent, layers, &tmp, &err)) {
        static int logged = 0;
        if (logged++ < 5) NXLOG_W(kTag, "texture %u: no transient %ux%u image for a converting copy: %s", t->id, c.imageExtent.width, c.imageExtent.height, err.c_str());
        *why = "transient image refused";
        return false;
    }
    const VkImageSubresourceLayers ts = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    VkBufferImageCopy tc = c;
    tc.imageSubresource = ts;
    tc.imageOffset = {0, 0, 0};
    // same size: nearest (an exact conversion); a scaled image: linear when the blit's source format filters
    const bool same_size = i1.x - i0.x == w && i1.y - i0.y == h;
    const VkFilter filter = !same_size && format_has(to_texture ? nf : g->format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkImageBlit b = {};
    image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
    if (to_texture) {
        vkCmdCopyBufferToImage(f.cb, buffer, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tc);
        image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        b.srcSubresource = ts;
        b.srcOffsets[1] = t1;
        b.dstSubresource = c.imageSubresource;
        b.dstOffsets[0] = i0;
        b.dstOffsets[1] = i1;
        vkCmdBlitImage(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
    } else {
        b.srcSubresource = c.imageSubresource;
        b.srcOffsets[0] = i0;
        b.srcOffsets[1] = i1;
        b.dstSubresource = ts;
        b.dstOffsets[1] = t1;
        vkCmdBlitImage(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
        image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        vkCmdCopyImageToBuffer(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &tc);
    }
    free_transient(tmp);
    static int logged = 0;
    if (logged++ < 20)
        NXLOG_I(kTag, "texture %u: copy %s %s %ux%u (level %u, %u layer(s)) converted through a %s blit (image format %d)", t->id, to_texture ? "buffer ->" : "-> buffer",
                g->fmt->name, c.imageExtent.width, c.imageExtent.height, c.imageSubresource.mipLevel, layers, g->promoted ? "HDR float16" : "fallback",
                static_cast<int>(g->format));
    return true;
}

bool tex_resolve_converted(Texture* s, Texture* d, Frame& f, const VkImageResolve& r, const char** why) {
    TexGpu *gs = tex_or_null(s), *gd = tex_or_null(d);
    *why = nullptr;
    if (!gs || !gd) { *why = "no image"; return false; }
    if (gs->depth || gd->depth) { *why = "depth/stencil"; return false; }
    if (!blit_convertible(gs->format, gd->format)) { *why = "no blit between the formats"; return false; }
    if (shape_of(*d).samples > 1) { *why = "multisampled destination"; return false; }
    if (!(gd->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { *why = "destination lacks transfer usage"; return false; }
    // s resolves into a transient of its own format (a resolve destination needs the colour attachment feature), blitted into d
    if (!format_has(gs->format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !format_has(gd->format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        *why = "format lacks blit support";
        return false;
    }
    if (!r.extent.width || !r.extent.height) { *why = "empty region"; return false; }
    gfx::Image tmp;
    std::string err;
    if (!make_transient(VK_IMAGE_TYPE_2D, gs->format, r.extent, 1, &tmp, &err)) {
        static int logged = 0;
        if (logged++ < 5) NXLOG_W(kTag, "texture %u: no transient %ux%u image for a converting resolve: %s", d->id, r.extent.width, r.extent.height, err.c_str());
        *why = "transient image refused";
        return false;
    }
    const VkImageSubresourceLayers ts = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageResolve tr = r;
    tr.dstSubresource = ts;
    tr.dstOffset = {0, 0, 0};
    image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    vkCmdResolveImage(f.cb, gs->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tr);
    image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    VkImageBlit b = {};
    b.srcSubresource = ts;
    b.srcOffsets[1] = {static_cast<int32_t>(r.extent.width), static_cast<int32_t>(r.extent.height), 1};
    b.dstSubresource = r.dstSubresource;
    b.dstOffsets[0] = r.dstOffset;
    b.dstOffsets[1] = {r.dstOffset.x + static_cast<int32_t>(r.extent.width), r.dstOffset.y + static_cast<int32_t>(r.extent.height), r.dstOffset.z + 1};
    vkCmdBlitImage(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gd->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    free_transient(tmp);
    static int logged = 0;
    if (logged++ < 20)
        NXLOG_I(kTag, "texture %u: downsample of texture %u (%ux%u) converted through a %s blit (format %d -> %d)", d->id, s->id, r.extent.width, r.extent.height,
                gd->promoted ? "HDR float16" : "fallback", static_cast<int>(gs->format), static_cast<int>(gd->format));
    return true;
}

VkImageAspectFlags tex_aspect(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->aspect : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkExtent3D tex_extent(Texture* t, uint32_t level) { return level_extent(shape_of(*t).base, level); }

uint32_t tex_levels(Texture* t) { return t->levels; }

uint32_t tex_layers(Texture* t) { return t->layers; }

bool tex_is_depth(Texture* t) { return t->fmt && t->fmt->aspect != kAspectColor; }

void tex_destroy(TexGpu* g) {
    if (!g) return;
    if (--g->refs > 0) return;   // another texture object over the same storage still uses the image
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        if (!g_live_set.erase(g)) return;   // already destroyed by tex_shutdown
    }
    g_gauge_textures_live.sub(1);
    for (auto& kv : g->views) { vkDestroyImageView(vkdev(), kv.second, nullptr); vk_gauge_image_views().sub(1); }
    g->views.clear();
    if (g->img) g_live_images.fetch_sub(1);
    device().destroy_image(g->img);
    if (g->rb) device().destroy_buffer(g->rb);   // download_texture's buffer: tex_destroy runs once the GPU is done with the image
    if (g->sparse) {
        for (VkDeviceMemory m : g->sparse->chunks) vkFreeMemory(vkdev(), m, nullptr);
        delete g->sparse;
    }
    delete g;
}

TexStats tex_stats() {
    TexStats s;
    s.images = g_live_images.load();
    s.uploads = counters().texture_uploads.load();
    s.upload_bytes = counters().texture_upload_bytes.load();
    s.transcodes = counters().bc7_transcodes.load();
    s.failed = counters().textures_failed.load();
    return s;
}

// Frees every cached object this module made (samplers, dummy views and images). Render thread, device idle, no texture alive:
// called by the tests before shutdown_for_test (the back end's teardown does not call it yet).
void tex_shutdown() {
    texup::shutdown();                // the cache upgrade threads stop (their work is dropped, never half written)
    texprep::release_staging();       // upload buffers of finished background results belong to the device that is going away
    std::vector<TexGpu*> left;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        left.assign(g_live_set.begin(), g_live_set.end());
    }
    // Textures that are still alive (the game never finalized them, or a test left them behind) keep their place in the registry. They
    // must not keep a pointer to the image object that is about to be freed: texture_registry_find_alias would hand it to the next texture
    // made over the same memory, and that texture would use freed memory (a barrier on VK_NULL_HANDLE, or a crash).
    texture_registry_detach_gpu();
    for (TexGpu* g : left) { g->refs = 1; tex_destroy(g); }
    for (auto& kv : g_sh.samplers)
        for (auto& e : kv.second) { vkDestroySampler(vkdev(), e.second, nullptr); vk_gauge_samplers().sub(1); }
    g_sh.samplers.clear();
    if (g_sh.dummy_sampler != VK_NULL_HANDLE) { vkDestroySampler(vkdev(), g_sh.dummy_sampler, nullptr); vk_gauge_samplers().sub(1); }
    for (auto& kv : g_sh.storage) {
        if (kv.second.view != VK_NULL_HANDLE) { vkDestroyImageView(vkdev(), kv.second.view, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(kv.second.img);
    }
    for (auto& kv : g_sh.sized) {
        if (kv.second.view != VK_NULL_HANDLE) { vkDestroyImageView(vkdev(), kv.second.view, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(kv.second.img);
    }
    for (Dummy* set : {g_sh.color, g_sh.depth, g_sh.dcmp[0], g_sh.dcmp[1], g_sh.dcmp[2], g_sh.dcmp[3], g_sh.dcmp[4], g_sh.dcmp[5], g_sh.dcmp[6], g_sh.dcmp[7]}) {
        for (int i = 0; i < 6; ++i) {
            if (set[i].view != VK_NULL_HANDLE) { vkDestroyImageView(vkdev(), set[i].view, nullptr); vk_gauge_image_views().sub(1); }
            device().destroy_image(set[i].img);
        }
    }
    g_sh = Shared();
}

void mark_texture_cpu_dirty(Texture* t) {
    if (!t) return;
    texprep::on_flushed(t);        // before the flag: a reader that sees the flag also sees the new generation
    t->cpu_dirty.store(true);
}

void texture_bound(Texture* t) { texprep::on_bound(t); }
void texture_registered(Texture* t) { texprep::on_registered(t); }

}  // namespace gpu
}  // namespace nxrt::nvn::be
