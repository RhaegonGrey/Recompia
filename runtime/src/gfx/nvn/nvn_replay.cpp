// SPDX-License-Identifier: GPL-3.0-or-later
// The command list interpreter of the NVN back end (render thread). Walks recorded lists in order, keeps the GPU state the
// console keeps (it persists across lists and submissions), and turns it into Vulkan: dynamic-rendering passes opened lazily on the
// first draw or clear of a render target set, graphics pipelines keyed by the baked state, descriptor sets built from the
// translated shaders' binding tables (nvn_shaders.h), vertex and index data straight from the imported pool memory.
// Lessons applied from SuperMarioWonderRecomp docs/nvn/DEBUG.md and DEBUG-2.md (see work notes in docs/S4d.md): viewport flip by a
// negative-height viewport, a memory barrier after every pass, textures sampled by a draw are made current before the pass,
// handles never dropped by count, deferred bind arrays read at record time.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
constexpr int kMaxDepth = 8;

GState S;
Frame* F = nullptr;
constexpr const char* kRpNames[kRpCount] = {"rp.draw", "rp.sets", "rp.set.build", "rp.set.write", "rp.set.bind", "rp.prep", "rp.pass",
                                            "rp.pipe", "rp.dyn", "rp.vb", "rp.dispatch", "rp.set0", "rp.set1", "rp.set2", "rp.set3",
                                            "rp.miss0", "rp.miss1", "rp.miss2", "rp.miss3", "rp.buf", "rp.tex", "rp.texel", "rp.same0", "rp.same1", "rp.same2", "rp.same3", "rp.reuse",
                                            "rp.begin", "rp.end", "rp.opin", "rp.op", "rp.tail", "rp.texpoll", "rp.mirror", "rp.outend"};
// Stamps are TSC ticks (ft::tsc), converted to ns in rp_flush. Disjoint "top" phases that together cover the replay zone: rp.begin and rp.end
// (per replay call: tex_poll_cpu_writes, mirror_begin_submit, out_end_submit, end_pass, ...), rp.opin (mark_dirty + out_before_op per op),
// rp.draw (begin_draw), rp.tail (what a draw op does after begin_draw: index resolve, vkCmdDraw*), rp.op (every other op, dispatch setup
// excluded: rp.dispatch), plus the NXFT zones rp.cbsetup / rp.cbend of nvn_gpu.cpp (slot, begin / end command buffer, query resets, batch
// flush). rp.texpoll, rp.mirror, rp.outend are parts of rp.begin / rp.end. The per-binding phases (rp.buf, rp.tex, rp.texel) run only with
// [prof] replay_fine (a scope each costs more than the work of a cached binding).
RpProfile g_rp;
static void rp_flush() {
    if (!g_rp.t0) return;
    static uint16_t ids[kRpCount];
    static bool named = false;
    if (!named) { for (int i = 0; i < kRpCount; ++i) ids[i] = ft::zone_id(kRpNames[i]); named = true; }
    for (int i = 0; i < kRpCount; ++i)
        if (g_rp.n[i]) ft::event(ids[i], g_rp.t0, g_rp.t0 + ft::tsc_to_ns(g_rp.ns[i]), g_rp.n[i]);
    std::memset(g_rp.ns, 0, sizeof g_rp.ns);
    std::memset(g_rp.n, 0, sizeof g_rp.n);
    g_rp.t0 = 0;
    g_rp.inner = 0;
}
GpuWrites g_gw;
static void note_gw(uint64_t addr, uint64_t size) {
    if (!size || !F) return;
    GpuWrite& last = g_gw.ring[(g_gw.next + kGpuWriteRing - 1) % kGpuWriteRing];
    if (g_gw.count && last.lo == addr && last.hi == addr + size) { last.serial = F->serial; return; }
    GpuWrite& w = g_gw.ring[g_gw.next];
    if (g_gw.count == kGpuWriteRing) g_gw.lost_serial = std::max(g_gw.lost_serial, w.serial);
    else ++g_gw.count;
    w = {addr, addr + size, F->serial};
    g_gw.next = (g_gw.next + 1) % kGpuWriteRing;
}

void gpu_wrote(uint64_t addr, uint64_t size, int use) {
    rp_bump(ft::kCntEpGpuWrote);
    note_gw(addr, size);
    if (note_gpu_write(addr, size, use)) {
        g_cmd.dirty |= kVb | kDesc;
        g_cmd.mirror_stale = true;
    }
}
static int g_dbg_depth = 0;                  // debug labels open in this submission (PushDebugGroup / PopDebugGroup)
nxrt::GaugeCounter g_gauge_pipeline_binds("nvn.cnt.pipeline_binds");   // graphics pipeline binds (cumulative)

// [nvn] gpu_split: a submission that has been recorded for split_budget_ticks is cut at the next safe point between two ops and the part
// so far goes to the GPU (split_piece, nvn_gpu.cpp). Safe points, so that nothing has to live on in the next command buffer:
// - no render pass open (after a copy, dispatch, barrier, query op...), or the op is SetRenderTargets: the open pass is ended there instead
//   of at the next draw, as ensure_pass would end it then (end_pass(true): the tracked-barrier bookkeeping carries over; a pass on the same
//   targets begins again with LOAD, after the barrier pending_conflict asks for);
// - no query active (occlusion and primitive counter windows begin and end in one command buffer, and their counts must not be cut);
// - no debug label open (labels do not outlive their command buffer here).
// Between ops no draw or dispatch is half set up; after the split g_cmd starts over as for a new command buffer (pipeline, dynamic state,
// vertex / index buffers, descriptor sets are bound again; compute and the helper passes bind their own state on every use). Off for traced
// submissions (the trace records submissions of its own inside a replay).
struct PieceState {
    bool armed = false;      // this submission may still be split
    bool blocked = false;    // a due split met a query or label (counted once per piece)
    uint32_t left = 0;       // splits still allowed
    uint64_t t0 = 0;         // TSC ticks (ft::tsc): start of the piece being recorded
    uint64_t budget = 0;
};
static PieceState g_piece;

static void piece_point(Op op) {
    if (ft::tsc() - g_piece.t0 < g_piece.budget) return;
    if (g_q.occ_active >= 0 || g_q.cw[0].active >= 0 || g_q.cw[1].active >= 0 || g_dbg_depth > 0) {
        if (!g_piece.blocked) { g_piece.blocked = true; split_note_blocked(); }
        return;
    }
    if (P.open) {
        if (op != Op::SetRenderTargets) return;
        end_pass(true);
    }
    if (!split_piece(*F)) { g_piece.armed = false; return; }
    g_cmd.begin_submit();
    g_piece.t0 = ft::tsc();
    g_piece.blocked = false;
    if (--g_piece.left == 0) g_piece.armed = false;
}

void log_once(LogOnce key, const char* msg) {   // render thread only
    static bool seen[kOnceCount] = {};
    if (!seen[key]) {
        seen[key] = true;
        NXLOG_W(kTag, "%s", msg);
    }
}

bool first_time_key(FirstKey kind, uint64_t id, std::string_view shader, std::string_view binding) {   // render thread only
    uint64_t h = fnv_step(fnv_step(kFnvBasis, kind), id);
    for (unsigned char c : shader) h = fnv_step(h, c);
    h = fnv_step(h, 0x100);   // between the two names (not a byte value)
    for (unsigned char c : binding) h = fnv_step(h, c);
    static std::unordered_set<uint64_t> seen;
    return seen.size() < 4096 && seen.insert(h).second;
}

// ---- the interpreter ----------------------------------------------------------------------------------------------------------
static void apply_bind_program(const P_BindProgram& p) {
    for (int st = 0; st < 6; ++st)
        if (p.stages & (1u << st)) S.stage[st] = p.program ? p.program->stage[st] : nullptr;
    if (key_check_on() && p.program) {   // [nvn] shader_key_check: a by-value copy of what SetShaders hashed, re-hashed at the first draw
        KeyStage ks[6];
        for (int st = 0; st < 6; ++st) ks[st] = {p.program->src_code[st], p.program->src_control[st], p.program->code_hash[st]};
        key_check_bind(reinterpret_cast<uintptr_t>(p.program) ^ (static_cast<uintptr_t>(p.program->serial) << 48), ks);
    }
    if (g_trace) trace_bind_program(p);
}

static void mark_dirty(const RecordReader& r) {
    switch (r.op()) {
        case Op::BindProgram: g_cmd.dirty |= kPipe | kDesc | kVb; break;
        case Op::BindBuffers: g_cmd.dirty |= r.payload<P_BindBuffers>().kind == kBufVertex ? kVb : kDesc; break;
        case Op::BindTextures: case Op::BindImages: case Op::SetTexturePool: case Op::SetSamplerPool: g_cmd.dirty |= kDesc; break;
        case Op::BindVertexAttribs: case Op::BindVertexStreams: g_cmd.dirty |= kPipe | kVb; break;
        case Op::BindBlend: case Op::BindColor: case Op::BindChannelMask: case Op::BindMultisample: case Op::SetDepthClamp: g_cmd.dirty |= kPipe; break;
        case Op::BindPolygon: g_cmd.dirty |= kPipe | kDynRaster | kDynBias; break;
        case Op::BindDepthStencil: case Op::SetDepthBounds: g_cmd.dirty |= kDynDepth | kDynStencil; break;
        case Op::SetViewports: case Op::SetScissors: case Op::SetDepthRanges: g_cmd.dirty |= kDynVp; break;
        case Op::SetStencil: g_cmd.dirty |= kDynStencil; break;
        case Op::SetBlendColor: g_cmd.dirty |= kDynBlend; break;
        case Op::SetLineWidth: case Op::SetRasterizerDiscard: case Op::SetPrimitiveRestart: g_cmd.dirty |= kDynRaster; break;
        case Op::SetPatchSize: g_cmd.dirty |= kPipe; break;   // patch control points are part of the pipeline
        case Op::SetPolygonOffsetClamp: g_cmd.dirty |= kDynBias; break;
        case Op::SetRenderTargets: g_cmd.dirty = kAllDirty; rp_bump(ft::kCntEpTargets); break;
        default: break;
    }
}

// Time of one op of the list, split into rp.opin (mark_dirty, out_before_op) and the op itself (rp.tail for draws, rp.op otherwise).
struct RpOpScope {
    bool on;
    Op op;
    uint64_t ta = 0, tb = 0, inner0 = 0;
    RpOpScope(bool rt, Op o) : on(rt), op(o) { if (on) { inner0 = g_rp.inner; ta = ft::tsc(); } }
    void mid() { if (on) tb = ft::tsc(); }
    ~RpOpScope() {
        if (!on) return;
        const uint64_t tc = ft::tsc();
        const uint64_t exec = tc - tb, inner = g_rp.inner - inner0;
        const int ph = (op == Op::DrawArrays || op == Op::DrawElements || op == Op::DrawIndirect || op == Op::DrawXfb) ? kRpTail : kRpOp;
        g_rp.ns[ph] += exec > inner ? exec - inner : 0;
        ++g_rp.n[ph];
        g_rp.ns[kRpOpIn] += tb - ta;
        ++g_rp.n[kRpOpIn];
        g_rp.inner = inner0 + (tc - ta);
    }
};

static void run_list(const CmdList& list, int depth) {
    if (depth > kMaxDepth) { log_once(kOnceCallDepth, "CallCommands nesting too deep"); return; }
    counters().lists_replayed.fetch_add(1, std::memory_order_relaxed);
    const bool rt = ft::on() && g_rp.t0;
    for (RecordReader r(list); r.next();) {
        if (g_piece.armed && (!P.open || r.op() == Op::SetRenderTargets)) piece_point(r.op());
        RpOpScope rpo(rt, r.op());
        mark_dirty(r);
        if (out_lazy()) out_before_op(r);                                                   // compute shadows: results back to the pool first
        else if (out_any() && !compute_out_may_wait(r.op())) flush_compute_out(kOutOther);
        rpo.mid();
        switch (r.op()) {
            case Op::Nop: break;
            case Op::BindProgram: apply_bind_program(r.payload<P_BindProgram>()); break;
            case Op::BindBuffers: {
                const auto& p = r.payload<P_BindBuffers>();
                const NVNbufferRange* rg = reinterpret_cast<const NVNbufferRange*>(r.blob<P_BindBuffers>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    const uint32_t k = p.first + i;
                    if (p.kind == kBufVertex && k < 16) S.vtx[k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufUniform && p.stage < 6 && k < 16) S.ubo[p.stage][k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufStorage && p.stage < 6 && k < 16) S.ssbo[p.stage][k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufXfb && k < 4) S.xfb[k] = {rg[i].address, rg[i].size};
                }
                break;
            }
            case Op::BindTextures: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < 6 && p.first + i < 64) S.tex[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindImages: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < 6 && p.first + i < 16) S.img[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindVertexAttribs: {
                const auto& p = r.payload<P_BindVertexAttribs>();
                const NVNvertexAttribState* a = reinterpret_cast<const NVNvertexAttribState*>(r.blob<P_BindVertexAttribs>());
                S.attr_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.attr_count; ++i) S.attr[i] = decode(&a[i]);
                break;
            }
            case Op::BindVertexStreams: {
                const auto& p = r.payload<P_BindVertexStreams>();
                const NVNvertexStreamState* a = reinterpret_cast<const NVNvertexStreamState*>(r.blob<P_BindVertexStreams>());
                S.stream_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.stream_count; ++i) S.stream[i] = decode(&a[i]);
                break;
            }
            case Op::BindBlend: {
                NVNblendState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st);
                BlendInfo b = decode(&st);
                if (b.target >= 0 && b.target < 8) S.blend[b.target] = b;
                break;
            }
            case Op::BindColor: { NVNcolorState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.color = decode(&st); break; }
            case Op::BindChannelMask: { NVNchannelMaskState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.chmask = decode(&st); break; }
            case Op::BindDepthStencil: { NVNdepthStencilState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ds = decode(&st); break; }
            case Op::BindPolygon: { NVNpolygonState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.poly = decode(&st); break; }
            case Op::BindMultisample: { NVNmultisampleState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ms = decode(&st); break; }
            case Op::SetTexturePool: {
                TexturePool* tp = static_cast<TexturePool*>(r.payload<P_Pool>().pool);
                if (tp != S.tpool.get()) ++g_rc.pools;   // resolve_texture: handles name other entries now
                S.tpool = Ref<TexturePool>::share(tp);
                break;
            }
            case Op::SetSamplerPool: {
                SamplerPool* sp = static_cast<SamplerPool*>(r.payload<P_Pool>().pool);
                if (sp != S.spool.get()) ++g_rc.pools;
                S.spool = Ref<SamplerPool>::share(sp);
                break;
            }
            case Op::SetViewports: {
                const auto& p = r.payload<P_Rects>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.vp[p.first + i], v + 4 * i, 16);
                S.nvp = std::max(S.nvp, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetScissors: {
                const auto& p = r.payload<P_Rects>();
                const int32_t* v = reinterpret_cast<const int32_t*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.sc[p.first + i], v + 4 * i, 16);
                S.nsc = std::max(S.nsc, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetDepthRanges: {
                const auto& p = r.payload<P_DepthRanges>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_DepthRanges>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.dr[p.first + i], v + 2 * i, 8);
                break;
            }
            case Op::SetDepthBounds: { const auto& p = r.payload<P_DepthBounds>(); S.depth_bounds = p.enable; S.db_near = p.near_; S.db_far = p.far_; break; }
            case Op::SetStencil: {
                const auto& p = r.payload<P_Stencil>();
                for (int f = 0; f < 2; ++f) {
                    const bool hit = p.face == NVN_FACE_FRONT_AND_BACK || (f == 0 && p.face == NVN_FACE_FRONT) || (f == 1 && p.face == NVN_FACE_BACK);
                    if (!hit) continue;
                    if (p.what == 0) S.stencil_ref[f] = p.value;
                    else if (p.what == 1) S.stencil_vmask[f] = p.value;
                    else S.stencil_wmask[f] = p.value;
                }
                break;
            }
            case Op::SetBlendColor: std::memcpy(S.blend_color, r.payload<P_BlendColor>().c, 16); break;
            case Op::SetPointSize: S.point_size = r.payload<P_Float>().v; break;
            case Op::SetLineWidth: S.line_width = r.payload<P_Float>().v; break;
            case Op::SetAlphaRef: S.alpha_ref = r.payload<P_Float>().v; break;
            case Op::SetPolygonOffsetClamp: { const auto& p = r.payload<P_PolygonOffsetClamp>(); S.bias_factor = p.factor; S.bias_units = p.units; S.bias_clamp = p.clamp; break; }
            case Op::SetSampleMask: S.sample_mask = r.payload<P_U32>().v; break;
            case Op::SetRasterizerDiscard: S.rasterizer_discard = r.payload<P_U32>().v != 0; break;
            case Op::SetDepthClamp: S.depth_clamp = r.payload<P_U32>().v != 0; break;
            case Op::SetPrimitiveRestart: { const auto& p = r.payload<P_PrimitiveRestart>(); S.prim_restart = p.enable != 0; S.restart_index = p.index; break; }
            case Op::SetPatchSize: S.patch_size = r.payload<P_U32>().v; g_cmd.dirty |= kPipe; break;   // the patch size is in the pipeline key
            case Op::SetTessLevels: {
                const auto& p = r.payload<P_TessLevels>();
                if (p.outer) std::memcpy(S.tess_outer, p.v, sizeof S.tess_outer);
                else std::memcpy(S.tess_inner, p.v, sizeof S.tess_inner);
                break;
            }
            case Op::SetRenderEnable:
                if (r.size() >= sizeof(RecHeader) + sizeof(P_RenderEnableCond)) {
                    const auto& p = r.payload<P_RenderEnableCond>();
                    S.render_enable = conditional_render_enabled(p.mode, p.address);
                } else {
                    S.render_enable = r.payload<P_U32>().v != 0;
                }
                break;
            case Op::SetCoverageModulationTable: case Op::SetConservativeRaster: break;
            case Op::SetRenderTargets: {
                const auto& p = r.payload<P_RenderTargets>();
                ++S.targets_gen;
                S.ncolor = p.count;
                for (uint32_t i = 0; i < 8; ++i) {
                    S.color_tex[i] = i < p.count ? Ref<Texture>::share(p.color[i].tex) : Ref<Texture>();
                    if (i < p.count) { S.color_view[i] = p.color[i].view; S.color_has_view[i] = p.color[i].has_view != 0; }
                    else S.color_has_view[i] = false;
                    const TextureViewInfo& v = S.color_view[i];
                    const bool hv = S.color_has_view[i];
                    S.color_level[i] = hv && v.has_levels ? static_cast<uint32_t>(v.min_level) : 0;
                    S.color_layer[i] = hv && v.has_layers ? static_cast<uint32_t>(v.min_layer) : 0;
                    S.color_layers[i] = hv && v.has_layers ? static_cast<uint32_t>(v.num_layers) : (S.color_tex[i] ? static_cast<uint32_t>(S.color_tex[i]->layers) : 0u);
                }
                S.depth_tex = Ref<Texture>::share(p.depth.tex);
                S.depth_view = p.depth.view;
                S.depth_has_view = p.depth.has_view != 0;
                break;
            }
            case Op::DrawArrays: do_draw_arrays(r.payload<P_DrawArrays>()); break;
            case Op::DrawElements: do_draw_elements(r.payload<P_DrawElements>()); break;
            case Op::DrawIndirect: do_draw_indirect(r.payload<P_DrawIndirect>()); break;
            case Op::DrawXfb: do_draw_xfb(r.payload<P_DrawXfb>()); break;
            case Op::PushDebugGroup:
                if (device().features().debug_labels && vkCmdBeginDebugUtilsLabelEXT) {
                    VkDebugUtilsLabelEXT l = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
                    l.pLabelName = reinterpret_cast<const char*>(r.blob<P_DebugGroup>());
                    vkCmdBeginDebugUtilsLabelEXT(F->cb, &l);
                    ++g_dbg_depth;
                }
                break;
            case Op::PopDebugGroup:
                if (g_dbg_depth > 0 && vkCmdEndDebugUtilsLabelEXT) { vkCmdEndDebugUtilsLabelEXT(F->cb); --g_dbg_depth; }   // a pop whose push was in an earlier submission has nothing to close
                break;
            case Op::DrawTexture: note_unsupported("DrawTexture"); break;
            case Op::ClearColor: do_clear_color(r.payload<P_ClearColor>()); break;
            case Op::ClearDepthStencil: do_clear_depth_stencil(r.payload<P_ClearDepthStencil>()); break;
            case Op::ClearTexture: do_clear_texture(r.payload<P_ClearTexture>()); break;
            case Op::ClearBuffer: {
                const auto& p = r.payload<P_ClearBuffer>();
                PoolReasonScope pr_(kReasonCopy);
                end_pass();
                BufRef b = resolve_buffer(p.address, p.size);
                if (b.ok) vkCmdFillBuffer(F->cb, b.buffer, b.offset, p.size & ~3ull, p.value);
                if (b.ok) gpu_wrote(p.address, p.size, kWriteFill);
                memory_barrier(F->cb);
                counters().clears.fetch_add(1);
                gstamp("fill buffer");
                break;
            }
            case Op::CopyBufferToBuffer: {
                const auto& p = r.payload<P_CopyBufferToBuffer>();
                note_copy_for_cpu(p.src, p.dst, p.size);   // [nvn] async_compute: what is copied into a CPU-visible pool, the CPU reads
                PoolReasonScope pr_(kReasonCopy);
                end_pass();
                BufRef s = held_or_pool(p.src, p.size, kOutBufferRead, true), d = resolve_buffer(p.dst, p.size);
                if (s.ok && d.ok) {
                    VkBufferCopy c = {s.offset, d.offset, p.size};
                    vkCmdCopyBuffer(F->cb, s.buffer, d.buffer, 1, &c);
                    gpu_wrote(p.dst, p.size, kWriteCopy);
                }
                memory_barrier(F->cb);
                counters().copies.fetch_add(1);
                gstamp("copy buffer");
                break;
            }
            case Op::CopyBufferToTexture: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), true); break;
            case Op::CopyTextureToBuffer: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), false); break;
            case Op::CopyTextureToTexture: do_copy_texture_texture(r.payload<P_CopyTexTex>()); break;
            case Op::Downsample: do_downsample(r.payload<P_Downsample>()); break;
            case Op::UpdateUniformBuffer: {
                const auto& p = r.payload<P_UpdateUniformBuffer>();
                const uint8_t* data = r.blob<P_UpdateUniformBuffer>();
                const uint32_t n = static_cast<uint32_t>(p.size & ~3ull);
                if (n) do_write_memory(p.address + static_cast<uint64_t>(p.offset), data, n);
                break;
            }
            case Op::Dispatch: { const auto& p = r.payload<P_Dispatch>(); do_dispatch(p.x, p.y, p.z, 0); break; }
            case Op::DispatchIndirect: do_dispatch(0, 0, 0, r.payload<P_DispatchIndirect>().address); break;
            case Op::ReportCounter: {
                const auto& p = r.payload<P_Counter>();
                struct { uint64_t counter, timestamp; } rep = {1, timestamp_ns()};   // counters other than the ones handled below are reported as non-zero
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready()) {
                    end_pass();
                    if (g_q.occ_active >= 0) {
                        vkCmdEndQuery(F->cb, g_q.occ_pool, static_cast<uint32_t>(g_q.occ_active));
                        g_q.occ_last = g_q.occ_active;
                        g_q.occ_active = -1;
                    }
                    if (p.address) {
                        rep.counter = 0;
                        g_q.reports[p.address] = kReportUnknown;   // the query result lands on the GPU
                        do_write_memory(p.address, &rep, sizeof rep);
                        BufRef b = resolve_buffer(p.address, sizeof rep);
                        if (b.ok && g_q.occ_last >= 0) {
                            vkCmdCopyQueryPoolResults(F->cb, g_q.occ_pool, static_cast<uint32_t>(g_q.occ_last), 1, b.buffer, b.offset, 16,
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                            if (pool_copy_on()) gpu_wrote(p.address, 16, kWriteCopy);   // [nvn] pool_copy: the result lands in guest memory at completion
                            memory_barrier(F->cb);
                        }
                    }
                    break;
                }
                if (p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED || p.type == NVN_COUNTER_TYPE_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
                    CounterWindow& cw = g_q.cw[p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED ? 0 : 1];
                    if (cw.active >= 0) {   // the GPU counted this window (it holds indirect or feedback draws): the report is the query's result
                        end_pass();
                        cw_end_query(cw, &cw == &g_q.cw[1]);
                        cw.last = cw.active;
                        cw.active = -1;
                        if (p.address) {
                            rep.counter = 0;
                            g_q.reports[p.address] = kReportUnknown;   // the result lands on the GPU
                            do_write_memory(p.address, &rep, sizeof rep);
                            BufRef b = resolve_buffer(p.address, sizeof rep);
                            if (b.ok) {
                                // the feedback query has two results (written, needed): the second would land on the timestamp word, which is written again after it
                                vkCmdCopyQueryPoolResults(F->cb, cw.pool, static_cast<uint32_t>(cw.last), 1, b.buffer, b.offset, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                                if (pool_copy_on()) gpu_wrote(p.address, 16, kWriteCopy);   // [nvn] pool_copy: lands at completion
                                memory_barrier(F->cb);
                                if (&cw == &g_q.cw[1]) do_write_memory(p.address + 8, &rep.timestamp, 8);
                            }
                        }
                        break;
                    }
                    rep.counter = p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED ? g_q.prims_generated : g_q.prims_xfb;
                    note_report(p.address, rep.counter);
                    if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                    break;
                }
                // TIMESTAMP_TOP is a timestamp too (taken when the command reaches the top of the pipe): its counter word is 0 like TIMESTAMP's
                const bool stamp = p.type == NVN_COUNTER_TYPE_TIMESTAMP || p.type == NVN_COUNTER_TYPE_TIMESTAMP_TOP;
                if (!stamp) note_unsupported("statistics counters other than samples passed, primitives generated and feedback primitives written are reported as non-zero");
                if (stamp) rep.counter = 0;
                note_report(p.address, rep.counter);
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ReportValue: {
                const auto& p = r.payload<P_ReportValue>();
                struct { uint64_t counter, timestamp; } rep = {p.value, timestamp_ns()};
                note_report(p.address, rep.counter);
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ResetCounter: {
                const auto& p = r.payload<P_Counter>();
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready()) {
                    end_pass();
                    if (g_q.occ_active >= 0) vkCmdEndQuery(F->cb, g_q.occ_pool, static_cast<uint32_t>(g_q.occ_active));
                    const uint32_t slot = g_q.occ_next++ % kOccSlots;
                    vkCmdResetQueryPool(F->cb, g_q.occ_pool, slot, 1);
                    vkCmdBeginQuery(F->cb, g_q.occ_pool, slot, device().features().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
                    g_q.occ_active = static_cast<int32_t>(slot);
                    g_q.occ_last = -1;
                } else if (p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED || p.type == NVN_COUNTER_TYPE_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
                    const bool gen = p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED;
                    (gen ? g_q.prims_generated : g_q.prims_xfb) = 0;
                    CounterWindow& cw = g_q.cw[gen ? 0 : 1];
                    if (cw.active >= 0) { end_pass(); cw_end_query(cw, !gen); cw.active = -1; }   // an unreported window is over
                    if (counter_window_has_gpu_draw(r, p.type) && cw_ready(cw, !gen)) {
                        static bool noted = false;
                        if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: primitive counter %u counted by a GPU query (first): its window holds an indirect or feedback draw", p.type); }
                        end_pass();
                        const uint32_t slot = cw.next++ % kCounterSlots;
                        vkCmdResetQueryPool(F->cb, cw.pool, slot, 1);
                        if (gen) vkCmdBeginQuery(F->cb, cw.pool, slot, 0);
                        else vkCmdBeginQueryIndexedEXT(F->cb, cw.pool, slot, 0, 0);
                        cw.active = static_cast<int32_t>(slot);
                    }
                }
                break;
            }
            case Op::Barrier: end_pass(); memory_barrier(F->cb); break;
            case Op::FenceSync: {
                const auto& p = r.payload<P_Sync>();
                if (p.sync) { p.sync->serial.store(F->serial); p.sync->armed.store(true); }
                break;
            }
            case Op::WaitSync: break;
            case Op::SignalEvent: {
                const auto& p = r.payload<P_Event>();
                if (p.event && p.event->pool && p.event->pool->base) {
                    Event* ev = p.event;
                    uint32_t v = p.value;
                    if (p.mode != NVN_EVENT_SIGNAL_MODE_WRITE) {
                        // ADD: the GPU cannot add into memory; the sum is computed here from the value after the signals recorded so far
                        uint32_t base = 0;
                        if (ev->shadow_valid.load()) base = ev->shadow.load();
                        else std::memcpy(&base, ev->pool->base + ev->offset, 4);
                        v = base + p.value;
                    }
                    ev->shadow.store(v);
                    ev->shadow_valid.store(true);
                    do_write_memory(ev->pool->gpu_base + static_cast<uint64_t>(ev->offset), &v, 4);
                }
                break;
            }
            case Op::WaitEvent: note_unsupported("GPU-side WaitEvent"); break;
            case Op::CallCommands: {
                const auto& p = r.payload<P_CallCommands>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_CallCommands>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    Ref<CmdList> l = handle_lookup(h[i]);
                    if (l) run_list(*l, depth + 1);
                    else log_once(kOnceMissingHandle, "CallCommands of an unknown handle skipped");
                }
                break;
            }
            case Op::DiscardColor: case Op::DiscardDepthStencil: break;
            case Op::ResolveDepth: note_unsupported("ResolveDepthBuffer"); break;
            case Op::BeginXfb: case Op::EndXfb: case Op::PauseXfb: case Op::ResumeXfb: {
                const auto& p = r.payload<P_Counter>();
                if (first_time_key(kFirstXfbOp, static_cast<uint64_t>(r.op()), S.stage[kVertex] ? std::string_view(S.stage[kVertex]->hash) : std::string_view("-")))
                    NXLOG_I("nvn.xfb", "%s control %llx vs %s xfb buffer0 %llx/%llu", op_name(r.op()), static_cast<unsigned long long>(p.address),
                            S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", static_cast<unsigned long long>(S.xfb[0].addr),
                            static_cast<unsigned long long>(S.xfb[0].size));
                TRACE("%s control %llx", op_name(r.op()), static_cast<unsigned long long>(p.address));
                if (!device().features().transform_feedback) {
                    note_unsupported("transform feedback (the device has no VK_EXT_transform_feedback)");
                    break;
                }
                if (r.op() == Op::BeginXfb) {
                    xfb_end_in_pass();    // a section still open in the pass ends where the new one starts
                    auto sit = g_xfb.slot_of.find(p.address);
                    if (sit == g_xfb.slot_of.end()) {
                        if (g_xfb.slot_next >= kXfbSlots) {   // all counters in use: start over (a DrawTransformFeedback of an older control buffer then draws nothing)
                            g_xfb.slot_of.clear();
                            g_xfb.slot_next = 0;
                        }
                        sit = g_xfb.slot_of.emplace(p.address, g_xfb.slot_next++).first;
                    }
                    g_xfb.cur = sit->second;
                    g_xfb.slot_written[g_xfb.cur] = false;
                    g_xfb.active = true;
                    g_xfb.have_counter = false;
                } else if (r.op() == Op::EndXfb) {
                    xfb_end_in_pass();
                    g_xfb.active = false;
                    g_xfb.have_counter = false;
                }
                break;
            }
            default: note_unsupported(op_name(r.op())); break;
        }
    }
}

}  // namespace replay

// [display] unpaced_loading (default off): the limiter stops waiting while the game loads, since its loading code advances once per
// presented frame. Loading = presented frames with fewer than 16 draws (boot's black screen draws 3: exposure, bloom, tone map; the
// loading screen a few more) AND the game still stalling: a present gap over 100 ms in the last 1.5 s. A light screen that presents
// steadily (the title screen, a menu) is paced again within 1.5 s, so it does not run at 1000 fps; so is any frame with 16+ draws.
// Boot only: until the first frame with 16+ draws, a boot loading screen presents steadily but its loading still advances once per frame
// (Pokopia: 14 s at 60 fps before the title), so a run of light frames stays unpaced for up to [display] unpaced_loading_s (20, 0 = off)
// seconds even without stalls. Once the game has drawn a real scene the stall rule alone applies, so a light screen in the world (a
// menu, a fade) is never unpaced just for being light, and a frame with 16+ draws is always paced.
// The detection always runs: its result (`want`) is also the loading gate of the compile paths (replay_loading, [nvn] gpl_gate = state).
// Only the limiter action depends on [display] unpaced_loading.
// Off by default: a loading screen's animations also advance once per presented frame, so they fast-forwarded above 60 fps, and an
// unpaced frame skips the limiter's 60 fps ceiling (the event hold, #144 #297) too. The gain was small: under a second on a world load (#48).
static bool g_loading = true;      // the detector's current `want`; loading until the first present (render thread)
bool replay_loading() { return g_loading; }

void note_frame_for_pacing() {
    static const bool on = nxrt::config_get_bool("display", "unpaced_loading", false);
    using clk = std::chrono::steady_clock;
    static uint64_t last_p = 0, last_d = 0;
    static int low = 0;
    static bool full = false;
    static clk::time_point last_present{}, last_stall{}, low_since{};
    const uint64_t p = counters().presents.load(std::memory_order_relaxed);
    if (p == last_p) return;
    const auto now = clk::now();
    const uint64_t d = counters().draws.load(std::memory_order_relaxed) + counters().draws_skipped.load(std::memory_order_relaxed);
    const uint64_t per = (d - last_d) / (p - last_p);
    last_p = p;
    last_d = d;
    if (last_present == clk::time_point{} || now - last_present > std::chrono::milliseconds(100)) last_stall = now;
    last_present = now;
    low = per < 16 ? low + 1 : 0;
    if (low == 1) low_since = now;
    static const double boot_s = std::max(0.0, nxrt::config_get_double("display", "unpaced_loading_s", 20.0));
    const bool boot_run = !full && now - low_since < std::chrono::duration<double>(boot_s);
    const bool want = low >= 3 && (now - last_stall < std::chrono::milliseconds(1500) || boot_run);
    if (want != g_loading) {
        g_loading = want;
        ft::mark(want ? "pace.loading" : "pace.playing", static_cast<uint32_t>(per));
        if (!on)
            NXLOG_I(kTag, "%s (present %llu, %llu draws; pacing unchanged: [display] unpaced_loading is false)", want ? "loading" : "playing",
                    static_cast<unsigned long long>(p), static_cast<unsigned long long>(per));
    }
    if (on && want != nxrt::frame_limiter_unpaced()) {
        nxrt::frame_limiter_set_unpaced(want);
        NXLOG_I(kTag, "pacing %s (present %llu, %llu draws)", want ? "off: loading" : "on", static_cast<unsigned long long>(p),
                static_cast<unsigned long long>(per));
    }
    if (!full && per >= 16) {
        full = true;
        NXLOG_I(kTag, "first frame with 16+ draws (present %llu, %llu draws)", static_cast<unsigned long long>(p), static_cast<unsigned long long>(per));
    }
}

void replay_begin_submit(Frame& f) {
    static const bool fine_init = (g_rp.fine = nxrt::config_get_bool("prof", "replay_fine", false), true);
    (void)fine_init;
    if (!g_tsr.resolved) tsr_resolve();   // the game's camera block (temporal upscaling), once
    rp_flush();
    rp_bump(ft::kCntEpSubmit);    // the sets of the last submission are gone with its descriptor pools
    ft::split_count(ft::kCntSubmissions);
    g_rp.t0 = ft::on() ? ft::now_ns() : 0;
    RpScope rp_begin(kRpBegin);
    gpl_note_presents(counters().presents.load(std::memory_order_relaxed));   // [nvn] gpl_gate = time: frames flowing = play
    note_frame_for_pacing();
    gpl_latch_play(!replay_loading());   // [nvn] gpl_gate = state: play vs loading for the whole submission (a stall inside it cannot flip it)
    F = &f;
    { RpScope rp_(kRpTexPoll); ft::SplitScope sp_(ft::kSplSetup); tex_poll_cpu_writes(); }   // per call: see setup_coalesce_on (nvn_gpu_texture.cpp)
    if (extras::LowLatency* ll = extras::lowlat()) ll->submit_begin();     // RENDERSUBMIT_START of the frame (S14)
    trace_begin_submit();
    {   // S38: device-local pool copies and tracked barriers. [nvn] gpuperf_ab = N: both are off in even windows of N presents and on in odd
        // ones (the paired-window measurement of S29); without it they follow their own settings.
        static const int64_t ab = config_get_int("nvn", "gpuperf_ab", 0);
        static const bool tracked = config_get_str("nvn", "pass_barriers", "tracked") != "full";
        static int last_on = -1;
        bool on = true;
        if (ab > 0) on = ((counters().presents.load() / static_cast<uint64_t>(ab)) & 1) != 0;
        if (static_cast<int>(on) != last_on) {
            last_on = on ? 1 : 0;
            if (ab > 0) ft::mark(on ? "gpuperf.on" : "gpuperf.off", static_cast<uint32_t>(counters().presents.load()));
        }
        g_bar.begin_submit(on && tracked);
        RpScope rp_(kRpMirror);
        ft::SplitScope sp_(ft::kSplSetup);
        mirror_begin_submit(f, on);
    }
    S.window_origin = window_origin_mode();
    S.depth_mode = depth_mode();
    P = Pass();
    g_cmd.begin_submit();
    g_copies.begin_submit();
    g_sets.begin_submit(f);
    g_piece = PieceState();
    g_piece.budget = split_budget_ticks();
    if (g_piece.budget && !g_trace && !g_trace_hooks) {
        g_piece.armed = true;
        g_piece.left = split_max_pieces() - 1;
        g_piece.t0 = ft::tsc();
    }
}

void replay_lists(Frame& f, const std::vector<Ref<CmdList>>& lists) {
    F = &f;
    for (const Ref<CmdList>& l : lists)
        if (l) run_list(*l, 0);
}

void replay_end_submit(Frame& f) {
    const uint64_t rp_te = ft::on() ? ft::tsc() : 0;
    F = &f;
    if (out_any()) {         // every compute result is in game memory, or held and guarded, before the submission's serial signals
        end_pass();
        RpScope rp_(kRpOutEnd);
        ft::SplitScope sp_(ft::kSplSetup);
        out_end_submit(F->cb, F->serial);
    }
    end_pass();
    g_copies.end_submit(f.serial);
    g_q.end_submit();
    for (; g_dbg_depth > 0; --g_dbg_depth) vkCmdEndDebugUtilsLabelEXT(f.cb);   // labels cannot outlive their command buffer either
    if (rp_te) { g_rp.ns[kRpEnd] += ft::tsc() - rp_te; ++g_rp.n[kRpEnd]; }
    rp_flush();
    F = nullptr;
}

void replay_shutdown() {
    g_xfb.shutdown();
    g_q.shutdown();
    g_dbg_depth = 0;
    g_gw.reset();
    g_mask.shutdown();
    g_mask_ready = false;
    if (g_qa.pipe) vkDestroyPipeline(vkdev(), g_qa.pipe, nullptr);
    if (g_qa.layout) vkDestroyPipelineLayout(vkdev(), g_qa.layout, nullptr);
    if (g_qa.set_layout) vkDestroyDescriptorSetLayout(vkdev(), g_qa.set_layout, nullptr);
    if (g_qa.index) device().destroy_buffer(g_qa.index);
    g_qa = QuadArgs();
    pipeline_shutdown();
    S = GState();
    ++g_rc.pools;   // the pools are no longer held: their addresses may come back as other pools
    P = Pass();
    g_bar.pending.clear();
    out_reset();
    g_sets.cache.clear();
    g_copies.shutdown();
}

// Gauge (nxrt/gauge.h): reads the atomic mirror above, never the map. Removed before the globals are destroyed.
namespace {
struct ReplayGauges {
    ReplayGauges() {
        nxrt::gauge_register("nvn.replay.fb_clones", [] { return g_copies.fb_clone_n.load(std::memory_order_relaxed); });
    }
    ~ReplayGauges() { nxrt::gauge_unregister("nvn.replay.fb_clones"); }
};
ReplayGauges g_replay_gauges;
}  // namespace

}  // namespace nxrt::nvn::be::gpu
