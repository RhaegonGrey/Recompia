// SPDX-License-Identifier: GPL-3.0-or-later
// Compute in the replay: compute shadows and held compute results ("compute out"), async-compute skipping, dispatches.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
// [addr, +size) of game memory a texture upload reads: held compute results there go back first (compute_out_lazy; else all of them).
void out_texture_read(Texture* t) {
    if (!out_lazy()) { flush_compute_out(kOutTexture); return; }
    if (t && t->mem && t->cpu_dirty.load()) compute_out_guard(reinterpret_cast<uint64_t>(t->mem), t->storage_size ? t->storage_size : 4096, kOutTexture);
}

// ---- compute shadows: storage and texel buffers of a dispatch in device-local memory ---------------------------------------------
// Pools are the game's own memory imported into Vulkan, so a shader reaches storage buffers and texel buffers over PCIe. Compute shaders
// that work on such buffers (the exposure histogram's atomics, the 4 MiB simulation buffers) took 9 ms each on an RTX 3080. [nvn]
// compute_shadows (default on): each storage or texel buffer range a dispatch binds (up to 16 MiB) is bound from device-local memory.
//  * Direct ([nvn] compute_direct, default on): the range is in a pool with a device-local copy (nvn_gpu_pool.cpp, the pool mirror). The
//    dispatch binds the copy itself. Pages the copy does not hold (the CPU wrote them since the GPU last used them) are copied in from the
//    pool first (mirror_fill), so only the CPU's writes cross PCIe. A range the shader may write is written in the copy; it goes back to the
//    pool once, when something other than a compute binding of the same range could look at it: before any command other than a dispatch,
//    a state change or a barrier, before a dispatch reads it another way (uniform block, indirect arguments, another range on its pages,
//    a texture upload), and at the end of the submission, before the serial the CPU waits for can signal. A run of dispatches over the
//    same buffers thus copies each result out once instead of once per dispatch.
//  * Shadow (otherwise: no copy for the pool, hot or banned pages, a range over two chunks): the range is copied into a buffer of its own
//    before the dispatch and copied back after it when the shader can write it (and into the pool copy, when there is one).
// Game memory holds every result by the end of the submission, in queue order, as before. Ranges that overlap another range of the same
// dispatch stay in the pool.
struct Shadow { gfx::Buffer buf; VkDeviceSize size = 0; uint64_t last = 0; };
static std::unordered_map<uint64_t, Shadow> g_shadows;            // by GPU address and size
struct ShadowUse { uint64_t addr; VkDeviceSize size; BufRef host; VkBuffer buf; VkDeviceSize off; bool writes; bool direct; MirrorSpan mirror; };
static std::vector<ShadowUse> g_shadow_uses;                       // this dispatch
// Results of direct bindings still only in the pool copy: nvn_gpu_pool.cpp (out_add ... out_end_submit, "compute out").
bool g_shadowing = false;
static bool g_sh_barrier = false, g_sh_copied = false;             // this dispatch: the barrier before the copies in is recorded / some copy in is
static uint64_t g_sh_uses = 0, g_sh_from_mirror = 0, g_sh_bytes_mirror = 0, g_sh_bytes_pool = 0, g_sh_bytes_out = 0, g_sh_no_span = 0;   // summary line
static uint64_t g_sh_direct = 0, g_sh_direct_filled = 0, g_sh_bytes_fill = 0, g_sh_merged = 0, g_sh_flushes = 0;
constexpr VkDeviceSize kShadowMax = 16ull << 20;

static bool compute_shadows_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_shadows", true);
    return on;
}
static bool compute_direct_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_direct", true);
    return on;
}

// Every result still only in the pool copy goes back to the pool (in queue order, before what is recorded next).
void flush_compute_out(int why) {
    if (!out_any()) return;
    // [nvn] pool_copy: out entries need a pool copy (mirror) or a held result, and copy pools have neither (lazy pools get no mirror,
    // out_lazy is off), so nothing is held; were one there, its copy-back into the shadow would be unrecorded: say so once.
    if (pool_copy_on()) {
        static bool noted = false;
        if (!noted) { noted = true; NXLOG_W("nvn.replay", "pool_copy: a compute result is held in a pool copy; its copy-back is not recorded as a GPU write"); }
    }
    end_pass();
    if (out_flush(F->cb, 0, UINT64_MAX, why)) {
        ++g_sh_flushes;
        gstamp("compute out");
    }
}

// [addr, addr + size) is about to be read or written some other way than a direct binding of exactly a held range: the results sharing a
// page with it go back first (a page granular test: the pool copy is filled and tracked by whole pages).
void compute_out_guard(uint64_t addr, uint64_t size, int why) {
    if (!size || !out_overlaps(addr, size)) return;
    end_pass();
    if (out_flush(F->cb, addr, size, why)) {
        ++g_sh_flushes;
        gstamp("compute out");
    }
}

// A buffer a draw reads: results it shares a page with go back first, unless the draw binds the pool copy for it (vertex, index and aligned
// uniform buffers whose pages are all current there, resolve_read).
void out_draw_read(uint64_t addr, uint64_t size, bool copy_ok, int why) {
    if (!addr) return;
    if (!size) size = 65536;
    if (!out_overlaps(addr, size)) return;
    if (copy_ok && out_servable(addr, size)) return;
    compute_out_guard(addr, size, why);
}

// The buffer for [addr, +size) that a command reads through a plain buffer binding (storage buffers, indirect arguments, copy sources): the
// pool copy when it holds a compute result there and is current for the whole range, else the pool, after the results there went back
// (flush: false inside a pass, where out_before_op has done that already).
BufRef held_or_pool(uint64_t addr, uint64_t size, int why, bool flush) {
    if (size && out_overlaps(addr, size)) {
        if (out_lazy() && out_servable(addr, size)) {
            const MirrorSpan ms = mirror_span(addr, size);
            BufRef r = resolve_buffer(addr, size);
            if (ms.buf && r.ok) {
                r.buffer = ms.buf;
                r.offset = ms.offset;
                r.avail = ms.avail;
                return r;
            }
        }
        if (flush) compute_out_guard(addr, size, why);
    }
    return resolve_buffer(addr, size);
}

// The reads of a draw's bound buffers (out_before_op).
static void out_draw_bindings() {
    for (const GState::Bind& b : S.vtx) out_draw_read(b.addr, b.size, true, kOutDrawBuffer);
    const VkDeviceSize ualign = device().properties().limits.minUniformBufferOffsetAlignment;
    for (int st = 0; st < kCompute; ++st) {
        if (!S.stage[st]) continue;
        for (const GState::Bind& b : S.ubo[st]) out_draw_read(b.addr, b.size, ualign && (b.addr % ualign) == 0, kOutDrawBuffer);
        for (const GState::Bind& b : S.ssbo[st]) out_draw_read(b.addr, b.size, true, kOutStorageXfb);   // bound from the copy (build_set)
    }
    for (const GState::Bind& b : S.xfb) out_draw_read(b.addr, b.size, false, kOutStorageXfb);
}

// The bytes from index_address an indexed indirect draw reads: max over its draws of (first index + count) * index size, from the
// arguments (and draw count) when the CPU can read them (not a held result, not written by a submission that has not completed).
// Otherwise the GPU writes them too and nobody knows the count here: everything from index_address to the end of its pool.
uint64_t indirect_index_span(const P_DrawIndirect& p) {
    PoolReasonScope pr_(kReasonIndirect);
    const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
    const uint64_t isz = isz0 ? isz0 : 4;
    const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : 20;
    const auto whole = [&] { const BufRef b = resolve_buffer(p.index_address, 1); return b.ok && b.avail ? static_cast<uint64_t>(b.avail) : uint64_t{1} << 20; };
    uint64_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = out_overlaps(p.count_address, 4) ? nullptr : cpu_view(p.count_address, 4);
        if (!c) return whole();
        uint32_t v = 0;
        std::memcpy(&v, c, 4);
        n = std::min<uint64_t>(v, p.max_count);
    }
    if (!n) return 0;
    const uint64_t args = stride * (n - 1) + 20;
    const uint8_t* a = out_overlaps(p.indirect_address, args) ? nullptr : cpu_view(p.indirect_address, args);
    if (!a) return whole();
    uint64_t end = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t v[3];
        std::memcpy(v, a + i * stride, sizeof v);   // count, instance count, first index
        if (!indirect_args_plausible(v[0], v[1])) return whole();   // not what the GPU will read (stale): the whole pool, as when unreadable
        if (v[0] && v[1]) end = std::max(end, (static_cast<uint64_t>(v[2]) + v[0]) * isz);
    }
    return end;
}

// [nvn] compute_out_lazy: before op, the results it may read or overwrite through the pool go back (instead of all of them before any
// command that is not a dispatch). Ops that only touch images, state and syncs leave them in the copy; a fence's serial is the
// submission's, and out_end_submit decides at its end.
void out_before_op(const RecordReader& r) {
    if (!out_any()) return;
    switch (r.op()) {
        case Op::Dispatch: case Op::DispatchIndirect: case Op::Barrier: case Op::CallCommands: case Op::PushDebugGroup: case Op::PopDebugGroup:
        case Op::ClearColor: case Op::ClearDepthStencil: case Op::ClearTexture: case Op::CopyTextureToTexture: case Op::Downsample:
        case Op::DrawTexture: case Op::DiscardColor: case Op::DiscardDepthStencil: case Op::ResolveDepth: case Op::FenceSync: case Op::WaitSync:
        case Op::ResetCounter: case Op::WaitEvent: case Op::SetRenderEnable:
            return;
        case Op::CopyBufferToTexture: case Op::CopyTextureToBuffer:    // do_copy_buffer_texture: its own range (it knows the size)
            return;
        case Op::DrawArrays:
        case Op::DrawElements: {
            out_draw_bindings();
            if (r.op() == Op::DrawElements) {
                const auto& p = r.payload<P_DrawElements>();
                const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
                out_draw_read(p.index_address, static_cast<uint64_t>(p.count) * (isz ? isz : 4), true, kOutDrawBuffer);
            }
            return;
        }
        case Op::DrawIndirect: {
            out_draw_bindings();
            const auto& p = r.payload<P_DrawIndirect>();
            const bool indexed = p.type != 0xffffffffu;
            const uint64_t cmd = indexed ? 20 : 16;
            const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : cmd;
            const uint64_t n = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
            out_draw_read(p.indirect_address, stride * (n - 1) + cmd, true, kOutIndirect);   // do_draw_indirect binds the copy for them
            if (p.multi && p.count_address) out_draw_read(p.count_address, 4, true, kOutIndirect);
            if (indexed) compute_out_guard(p.index_address, indirect_index_span(p), kOutIndirect);   // the draw binds the pool for its indices
            return;
        }
        case Op::ClearBuffer: {
            const auto& p = r.payload<P_ClearBuffer>();
            compute_out_guard(p.address, p.size, kOutBufferWrite);
            return;
        }
        case Op::CopyBufferToBuffer: {
            const auto& p = r.payload<P_CopyBufferToBuffer>();
            out_draw_read(p.src, p.size, true, kOutBufferRead);       // the copy reads the pool copy (held_or_pool)
            compute_out_guard(p.dst, p.size, kOutBufferWrite);
            return;
        }
        case Op::UpdateUniformBuffer: {
            const auto& p = r.payload<P_UpdateUniformBuffer>();
            compute_out_guard(p.address + static_cast<uint64_t>(p.offset), p.size, kOutBufferWrite);
            return;
        }
        case Op::ReportCounter: compute_out_guard(r.payload<P_Counter>().address, 32, kOutBufferWrite); return;
        case Op::ReportValue: compute_out_guard(r.payload<P_ReportValue>().address, 32, kOutBufferWrite); return;
        case Op::SignalEvent: {
            const auto& p = r.payload<P_Event>();
            if (p.event && p.event->pool && p.event->pool->base)
                compute_out_guard(p.event->pool->gpu_base + static_cast<uint64_t>(p.event->offset), 4, kOutEvent);
            return;
        }
        case Op::DrawXfb: flush_compute_out(kOutIndirect); return;
        default:
            if (r.op() < Op::DrawArrays) return;    // state
            flush_compute_out(kOutOther);           // feedback begin/end and the rest: everything first
            return;
    }
}

static void sh_barrier_once() {
    if (g_sh_barrier) return;
    memory_barrier(F->cb);       // earlier writes to these ranges (any command) before the copies read them
    g_sh_barrier = true;
}

// The buffer a dispatch binds for [addr, addr + size): the pool copy, the shadow (its copy-in recorded now), or `host` when it cannot have
// either.
BufRef shadow_of(uint64_t addr, VkDeviceSize size, const BufRef& host, bool writes) {
    if (!g_shadowing || !host.ok || size == 0) return host;
    for (ShadowUse& u : g_shadow_uses) {
        if (u.addr == addr && u.size == size) {           // the same range bound twice: one binding
            u.writes |= writes;
            BufRef r = host; r.buffer = u.buf; r.offset = u.off; r.avail = size;
            return r;
        }
        if (addr < u.addr + u.size && u.addr < addr + size) {   // overlapping ranges stay coherent in the pool
            compute_out_guard(addr, size, kOutDispatch);
            return host;
        }
    }
    // a result an earlier dispatch left in the pool copy: this dispatch binds it there again (the pages are not valid for the tracker
    // until the result goes back, so mirror_span would not say so)
    {
        VkBuffer ob = VK_NULL_HANDLE;
        VkDeviceSize oo = 0;
        if (out_exact(addr, size, &ob, &oo)) {
            ++g_sh_uses;
            ++g_sh_direct;
            g_shadow_uses.push_back({addr, size, host, ob, oo, writes, true, MirrorSpan{ob, oo, true}});
            BufRef r = host; r.buffer = ob; r.offset = oo; r.avail = size;
            return r;
        }
    }
    if (!out_lazy()) compute_out_guard(addr, size, kOutDispatch);
    if (size > kShadowMax) { compute_out_guard(addr, size, kOutDispatch); return host; }
    const MirrorSpan ms = mirror_span(addr, size);    // pages holding a compute result count as current there (compute_out_lazy)
    if (ms.buf && compute_direct_on()) {
        bool ok = ms.valid;
        if (!ok) {
            sh_barrier_once();
            uint64_t filled = 0;
            ok = mirror_fill(F->cb, addr, size, &filled);
            if (filled) { g_sh_copied = true; g_sh_bytes_fill += filled; }
            if (ok) ++g_sh_direct_filled;
        }
        if (ok) {
            ++g_sh_uses;
            ++g_sh_direct;
            g_shadow_uses.push_back({addr, size, host, ms.buf, ms.offset, writes, true, ms});
            BufRef r = host; r.buffer = ms.buf; r.offset = ms.offset; r.avail = size;
            return r;
        }
    }
    compute_out_guard(addr, size, kOutDispatch);      // the shadow reads the pool: held results there go back first
    const uint64_t key = addr ^ (static_cast<uint64_t>(size) << 40) ^ (static_cast<uint64_t>(size) >> 24);
    Shadow& sh = g_shadows[key];
    if (!sh.buf || sh.size != size) {
        if (sh.buf) { gfx::Buffer old = sh.buf; forget_buffer_views(old.buffer); defer_destroy([old]() mutable { device().destroy_buffer(old); }); sh.buf = gfx::Buffer{}; }
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (device().create_buffer(size, usage, gfx::MemoryUse::GpuOnly, &sh.buf, "nvn compute shadow") != VK_SUCCESS) {
            sh.buf = gfx::Buffer{};
            g_shadows.erase(key);
            return host;
        }
        sh.size = size;
    }
    sh.last = F->serial;
    sh_barrier_once();
    // The device-local pool copy, when it holds the range, is the cheap source (video memory to video memory instead of over PCIe); after a
    // dispatch that writes the range, the result goes to both, so the next dispatch finds the copy valid again (mirror_span_written).
    ++g_sh_uses;
    if (!ms.buf) ++g_sh_no_span;
    if (ms.valid) { ++g_sh_from_mirror; g_sh_bytes_mirror += size; } else g_sh_bytes_pool += size;
    VkBufferCopy c = {ms.valid ? ms.offset : host.offset, 0, size};
    vkCmdCopyBuffer(F->cb, ms.valid ? ms.buf : host.buffer, sh.buf.buffer, 1, &c);
    g_sh_copied = true;
    g_shadow_uses.push_back({addr, size, host, sh.buf.buffer, 0, writes, false, ms});
    BufRef r = host;
    r.buffer = sh.buf.buffer;
    r.offset = 0;
    r.avail = size;
    return r;
}

// After the dispatch: the written shadows go back to the pool; written direct ranges wait in g_out_pending. Shadows unused for a while
// are freed.
static void shadows_after_dispatch() {
    bool any = false;
    for (const ShadowUse& u : g_shadow_uses) {
        if (!u.writes) continue;
        if (u.direct) {
            if (out_add(u.addr, u.size, u.buf, u.off, u.host)) ++g_sh_merged;   // written again before it went back: one copy out for both
            continue;
        }
        if (!any) { memory_barrier(F->cb); any = true; }
        VkBufferCopy c = {0, u.host.offset, u.size};
        vkCmdCopyBuffer(F->cb, u.buf, u.host.buffer, 1, &c);
        // [nvn] pool_copy: u.host is a shadow; the binding recorded this range as a GPU write already (gpu_wrote), the copy-back is that write
        if (pool_copy_on()) pcopy_note_write(u.addr, u.size, u.host.buffer);
        g_sh_bytes_out += u.size;
        if (u.mirror.buf) {
            VkBufferCopy mc = {0, u.mirror.offset, u.size};
            vkCmdCopyBuffer(F->cb, u.buf, u.mirror.buf, 1, &mc);
            mirror_span_written(u.addr, u.size);
        }
    }
    if (any) gstamp("shadow out");
    g_shadow_uses.clear();
    g_shadowing = false;
    static uint64_t last_sweep = 0;
    if (F->serial - last_sweep > 600) {
        last_sweep = F->serial;
        NXLOG_I("nvn.replay", "compute shadows: %llu bound (%llu from the pool copy, %llu with no copy), in from the copy %llu MiB, in from the pool %llu MiB, out %llu MiB; "
                "direct to the pool copy %llu (%llu after filling pages, in %llu MiB, out %llu MiB in %llu flushes, %llu writes merged)",
                (unsigned long long)g_sh_uses, (unsigned long long)g_sh_from_mirror, (unsigned long long)g_sh_no_span, (unsigned long long)(g_sh_bytes_mirror >> 20),
                (unsigned long long)(g_sh_bytes_pool >> 20), (unsigned long long)(g_sh_bytes_out >> 20), (unsigned long long)g_sh_direct,
                (unsigned long long)g_sh_direct_filled, (unsigned long long)(g_sh_bytes_fill >> 20), (unsigned long long)(out_bytes_total() >> 20),
                (unsigned long long)g_sh_flushes, (unsigned long long)g_sh_merged);
        for (auto it = g_shadows.begin(); it != g_shadows.end();) {
            if (F->serial - it->second.last > 600) {
                gfx::Buffer old = it->second.buf;
                forget_buffer_views(old.buffer);
                defer_destroy([old]() mutable { device().destroy_buffer(old); });
                it = g_shadows.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ---- [nvn] async_compute: which dispatches may be skipped while their pipeline builds -----------------------------------------------
// Only one whose writes the CPU cannot see: every storage buffer it binds lies in one pool with NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS (not
// virtual: its physical backing may be CPU-visible) and in no 64 KiB granule a buffer copy ever read into a CPU-visible pool, and no storage
// image it binds is a texture download_texture read back. Image buffers (storage texel buffers) are not checked: such a dispatch is never
// skipped. Render thread.
static std::unordered_set<uint64_t> g_cs_cpu_read;   // granules (address >> 16) copied into CPU-visible pools (run_list, CopyBufferToBuffer)

void note_copy_for_cpu(uint64_t src, uint64_t dst, uint64_t size) {
    static const bool on = async_compute_on();
    if (!on || !size || (pool_flags_at(dst) & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return;
    size = std::min<uint64_t>(size, 1ull << 34);              // a malformed size: bounded (the copy itself fails to resolve)
    for (uint64_t g = src >> 16, e = (src + size - 1) >> 16; g <= e; ++g) g_cs_cpu_read.insert(g);
}

static bool dispatch_cpu_blind(const ProgLayout& pl) {
    for (const BindingDesc& d : pl.bindings) {
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return false;
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const TexResolved r = resolve_image(d);
            if (r.tex && tex_was_read_back(r.tex)) return false;
            continue;
        }
        if (d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.nvn.source != NvnSource::StorageBuffer) continue;   // read only, or ours
        const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);       // as bind_sets
        if (slot < 0 || slot >= 16 || !S.ssbo[d.nvn_stage][slot].addr) continue;                                // the dummy is bound
        const GState::Bind& bind = S.ssbo[d.nvn_stage][slot];
        if (!bind.size) return false;                         // the range would be the rest of the pool: not checked
        const uint64_t lo = bind.addr, hi = bind.addr + bind.size - 1;
        const uint32_t f = pool_flags_at(lo);
        if (!(f & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || (f & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return false;
        if (pool_lookup(lo).pool != pool_lookup(hi).pool) return false;
        for (uint64_t g = lo >> 16; g <= (hi >> 16); ++g)
            if (g_cs_cpu_read.count(g)) return false;
    }
    return true;
}

void do_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    RpScope rp_all(kRpDispatch);
    const ShaderInfo* cs = S.stage[kCompute];
    if (g_trace) trace_dispatch(x, y, z, indirect, cs);
    if (!cs) return;
    const ShaderInfo* stages[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, cs};
    const ProgLayout* pl = prog_layout(stages);
    if (!pl) return;
    prepare_textures(*pl);
    end_pass();
    if (g_trace) trace_dispatch_bindings(*pl);
    // [nvn] async_compute in play (the loading gate): a dispatch whose pipeline is not built and whose writes the CPU cannot see is skipped
    // while its pipeline goes first in line on the async threads (pop-in for a frame or two). Any other is built here as before and logged.
    if (async_compute_on() && gpl_gate_open() && !compute_pipeline_done(cs, *pl) && dispatch_cpu_blind(*pl) && compute_queue_front(cs, *pl)) {
        compute_note_skipped();
        ft::mark("cs.skip");
        static int logged = 0;
        if (logged < 50) {
            ++logged;
            NXLOG_I(kTag, "compute: dispatch skipped while its pipeline builds in the background: %s", cs->hash.c_str());
        }
        return;
    }
    VkPipeline pipe = compute_pipeline(cs, *pl);
    if (!pipe) return;
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    g_cmd.bound_pipeline = VK_NULL_HANDLE;
    support_for_draw(*pl, true);
    const BufRef ind = indirect ? held_or_pool(indirect, 12, kOutIndirect, true) : BufRef{};   // arguments a dispatch wrote: from the copy
    g_shadowing = compute_shadows_on();
    g_shadow_uses.clear();
    g_sh_barrier = g_sh_copied = false;
    bind_sets(*pl, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (g_sh_copied) {
        memory_barrier(F->cb);     // the copies into the shadows before the dispatch reads them
        gstamp("shadow in");
    }
    // [gfx] gpu_checkpoints: a checkpoint before the dispatch names it (shader, groups or the guest address of indirect arguments) if the
    // device is lost while it runs. One compare when off or without NVIDIA's extension.
    if (device().checkpoints_on(gfx::Device::Checkpoint::Dispatch)) {
        if (indirect) device().checkpoint(F->cb, gfx::Device::Checkpoint::Dispatch, &cs->hash, nullptr, static_cast<uint32_t>(indirect),
                                          static_cast<uint32_t>(indirect >> 32), 0, true);
        else device().checkpoint(F->cb, gfx::Device::Checkpoint::Dispatch, &cs->hash, nullptr, x, y, z, false);
    }
    if (indirect) {
        vkCmdDispatchIndirect(F->cb, ind.buffer, ind.offset);
    } else {
        vkCmdDispatch(F->cb, x, y, z);
        if (gpass_on()) gstamp(("cs " + cs->hash.substr(0, 8)).c_str());
    }
    shadows_after_dispatch();
    memory_barrier(F->cb);
    g_cmd.dirty = kAllDirty;
    shadow_invalidate();
    counters().dispatches.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
