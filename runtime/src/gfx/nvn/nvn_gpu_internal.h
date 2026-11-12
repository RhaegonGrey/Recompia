// SPDX-License-Identifier: GPL-3.0-or-later
// Internal interfaces of the Vulkan side of the NVN back end (render thread only). Modules:
//   nvn_gpu.cpp          the Gpu singleton: device access, render thread, submission slots, timeline, deferred destruction, uploads
//   nvn_gpu_pool.cpp     memory pools imported as Vulkan memory, GPU address -> VkBuffer
//   nvn_gpu_texture.cpp  texture images, uploads, views, samplers, layout tracking
//   nvn_replay.cpp       command list interpreter (state, passes, draws, clears, copies)
//   nvn_pipeline.cpp     pipeline layouts, graphics/compute pipelines, descriptor sets
//   nvn_present.cpp      window texture -> swapchain
// Everything declared here must be called on the render thread (gpu::on_render_thread()) unless it says otherwise.
#pragma once
#include <atomic>
#include <mutex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <volk.h>

#include "nxrt/gfx.h"
#include "nvn_present_pass.h"

namespace nxrt::extras { class OutputPass; }
#include "nvn_core.h"
#include "nvn_gpu.h"

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::nvn::be::gpu {

bool on_render_thread();
void flush_caches();              // writes the pipeline cache to disk now (the key file is appended as pipelines are created)
gfx::Device& device();
VkDevice vkdev();

struct Counters {
    std::atomic<uint64_t> submits{0}, lists_replayed{0}, draws{0}, draws_skipped{0}, dispatches{0}, clears{0}, copies{0}, presents{0};
    std::atomic<uint64_t> passes{0}, pass_breaks{0}, pipelines_created{0}, pipeline_cache_hits{0}, descriptor_sets{0};
    std::atomic<uint64_t> textures_created{0}, texture_uploads{0}, texture_upload_bytes{0}, bc7_transcodes{0}, textures_failed{0};
    std::atomic<uint64_t> pools_imported{0}, unresolved_addresses{0}, unsupported_ops{0}, submit_replay_ns{0};
};
Counters& counters();
Counters* counters_if();   // null before the GPU is up
// An operation the back end does not implement was skipped: logged the first time per name, counted for the report.
void note_unsupported(const char* what);
std::string unsupported_report();

// Hands the submissions recorded so far to the GPU ([nvn] submit_batch keeps them back). Call before a queue operation that must come after
// them: the present, sparse binds.
void submit_recorded();
// [nvn] submit_thread: returns once the submit worker has run everything handed to it (no-op when off). Before a queue operation of
// the render thread itself (sparse bind, debug wait-idle submit) and before swapchain work (present_on_render_thread).
void submit_drain();
// The present: with [nvn] submit_batch = frame, the batch goes in the present's own vkQueueSubmit2 (one kernel submission a frame).
// true: p is filled (cmds holds its command buffers); call present_batch_done(p) after Renderer::end_frame(p). false: the batch is
// submitted already (submit_recorded), call end_frame without a prefix.
bool take_batch_for_present(std::vector<VkCommandBuffer>* cmds, gfx::Renderer::SubmitPrefix* p);
void present_batch_done(const gfx::Renderer::SubmitPrefix& p);
// Runs fn on the render thread and waits for it. From the render thread itself it just calls fn.
void run_on_render_thread(const std::function<void()>& fn);
// Test and one-shot hook: runs fn(Frame&) on the render thread with a fresh command buffer, submits it and waits for the GPU.
// Returns the serial of the submission.
uint64_t run_frame_sync(const std::function<void(struct Frame&)>& fn);
// Brings the Vulkan layer up on a device without touching the NVN entry points (tests of nvn_gpu_*.cpp).
bool init_for_test(gfx::Device& dev, gfx::Renderer* renderer = nullptr);
void shutdown_for_test();

// ---- the recording context of one submission ---------------------------------------------------------------------------
struct UploadAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;       // host pointer to the allocation (host visible, coherent)
    VkDeviceSize size = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

struct Frame {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t serial = 0;          // the timeline value this submission signals when the GPU has run it
    void* slot = nullptr;         // nvn_gpu.cpp: the submission slot (command pool, descriptor pools, upload memory)
    // Host-visible memory valid until the submission has completed: small per-draw data (support buffer, driver constants,
    // inline uniform data). align is a power of two.
    UploadAlloc upload(VkDeviceSize size, VkDeviceSize align);
    // A staging buffer for texture data (own buffer when large). Freed when the submission has completed.
    UploadAlloc staging(VkDeviceSize size);
    // A staging buffer somebody else made and filled (background texture preparation): freed like the ones above when the submission has completed.
    UploadAlloc adopt_staging(gfx::Buffer b);
    // Runs fn on the render thread after the submission has completed on the GPU (and, for objects the replay used, no
    // later submission can still be using them if they were last used in this one).
    void defer(std::function<void()> fn);
    // A descriptor set of this submission's pool (freed when the slot is reused). Null on failure.
    VkDescriptorSet alloc_set(VkDescriptorSetLayout layout);
};

// Destruction: fn runs once every submission made so far (and the one being recorded) has completed.
void defer_destroy(std::function<void()> fn);
uint64_t completed_serial();      // cached timeline value; call poll_completed() for a fresh read
uint64_t poll_completed();
uint64_t current_serial();        // the serial of the submission being recorded (or the last one)

// ---- GPU pass profiler ([prof] gpu_passes = true, needs the frame trace) --------------------------------------------------------------
// Writes a GPU timestamp after everything recorded so far; the time since the previous stamp of the submission becomes a trace event
// `gp:<label>` (arg = draws or 0). No-ops when the profiler is off.
bool gpass_on();
void gpass_stamp(Frame& f, const char* label, uint32_t arg = 0);

// ---- descriptors ([nvn] set_reuse; read once, nvn_gpu.cpp) ---------------------------------------------------------------------
bool set_reuse_on();              // a descriptor set whose bindings did not change since the last draw is not resolved again (build_set)

// ---- barriers ---------------------------------------------------------------------------------------------------------------
void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect,
                   uint32_t base_level, uint32_t level_count, uint32_t base_layer, uint32_t layer_count);
// All commands before, all commands after, all memory (used after passes, dispatches, copies and fills).
void memory_barrier(VkCommandBuffer cb);
// [nvn] pass_barriers = tracked (S38): image_barrier derives its stage and access masks from the two layouts instead of all commands and
// all memory. Set by the replay for each submission; any thread may record image barriers, so it is an atomic.
void set_narrow_barriers(bool on);

// ---- pools ---------------------------------------------------------------------------------------------------------------
struct BufRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize avail = 0;       // bytes from offset to the end of the pool buffer
    bool ok = false;
};
// The buffer and offset of `size` bytes at a GPU address. When the address is in no pool (or size runs past the pool), ok is
// false and the buffer is the dummy buffer (a zeroed 64 KiB buffer) so the draw can go on; this is logged once per address.
BufRef resolve_buffer(uint64_t gpu_address, uint64_t size);
VkBuffer dummy_buffer();
// The last 32 bytes of the dummy buffer hold the default of an unset vertex attribute: float (0,0,0,1) and integer (0,0,0,1).
constexpr VkDeviceSize kDummyAttrFloatOffset = 65504, kDummyAttrIntOffset = 65520;
uint64_t pool_imports();          // statistics
// [nvn] pool_lazy (+ pool_lazy_cpu_untile): [gpu_address, +size) lies in a lazily imported pool and no import holds it yet, so a texture
// upload should read it on the CPU rather than import it for one GPU copy. Counts the answer for the summary line.
bool pool_lazy_wants_cpu(uint64_t gpu_address, uint64_t size);
// [nvn] pool_lazy: held by the texture write watch's arm worker around each arm and by a lazy import around making its pages writable and
// importing them; an arm in between made pages read-only again and the import failed (VK_ERROR_INVALID_EXTERNAL_HANDLE) (nvn_gpu_texture.cpp).
std::mutex& tex_arm_import_mutex();
struct PoolLazyStats {
    bool on = false;                  // [nvn] pool_lazy is in effect
    int64_t live_bytes = 0;           // bytes of lazy imports alive now
    uint64_t imported_bytes = 0, imports = 0, spans = 0, dropped = 0, cpu_untiles = 0;   // so far
};
PoolLazyStats pool_lazy_stats();      // any thread (statistics, tests)
// Called from destroy_pool_gpu on the render thread.
void pool_gpu_destroy(PoolGpu* p);
void pool_shutdown();             // destroys every imported pool still alive and the dummy buffer (device idle)
// Device-local copies of pool pages (S38, [nvn] pool_mirror). resolve_read is resolve_buffer for data draws only read (vertex and index
// buffers, uniform blocks): it gives the device-local copy when every page of the range is current there, else the pool. The replay calls
// note_gpu_write for every range a command may write in pool memory (true: a copied page there went stale, so bindings must be made again),
// and mirror_begin_submit before anything else of a submission (it records the copies of the pages draws wanted; on = this submission may
// read copies).
bool mirror_enabled();
enum ReadUse { kReadVertex = 0, kReadIndex = 1, kReadUniform = 2 };
enum WriteUse { kWriteStorage = 0, kWriteXfb = 1, kWriteImageCopy = 2, kWriteUpdate = 3, kWriteFill = 4, kWriteCopy = 5 };
BufRef resolve_read(uint64_t gpu_address, uint64_t size, int use);
// The device-local copy of [gpu_address, +size) for the compute shadows (nvn_replay.cpp): its chunk and offset (buf null: no copy for it, or the
// range spans two chunks), and whether the copy holds the pool's bytes right now. Call it before note_gpu_write invalidates the range.
struct MirrorSpan { VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize offset = 0; bool valid = false; VkDeviceSize avail = 0; };   // avail: bytes to the chunk's end
MirrorSpan mirror_span(uint64_t gpu_address, uint64_t size);
// The GPU wrote the same bytes of the range into the pool and into its copy (a shadow's result): the pages wholly inside the range hold the
// pool's bytes again and are tracked again (unless hot or banned), so the next dispatch reads them from video memory.
void mirror_span_written(uint64_t gpu_address, uint64_t size);
// The compute bindings that use the copy directly (nvn_replay.cpp): records into cb the copies, from the pool, of the pages of the range the
// copy does not hold, and tracks them like the copies of mirror_begin_submit. The caller orders earlier writes to the pool before cb's copies
// and the copies before their readers. True when every page of the range is valid in the copy afterwards; false (nothing or only part of it
// copied) when a page is hot, banned or cannot be tracked, or the range has no copy. *bytes (optional) receives the bytes copied.
bool mirror_fill(VkCommandBuffer cb, uint64_t gpu_address, uint64_t size, uint64_t* bytes);
bool note_gpu_write(uint64_t gpu_address, uint64_t size, int use);
void mirror_begin_submit(Frame& f, bool on);
void mirror_destroy(PoolGpu* p);

// The game's shaders come from NVIDIA's 32-wide warps: shuffles, ballots and votes assume 32 invocations per subgroup. For a stage of a
// game pipeline: a VkPipelineShaderStageRequiredSubgroupSizeCreateInfo asking for 32 (static, shareable) when the GPU's subgroups are not
// 32 wide and it can be asked for that stage (AMD runs 64 wide by default); null otherwise (NVIDIA, or a stage that cannot ask).
// compute_invocations: a compute stage's workgroup size (x * y * z). Vulkan allows the request only when the workgroup fits in
// maxComputeWorkgroupSubgroups subgroups of 32; a bigger workgroup runs at the driver's width instead (asking anyway is invalid usage).
const void* subgroup32_info(VkShaderStageFlagBits stage, uint64_t compute_invocations = 0);

// ---- compute results held in the pool copy (nvn_gpu_pool.cpp, "compute out") ----------------------------------------------------
// A dispatch that binds the pool copy directly (nvn_replay.cpp, compute shadows) writes its result there: the range is "out" until the
// copy-back to the pool. [nvn] compute_out_lazy (default on): at the end of a submission a result is not copied back but held in the
// copy, its pages guarded (nxrt/write_watch.h: the CPU's first access waits for an on-demand copy, out_fetch); draws read it from the copy
// (resolve_read), and the other GPU readers of the pool get it copied back first (out_flush). Results in pages the CPU has read before
// (a guard fault or nvnMemoryPool/BufferInvalidateMappedRange) are copied back at the end of the submission, as without the option.
// Off: every result goes back at the end of its submission, and before any command other than a dispatch, a state change or a barrier.
// Render thread unless noted.
bool out_lazy();
bool out_any();                                                    // any thread (a hint)
// The direct binding [addr, +size) at (mbuf, moff) of the copy was written (host: the pool's buffer). True: the same range was out already.
bool out_add(uint64_t addr, VkDeviceSize size, VkBuffer mbuf, VkDeviceSize moff, const BufRef& host);
// The same range is out: its place in the copy.
bool out_exact(uint64_t addr, VkDeviceSize size, VkBuffer* buf, VkDeviceSize* off);
// Some page of [addr, +size) holds a result that is newer in the copy than in the pool.
bool out_overlaps(uint64_t addr, uint64_t size);
// resolve_read would bind the copy for [addr, +size) (every page current there).
bool out_servable(uint64_t addr, uint64_t size);
// Why results went back (the stats line): the end of a submission, or the GPU reader of the pool that needed them.
enum OutWhy {
    kOutGpuReader = 0,      // unspecified reader
    kOutSubmitEnd,          // end of the submission (CPU-read pages, not guardable, or compute_out_lazy off)
    kOutMirrorOff,          // a submission without pool copies (gpuperf A/B off window)
    kOutDispatch,           // a compute binding over a held result that cannot bind the copy (overlapping ranges, no copy, shadow)
    kOutDrawBuffer,         // a vertex, index or uniform buffer the copy cannot serve (two chunks, unaligned uniform ...)
    kOutStorageXfb,         // a storage or feedback buffer of a draw the copy cannot serve
    kOutIndirect,           // indirect or feedback-draw arguments the copy cannot serve
    kOutBufferWrite,        // a command writing pool memory over a held result (clear, copy destination, uniform update, report)
    kOutBufferRead,         // a buffer copy or buffer-texture copy reading a held result the copy cannot serve
    kOutTexture,            // a texture upload reading a held result
    kOutEvent,              // an event signal over a held result
    kOutOther,              // anything else (feedback begin/end, ops without an address)
    kOutWhyCount
};
// Records into cb (outside any pass) the copy-back of every result sharing a page with [addr, +size) (size UINT64_MAX: all), with the
// barriers around it. Returns the bytes.
uint64_t out_flush(VkCommandBuffer cb, uint64_t addr, uint64_t size, int why);
// The end of a submission's recording (serial): each result written in it is held (guarded) or copied back.
void out_end_submit(VkCommandBuffer cb, uint64_t serial);
// Any thread: the CPU will read [addr, +size) (nvnMemoryPool/BufferInvalidateMappedRange; a guard fault calls it too): results held
// there are copied back now (waiting for the GPU), and later results there go back at the end of their submission.
void out_fetch(uint64_t addr, uint64_t size, bool by_fault);
uint64_t out_bytes_total();                                        // bytes copied back so far (statistics)
void out_reset();                                                  // replay shutdown: drops every entry (guards off)

// ---- textures --------------------------------------------------------------------------------------------------------------
// What a binding site needs to see of a texture.
struct ViewRequest {
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;     // the shader's image dimension (from the SPIR-V binding)
    bool depth_compare = false;
    uint32_t base_level = 0, level_count = 0;         // 0 = all levels from base
    uint32_t base_layer = 0, layer_count = 0;         // 0 = all layers from base
    VkFormat format = VK_FORMAT_UNDEFINED;            // UNDEFINED: the image's own format
    VkComponentMapping swizzle = {};                  // identity when all IDENTITY
    bool depth_aspect = true;                         // depth/stencil images: sample depth (true) or stencil (false)
    uint8_t component = 0;                            // the shader's sampled type: 0 unknown, 1 float, 2 sint, 3 uint (raw-bit aliases of 32-bit formats)
};
// Creates the VkImage if needed and returns it (null when the format cannot be created: logged once; callers then bind
// the dummy). Does not upload.
TexGpu* tex_gpu(Texture* t);
// tex_gpu for a texture about to be a render target: an image made now gets the resolution scale when the texture qualifies ([gfx]
// render_scale). tex_gpu_scaled makes the image at a given scale (a copy of a scaled target). Both return the existing image if there is one.
TexGpu* tex_gpu_attachment(Texture* t);
TexGpu* tex_gpu_scaled(Texture* t, float scale);
float render_scale();                                   // [gfx] render_scale (1 until known)
float tex_scale(Texture* t);                            // the image size over the NVN size (1 for most textures)
VkExtent3D tex_image_extent(Texture* t, uint32_t level = 0);   // the image's own size (tex_extent times tex_scale)
// The scale a render target should have: its own when it was brought back to 1 for good, else the render scale when it qualifies.
float tex_scale_wanted(Texture* t);
// Remakes the image at scale `to`, content blitted over (a render target made unscaled, storage use, a pass mixing scales). Going to 1 is
// for good. No render pass open.
bool tex_rescale(Texture* t, Frame& f, float to, const char* why);
// Records an upload when the texture's memory was not yet uploaded or the CPU wrote it since (and the GPU did not render it).
// Must be called with no render pass open on `f.cb`. Leaves the image in SHADER_READ_ONLY_OPTIMAL after an upload.
void tex_ensure_current(Texture* t, Frame& f);
void tex_poll_cpu_writes();   // start of a submission: textures of coherent pools whose pages the CPU wrote become dirty (nvn_gpu_texture.cpp)
// [nvn] setup_coalesce (default on): the per replay call write-watch polls read only the bitmap words a summary marks, and the mirror walks
// only the range of pages draws wanted (nvn_gpu_texture.cpp; same pages found, same copies made). Logs its activation once.
bool setup_coalesce_on();
// Moves the whole image to `layout` (no-op if already there). Call with no render pass open.
void tex_transition(Texture* t, Frame& f, VkImageLayout layout);
VkImageLayout tex_layout(Texture* t);
// tex_transition for a render pass that writes level `level` only: the move counts as a write of that level (tex_write_gen), not of all.
void tex_transition_attach(Texture* t, Frame& f, VkImageLayout layout, uint32_t level);
// The texture has been (or is about to be) written by the GPU: no more uploads from memory until the CPU writes it. `level` >= 0: only
// that level is written (a render pass), else any level may be.
void tex_mark_gpu_written(Texture* t, int level = -1);
// Write generation of an image level: a globally unique number that changes whenever the level may have been written (any layout change
// into a writable layout, a pass on it, a new image). Equal numbers at two times: the level was not written in between. 0: no image.
uint64_t tex_write_gen(Texture* t, uint32_t level);
// Remakes the image with VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT (content copied over) unless it has it. Once per image; false
// when the image cannot have it (the caller keeps the feedback copy). No render pass open.
bool tex_enable_feedback_loop(Texture* t, Frame& f);
bool tex_has_feedback_loop(Texture* t);
// tex_ensure_current would do nothing: the image holds the newest content (rendered or uploaded, the CPU did not write since).
bool tex_current_on_gpu(Texture* t);
void tex_sparse_before_copy(Texture* t, uint32_t layer, uint32_t level);   // a sparse texture: bind the mapped tiles a copy writes
VkImage tex_image(Texture* t);
VkFormat tex_vk_format(Texture* t);
VkImageAspectFlags tex_aspect(Texture* t);
// HDR scene pipeline: the 8-bit sRGB render target is kept as RGBA16F (its image format is not the NVN one).
bool tex_promoted(Texture* t);
// A buffer <-> image copy of a texture whose image lays texels out unlike its NVN format (vk_copy_layout_matches false: an HDR-promoted
// target, a wider fallback). `c` is in NVN terms (texels, the buffer in the NVN format's layout); it goes through a transient image in
// the NVN format that is blitted from or to the texture (the GPU converts; a scaled image's region is scaled). The texture must be in
// TRANSFER_DST_OPTIMAL (to_texture) or TRANSFER_SRC_OPTIMAL. False with *why set when no conversion is possible: nothing recorded.
bool tex_copy_buffer_converted(Texture* t, Frame& f, VkBuffer buffer, const VkBufferImageCopy& c, bool to_texture, const char** why);
// vkCmdResolveImage of s into d when their formats differ (d HDR-promoted): resolved into a transient image of s's format, blitted into
// d. s in TRANSFER_SRC_OPTIMAL, d in TRANSFER_DST_OPTIMAL. False with *why set: nothing recorded.
bool tex_resolve_converted(Texture* s, Texture* d, Frame& f, const VkImageResolve& r, const char** why);
VkExtent3D tex_extent(Texture* t, uint32_t level = 0);
uint32_t tex_levels(Texture* t);
uint32_t tex_layers(Texture* t);
bool tex_is_depth(Texture* t);
// Sampled image view for a binding (cached). Null when impossible (the caller binds the dummy view).
VkImageView tex_sampled_view(Texture* t, const ViewRequest& req);
// Single-level, single-layer (or all layers when layer_count > 1) view for use as an attachment.
VkImageView tex_attachment_view(Texture* t, uint32_t level, uint32_t layer, uint32_t layer_count, VkFormat view_format);
// The format colour attachments of this texture are rendered through (integer 32-bit formats as their float alias).
VkFormat tex_attachment_format(Texture* t, VkFormat view_format);
// The image can be bound as a storage image (created with NVN_TEXTURE_FLAGS_IMAGE).
bool tex_has_storage(Texture* t);
VkFormat tex_storage_format(Texture* t, VkFormat requested);   // the view format a storage binding uses: the UNORM partner of an sRGB image
// The layout a texture is sampled in: SHADER_READ_ONLY_OPTIMAL, or DEPTH_STENCIL_READ_ONLY_OPTIMAL for depth and stencil formats (so
// the same layout serves a depth texture that is also the read-only depth attachment of the pass).
VkImageLayout tex_sampled_layout(Texture* t);
// A BUFFER-target texture as a texel buffer view over the pool buffer that holds its texels (the VkBuffer storage buffers and CPU writes
// use, so the view always sees current data). `view` = the texture pool entry's view (format) or null; component = the shader's sampled
// type (0 any, 1 float, 2 sint, 3 uint: a float read of a 32-bit integer format reads the raw bits); shader_format = the storage image
// format the SPIR-V declares (UNDEFINED: the texture's own). Null when impossible (the caller logs and leaves the dummy).
// *gpu_address / *bytes receive the range viewed (for gpu_wrote). Views are kept until their pool's buffer is destroyed.
VkBufferView tex_buffer_view(Texture* t, const TextureViewInfo* view, int component, VkFormat shader_format, bool storage,
                             uint64_t* gpu_address = nullptr, uint64_t* bytes = nullptr, VkFormat* format = nullptr);
// make_buffer_view: a cached texel buffer view (null when Vulkan refuses it, remembered).
VkBufferView make_buffer_view(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkFormat format);
// forget_buffer_views: drop (and destroy once the GPU is done) the cached views of a buffer about to be destroyed.
void forget_buffer_views(VkBuffer buffer);
// Destroys the texel buffer views over `buffer` (pool_gpu_destroy, before the buffer goes).
void buffer_views_forget(VkBuffer buffer);
// A texel buffer view of `format` over the zeroed dummy buffer, for texel buffer descriptors that cannot be resolved.
VkBufferView dummy_buffer_view(VkFormat format, bool storage);
// A 1x1 storage image view in GENERAL layout for descriptors that cannot be resolved (UNDEFINED format: RGBA8 UNORM).
VkImageView dummy_storage_view(VkImageViewType type, VkFormat format);
// A UNORM (non-sRGB) view of the whole texture for the present pass.
VkImageView tex_present_view(Texture* t);
// Dummy 1x1 views used when a binding cannot be resolved: black texture of the requested type.
VkImageView dummy_view(VkImageViewType type, bool depth_compare);
// Stand-in for a depth-comparison sampler that meets the colour texture t: a 1x1 depth image holding the red value of t's first texel (what the console
// compares against), viewed as `type`. Cached by value and per texture.
VkImageView dcmp_view(Texture* t, VkImageViewType type);
// A black 2D (or 2D array) texture of the given size, for a binding whose only use is a size query over an image that cannot be
// viewed with the shader's type (a 2D view of a 3D image is no sampled view without VK_EXT_image_2d_view_of_3d). Cached per size;
// null for other types or sizes past the device limits.
VkImageView dummy_view_sized(VkImageViewType type, uint32_t width, uint32_t height, uint32_t layers);
VkSampler dummy_sampler();
// Sampler for the NVN sampler state (cached forever). Never null.
VkSampler sampler_for(const SamplerInfo& s);
// sampler_for for a sampler that reads an image of `image_format`: linear filters become nearest when the device cannot filter that format
// (nvn_format_guard.h).
VkSampler sampler_for(const SamplerInfo& s, VkFormat image_format);
VkSampler sampler_for(const SamplerInfo& s, Texture* sampled);   // the image format of `sampled` (null or no image: unchanged)
// Buffer <-> image copies with the NVN conventions (used by the replay for CopyBufferToTexture etc.).
// Debug: the content of level 0 / layer 0 of a texture the GPU has written, as a PNG (HDR values squashed with x / (1 + x), integer
// formats show their low bytes as colour) and its per-channel statistics. Render thread; waits for the GPU. False: not dumpable.
bool dump_texture(Texture* t, const std::string& png_path, std::string* stats);
// Debug: records, into the frame being replayed, a copy of level 0 / layer 0 of the texture (no pass may be open) and keeps it until
// snapshot_write(): returns an index (-1: not copyable). snapshot_write writes it as dump_texture does and frees the buffer.
int snapshot_record(Texture* t, Frame& f);
bool snapshot_write(int index, const std::string& png_path, std::string* stats);
// [nvn] trace_dump_tex = <dir>: writes the data prepare_texture builds for t (raw blocks of every level and layer) as t<id>.bin with a t<id>.txt describing it, once per texture.
void trace_dump_prepared(Texture* t, const std::string& dir);
int snapshot_record_raw(Texture* t, Frame& f);   // block-compressed level 0 as the GPU holds it; snapshot_write writes the bytes
void tex_destroy(TexGpu* g);      // render thread
void tex_shutdown();              // frees samplers and dummy resources (device idle)
// Image formats chosen: bool descriptor for tests and logs
struct TexStats { uint64_t images = 0, uploads = 0, upload_bytes = 0, transcodes = 0, failed = 0; };
TexStats tex_stats();

// ---- replay (nvn_replay.cpp) ----------------------------------------------------------------------------------------------
// Called by nvn_gpu.cpp inside a submission: begin_submit resets per-submit tracking, replay_lists interprets the lists in
// order (state persists across lists and submissions), end_submit ends the open render pass and emits the closing barrier.
void replay_begin_submit(Frame& f);
void replay_lists(Frame& f, const std::vector<Ref<CmdList>>& lists);
void replay_end_submit(Frame& f);
void replay_request_trace();      // trace the next whole presented frame (any thread)
void replay_shutdown();           // destroys pipelines and layouts (render thread, device idle)

// ---- [nvn] gpu_split (nvn_gpu.cpp) -----------------------------------------------------------------------------------------
// The recording time of one piece in TSC ticks (ft::tsc; 0: off) and the most pieces one submission is cut into.
uint64_t split_budget_ticks();
uint32_t split_max_pieces();
// Ends f's command buffer and sends it to the queue now (after the submissions recorded before it), signalling nothing; f.cb is then a fresh
// command buffer of the same slot. The caller splits only where nothing has to carry over from one command buffer to the next (no render pass,
// query or debug label open) and treats f.cb as new (no bound state). false: not split (no command buffer left, device lost), f unchanged.
bool split_piece(Frame& f);
void split_note_blocked();        // a due split met an open query or debug label (the summary line counts them)

// ---- shared facts (nvn_gpu.cpp) ----------------------------------------------------------------------------------------
int window_origin_mode();
int depth_mode();
gfx::Renderer* renderer();
FrameLimiter* limiter();
PresentPass* present_pass();
extras::OutputPass* output_pass();
bool keep_aspect();

// After a traced frame ([nvn] trace_frame): dumps the targets it drew to (PNG next to the log: trace_<texture id>_<format>.png) and logs their statistics.
void trace_dump_targets();

// ---- present -----------------------------------------------------------------------------------------------------------
void present_on_render_thread(Window* w, int index, bool again = false);   // again: present_again (not counted as a game present)
void present_again();

// ---- never-block compiles (WP06) -------------------------------------------------------------------------------------------
// The replay's frame detector (note_frame_for_pacing, nvn_replay.cpp): true while the game loads (light frames while it stalls, or boot's
// light run), and before the first present. Runs whether or not [display] unpaced_loading acts on it. Render thread.
bool replay_loading();
// The NVNmemoryPoolFlags of the pool holding a GPU address, 0 when it is in no pool (nvn_gpu_pool.cpp). Any thread.
uint32_t pool_flags_at(uint64_t gpu_address);
// True once download_texture copied the texture's image into game memory (the CPU reads it): [nvn] async_compute never skips a dispatch
// that writes it (nvn_gpu_texture.cpp). Render thread; creates nothing.
bool tex_was_read_back(Texture* t);
// ---- readback ([nvn] readback_async / readback_defer, nvn_gpu.cpp / nvn_gpu_texture_readback.cpp) ---------------------------------
// Any thread but the render thread. queue_record: queue_submit without the GPU wait (returns the serial once the render thread recorded
// the job). wait_serial_grace: up to grace_ns for the render thread to flush the serial itself, then wait_serial(serial, UINT64_MAX).
uint64_t queue_record(std::function<void(struct Frame&)> record);
// queue_readback_post: returns at once with the reserved serial; the render thread records the job in queue order and flushes the batch
// right after it, so the copy reaches the GPU with no thread waiting for it (a deferred readback is never waited for, only polled).
uint64_t queue_readback_post(std::function<void(struct Frame&)> record);
// poll_readback_serial: the GPU timeline against the serial, a query only (nothing is flushed or waited for). Failed: no back end, no
// serial, or the device is lost. progress ([nvn] readback_log): where the queue stands.
enum class ReadbackPoll { Pending, Complete, Failed };
struct ReadbackProgress { uint64_t reserved = 0, flushed = 0, submitted = 0, completed = 0; };
ReadbackPoll poll_readback_serial(uint64_t serial, ReadbackProgress* progress = nullptr);
struct WaitTimes { uint64_t grace_ns = 0, wait_ns = 0; bool forced_flush = false; };   // [nvn] readback_log: where a read's wait went
bool wait_serial_grace(uint64_t serial, uint64_t grace_ns, WaitTimes* times = nullptr);
// download_texture's counts (any thread; tests, the summary): copies recorded, readbacks skipped as unchanged, readbacks in all.
struct ReadbackStats { uint64_t copies = 0, skipped = 0, calls = 0; };
ReadbackStats readback_stats();

// ---- pool import attribution ([diag] stutter_logs; nvn_gpu_pool.cpp import_range) -----------------------------------------------------
// Why the code on this thread is resolving pool memory: import_range counts every import under the reason current when it ran
// (gauges nvn.pool.import.reason.<name>). Render thread; an import on another thread counts as kReasonOther.
enum PoolReason { kReasonOther, kReasonVertex, kReasonIndex, kReasonUniform, kReasonStorage, kReasonTexel, kReasonIndirect, kReasonCopy,
                  kReasonImageCopy, kReasonUpdate, kReasonUntile, kReasonCount };
inline thread_local PoolReason t_pool_reason = kReasonOther;
struct PoolReasonScope {
    PoolReason saved;
    explicit PoolReasonScope(PoolReason r) : saved(t_pool_reason) { t_pool_reason = r; }
    ~PoolReasonScope() { t_pool_reason = saved; }
    PoolReasonScope(const PoolReasonScope&) = delete;
    PoolReasonScope& operator=(const PoolReasonScope&) = delete;
};
inline PoolReason pool_reason() { return t_pool_reason; }
// [nvn] one_submit (render thread, in the present): present_take_parked takes the batch flushes whose submit-worker jobs have not started
// yet, before the present's submit_drain; take_batch_for_present then puts them in the present's vkQueueSubmit2 ahead of the frame's own
// batch. present_give_back hands what was taken and not used back to the worker, each flush on its own (a present that ends early, or
// one about to wait in the frame limiter). No-ops when one_submit is off.
void present_take_parked();
void present_give_back();

}  // namespace nxrt::nvn::be::gpu

// ---- pool storage (nvn_gpu_pool.cpp, nvn_pool_copy.cpp) ------------------------------------------------------------------------
#include <deque>
#include <memory>
namespace nxrt::nvn::be {
namespace gpu { struct Mirror; struct PoolTrack; }

// One Vulkan buffer over a byte range of a pool: an import (its own private view, VkDeviceMemory and VkBuffer), or with [nvn] pool_copy a
// host-visible buffer of its own (shadow) that holds a copy of the range (nvn_pool_copy.cpp).
struct PoolChunk {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    void* view = nullptr;             // private small view of the heap section the import goes through (nullptr: imported at its own address)
    uint64_t off = 0;                 // pool offset (from PoolGpu::base) of byte 0 of the import
    VkDeviceSize size = 0;
    bool tried = false;               // import attempted
    bool failed = false;              // the last attempt failed: tried again after a backoff (nvn_gpu_pool.cpp retry_due), never for good
    uint16_t fails = 0;               // failed attempts in a row
    uint32_t failed_at = 0;           // present count of the last failed attempt
    uint32_t fail_since = 0;          // present count of the first failed attempt of the run (the recovery log line)
    uint32_t used_at = 0;             // present count of the last bind that resolved to it (lazy_drop_idle, oom_drop_idle)
    uint64_t used_serial = 0;         // the submission being recorded at that bind (dropped only once it has completed)
    // [nvn] pool_copy: buffer = shadow.buffer, mem = VK_NULL_HANDLE. page_copied[k] = 1 + the last submission the guest -> shadow copy of
    // page k of the chunk serves (0: no copy yet). land_refs: GPU writes recorded into it that have not landed in guest memory yet (under the landing mutex).
    // cov_*: chunk pages [cov_lo, cov_hi) were all copied in submission cov_serial (the touch's fast path). Render thread except land_refs.
    gfx::Buffer shadow;
    bool is_shadow = false;
    std::vector<uint64_t> page_copied;
    std::vector<uint32_t> page_gen;   // the write tracker's generation of page k when it was copied (nvn_pool_track.h); 0: copy at every use
    uint32_t land_refs = 0;
    uint64_t cov_serial = 0, cov_lo = 0, cov_hi = 0;
    bool again = false;               // its cells had a shadow before, dropped since (statistics: copies that fill it again)
    uint32_t filled = 0;              // pages with a copy (statistics: how much of the shadow is in use)
    struct PoolGpu* owner = nullptr;  // shadow chunks: the pool (eviction)
};

// A pool is imported whole (one chunk; the background importer does it) or, when it is larger than one chunk ([nvn] pool_import_chunk_mb), in
// chunks imported on the render thread the first time an address in them is resolved. Chunk k covers pool bytes [k*stride, k*stride + clen):
// neighbouring chunks overlap by clen - stride bytes, so any range that short lies wholly in one chunk. Each chunk goes through its own private
// view of the section (never the same host pointer twice: the spec lets a driver refuse importing the same pointer again). A longer range
// that crosses a stride boundary gets a span: one more import over the strides it touches (rare; logged).
struct PoolGpu {
    uint8_t* base = nullptr;          // aligned start of the pool's imports
    VkDeviceSize size = 0;            // aligned size
    bool failed = false;              // cannot be imported at all (no VK_EXT_external_memory_host, no storage): final; a failed chunk is retried
    bool viewed = false;              // imports go through private views of the heap section (the mirror needs that)
    int flags = 0;
    uint64_t stride = 0;              // chunk k starts at k * stride (whole import: stride = size)
    uint64_t clen = 0;                // bytes per chunk (the last one may be shorter)
    std::vector<PoolChunk> chunks;
    std::deque<PoolChunk> spans;      // imports of ranges longer than the overlap that cross a stride boundary (deque: stable addresses)
    gpu::Mirror* mirror = nullptr;  // device-local copy of the pages draws read (S38, [nvn] pool_mirror); render thread
    bool mirror_tried = false;
    // [nvn] pool_lazy: imported in pieces as binds first touch them (stride = the cell size). The pool is cut into cells; an import (ext) is
    // a run of whole cells, made when a bind touches cells no import holds yet, and no two of them share a page (same-address imports
    // that overlap are refused by some drivers: VK_ERROR_INVALID_EXTERNAL_HANDLE on the RX 9060 XT). cell_ext[k] = index in ext of the
    // import holding cell k, -1: none. A bind that runs from one import into cells another one holds gets a span (overlapping; spans).
    // An import no bind used for [nvn] pool_lazy_idle presents is dropped (lazy_drop_idle).
    bool lazy = false;
    std::deque<PoolChunk> ext;
    std::vector<int32_t> cell_ext;
    std::vector<int32_t> ext_free;    // indices of dropped imports in ext, reused
    // [nvn] pool_copy: the pool's cells (and spans) are shadows, never imports (lazy is set too: same cell structure). page_gpu[p] = the
    // highest serial with a GPU write recorded into page p of the pool (any chunk or span), 0 once that write has landed. Copy-mode spans
    // are destroyed in place, never erased (land records point at them). page_landed[p]: a GPU write landed in page p since a shadow
    // last copied it (statistics: the copy that follows fetches bytes a shadow gave). cell_had[k]: cell k had a shadow before.
    bool copy = false;
    std::unique_ptr<std::atomic<uint64_t>[]> page_gpu;
    std::unique_ptr<std::atomic<uint8_t>[]> page_landed;
    std::vector<uint8_t> cell_had;
    gpu::PoolTrack* track = nullptr;  // which pages the CPU wrote since they were copied (nvn_pool_track.h); null: every bind copies
};

namespace gpu {
// ---- [nvn] pool_copy = auto | on | off (nvn_pool_copy.cpp, nvn_pool_land.cpp) ---------------------------------------------------
// Copy (shadow) mode: pool cells live in host-visible Vulkan buffers of their own instead of userptr imports of guest memory. Every resolve
// copies the pages of the range it touches that the CPU wrote since their last copy (nvn_pool_track.h; at most once per submission, never
// over a page with a GPU write in flight); GPU writes are recorded at record time and copied back into guest memory when their serial
// completes, before any caller sees it complete (read_completed).
bool pool_copy_on();                                  // read once (auto: Wine, or an integrated GPU with <=8 GiB of device-local memory); logged
uint64_t pool_copy_cell();                            // [nvn] pool_copy_cell_mb in bytes
uint32_t pool_copy_idle();                            // [nvn] pool_copy_idle presents
bool pool_copy_any();                                 // any thread: a copy pool exists (cheap)
void pcopy_pool_init(PoolGpu* g);                     // import_pool: page_gpu, the write tracker, the landing thread
void pcopy_pool_destroy(PoolGpu* g);                  // pool_gpu_destroy, after its chunks: the write tracker goes
void pcopy_track_stop(PoolGpu* g);                    // a cell of the pool is imported instead: no page of it stays read-only
// A shadow over pool bytes [off, off + size) into c (what import_range does for an import). False: the caller imports it instead.
// cells: the range is whole cells of the pool (an ext entry), not a span.
bool pcopy_alloc(PoolGpu* g, PoolChunk& c, uint64_t off, uint64_t size, const char* what, bool cells);
// A shadow span over the cells [off, off + size) touches: an existing one or a new one (*made). Null: none could be made.
PoolChunk* pcopy_span(PoolGpu* g, uint64_t off, uint64_t size, bool* made);
void pcopy_free(PoolChunk& c);                        // destroy_chunk of a shadow (its land records are dropped)
bool pcopy_busy(const PoolChunk& c);                  // GPU writes into it have not landed yet (land_refs != 0)
void pcopy_touch(PoolGpu* g, PoolChunk* c, uint64_t off, uint64_t size);   // resolve_in_pool: guest -> shadow for the range (off: pool offset)
// note_gpu_write: a GPU write of the submission being recorded (hint: the buffer it writes, when the caller knows it)
void pcopy_note_write(uint64_t gpu_address, uint64_t size, VkBuffer hint = VK_NULL_HANDLE);
// The GPU reads [gpu_address, +size) from the buffer the last resolve of gpu_address gave (a bind resolved with a smaller size, e.g. the
// index buffer of an indirect draw): the rest of the range is copied too. No-op when copy mode is off.
void pool_copy_touch(uint64_t gpu_address, uint64_t size);
uint64_t pcopy_land(uint64_t reached);                // any thread: lands every recorded write of a serial <= reached (read_completed)
void pcopy_shutdown();                                // pool_shutdown: stops the landing thread, drops records
void pcopy_log_summary(uint32_t presents);            // every 1200 presents (lazy_drop_idle)
void pcopy_evict_for(uint64_t bytes);                 // over [nvn] pool_copy_mb: drops least recently used idle shadows first
// nvn_gpu_pool.cpp: removes a lazy (or copy) cell import / shadow from its pool and destroys it (lazy_drop_idle, pcopy_evict_for).
void pool_drop_ext(PoolGpu* g, size_t ext_index);
void pool_drop_span(PoolGpu* g, PoolChunk& c);
bool pool_drop_chunk_if_live(PoolGpu* g, PoolChunk* c);   // pcopy_evict_for: under g_live_mu, only if g is still live and owns c
// nvn_gpu.cpp: waits up to timeout_ns for the timeline to reach serial (the landing thread). False: timed out, device lost or no back end.
bool timeline_wait(uint64_t serial, uint64_t timeout_ns);
// nvn_sync_wake.cpp: the thread that sees completions for polling game threads; with the back end (install / tear_down).
void sync_wake_start();
void sync_wake_stop();
uint64_t recording_serial();                          // the serial being recorded, 0 outside a recording (render thread)
uint64_t queued_serial();                             // the serial of the last submission queued (any thread)
struct PoolCopyStats {
    bool on = false;
    int64_t live_bytes = 0;
    uint64_t chunks = 0, allocs = 0, copied_bytes = 0, landed_bytes = 0, records = 0, pending = 0;
    uint64_t write_unmatched = 0, import_fallback = 0, unreadable = 0, span_writes = 0, evicted = 0;
};
PoolCopyStats pool_copy_stats();                      // any thread (statistics, tests)
}  // namespace gpu
}  // namespace nxrt::nvn::be
