// SPDX-License-Identifier: GPL-3.0-or-later
// The boundary between the NVN entry points (host side, api_*.cpp) and the GPU back end (render thread, nvn_gpu*.cpp,
// nvn_replay.cpp). The api files only use the functions below; they never include Vulkan.
#pragma once
#include <cstdint>
#include <vector>

#include "nvn_cmd.h"
#include "nvn_core.h"

namespace nxrt::nvn::be::gpu {

// True when a Vulkan back end is running (install_backend was called with a device). The stub reports false.
bool active();
// True when the Vulkan back end can show debug labels (VK_EXT_debug_utils): the debug group commands are recorded only then. The stub reports false.
bool debug_labels();

// ---- submission --------------------------------------------------------------------------------------------------------
// Replays the lists on the render thread, in order, and returns when the commands are recorded into Vulkan (not when the GPU
// has run them). Returns the serial of the submission: the GPU timeline value that is reached when it has completed.
uint64_t submit(const std::vector<Ref<CmdList>>& lists);
// Submits whatever is pending (nothing is pending in this design: submit() always submits) and returns the last serial.
uint64_t flush();
// Waits until every submission so far has completed on the GPU and its write-backs are visible to the CPU.
void finish();
uint64_t last_serial();
// Waits for `serial` to complete (timeout_ns: 0 poll, UINT64_MAX forever). True when it did.
bool wait_serial(uint64_t serial, uint64_t timeout_ns);
// What asks: Sync = a guest poll (nvnSyncWait with timeout 0), which [nvn] poll_flush may leave in the batch; Wait = the caller goes on to block on the
// serial (or needs it at once), so the batch is flushed immediately.
enum class PollKind { Wait, Sync };
bool serial_done(uint64_t serial, PollKind kind = PollKind::Wait);
// A guest poll answered "not signaled" (nvn_sync_wake.cpp): the calling thread's next short sleep ends when a submission completes.
// token: kernel::early_wake_token() read before the poll. No-op while [kernel] early_wake is off.
void sync_wake_want(uint32_t token);

// Window: texture `index` of the window is shown (after the submissions already made). The call returns when the frame has
// been handed to the swapchain; it honours the shell's frame limiter.
void present(Window* window, int index);

// ---- resources (called from destructors and finalizers, any thread) --------------------------------------------------------
void destroy_texture_gpu(TexGpu* t);          // the Vulkan image of a finalized texture; deferred until the GPU is done with it
void destroy_pool_gpu(PoolGpu* p);
// The game finalized t (or initialized its NVNtexture again): its image goes now, deferred until the GPU is done with it, although a texture
// pool entry may keep the object alive until the game registers another texture at that id. The console's pool entry is a descriptor that
// does not own the texture; ACNH finalizes ~300 textures a second and leaves most of them in its 8192-entry pool. Should the object be used
// again, the image is made again from its storage. Queued behind the submissions already made (they are replayed in order).
void release_texture_gpu(Texture* t);
// A pool was created: import it into Vulkan on a background thread so the first draw that uses it does not wait for the driver (the import
// pins the pages; it takes 10 to 80 ms). A no-op without a back end and for virtual pools.
void prefetch_pool(MemoryPool* p);
// A program got its shaders: queue the recorded pipelines that use exactly these shaders (nvn_prewarm.cpp). A no-op without a back end.
void prewarm_program_shaders(const ShaderInfo* const stages[6]);
// A command buffer was recorded with this program bound (nvn_gpl.cpp): the shader parts of its pipelines are compiled first. Any thread.
void program_bound(const ShaderInfo* const stages[6]);
// The CPU wrote texture memory (nvnMemoryPoolFlushMappedRange, nvnTextureWriteTexels ...): upload again before the next use.
void mark_texture_cpu_dirty(Texture* t);
// A texture is about to be sampled: it was put into a texture pool (nvnTexturePoolRegisterTexture / Image) or its handle was bound while a command
// list was recorded (nvnCommandBufferBindTexture*). Its upload data is prepared in the background (nvn_texprep.h) if the CPU wrote it. Any thread.
void texture_bound(Texture* t);
void texture_registered(Texture* t);   // the registration half of the above: only big textures are acted on

// The GPU image of a texture is copied back into the texture's storage in the game's memory in its block-linear layout (for
// nvnTextureReadTexels / CopyTextureToBuffer users that read render targets on the CPU). Waits for the GPU. False when the
// texture has no GPU image yet or cannot be read back. [nvn] readback_async (default true): only the levels and layers asked for
// (counts clamped to the texture; the defaults ask for all), the render thread records the copy and does not wait for it, and a
// level the GPU did not write since its last readback is not copied again. false: every level, with the GPU drained first (as before).
bool download_texture(Texture* t, uint32_t level_first = 0, uint32_t level_count = UINT32_MAX, uint32_t layer_first = 0,
                      uint32_t layer_count = UINT32_MAX);

// nvnMemoryPoolInvalidateMappedRange / nvnBufferInvalidateMappedRange: the CPU is about to read [addr, +size), which the GPU may have
// written. Compute results the back end holds in video memory there are copied back first (waits for the GPU). Any thread.
void cpu_will_read(uint64_t addr, uint64_t size);

// nvnDeviceSetWindowOriginMode / nvnDeviceSetDepthMode: the replay needs them to place viewports and the depth range.
void set_window_modes(int origin_mode, int depth_mode);

// ---- device facts for the api files ------------------------------------------------------------------------------------
uint64_t timestamp_ns();                       // nvnDeviceGetCurrentTimestampInNanoseconds

}  // namespace nxrt::nvn::be::gpu
