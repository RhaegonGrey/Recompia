// SPDX-License-Identifier: GPL-3.0-or-later
// Internal header of the command list interpreter (nvn_replay*.cpp, render thread only): the replay state and helpers its files
// share. Every replay file includes this header and nothing after it: the counting macros below must apply to the replay code only.
#pragma once
#include <atomic>
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <string_view>
#include <unordered_set>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/log.h"
#include "nvn_decoded.h"
#include "nvn_driver_bank.h"
#include "nvn_enums.h"
#include "nvn_fnv.h"
#include "nvn_format.h"
#include "nvn_mask_clear.h"
#include "nvn_gpu_internal.h"
#include "nvn_tess_gen.h"
#include "nxrt/lowlat.h"
#include "extras/hdr_nvn.h"
#include "extras/fsr1_spatial.h"
#include "extras/spatial_upscaler.h"
#include "extras/temporal.h"
#include "nvn_key_check.h"
#include "nvn_pipeline.h"
#include "nvn_shaders.h"

// [prof] render_split: the Vulkan commands of the replay are counted by kind (binds, draws, dynamic state, passes) with one branch when off.
// The macros call the real function (a macro name is not expanded again inside its own expansion).
#define vkCmdBindPipeline(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdBind), vkCmdBindPipeline(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdBind), vkCmdBindDescriptorSets(__VA_ARGS__))
#define vkCmdBindVertexBuffers2(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdBind), vkCmdBindVertexBuffers2(__VA_ARGS__))
#define vkCmdBindIndexBuffer(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdBind), vkCmdBindIndexBuffer(__VA_ARGS__))
#define vkCmdDraw(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndirectByteCountEXT(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDraw), vkCmdDrawIndirectByteCountEXT(__VA_ARGS__))
#define vkCmdSetViewportWithCount(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetViewportWithCount(__VA_ARGS__))
#define vkCmdSetScissorWithCount(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetScissorWithCount(__VA_ARGS__))
#define vkCmdSetStencilWriteMask(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetStencilWriteMask(__VA_ARGS__))
#define vkCmdSetStencilTestEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetStencilTestEnable(__VA_ARGS__))
#define vkCmdSetStencilReference(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetStencilReference(__VA_ARGS__))
#define vkCmdSetStencilOp(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetStencilOp(__VA_ARGS__))
#define vkCmdSetStencilCompareMask(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetStencilCompareMask(__VA_ARGS__))
#define vkCmdSetRasterizerDiscardEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetRasterizerDiscardEnable(__VA_ARGS__))
#define vkCmdSetPrimitiveTopology(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetPrimitiveTopology(__VA_ARGS__))
#define vkCmdSetPrimitiveRestartEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetPrimitiveRestartEnable(__VA_ARGS__))
#define vkCmdSetLineWidth(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetLineWidth(__VA_ARGS__))
#define vkCmdSetFrontFace(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetFrontFace(__VA_ARGS__))
#define vkCmdSetDepthWriteEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthWriteEnable(__VA_ARGS__))
#define vkCmdSetDepthTestEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthTestEnable(__VA_ARGS__))
#define vkCmdSetDepthCompareOp(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthCompareOp(__VA_ARGS__))
#define vkCmdSetDepthBoundsTestEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthBoundsTestEnable(__VA_ARGS__))
#define vkCmdSetDepthBounds(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthBounds(__VA_ARGS__))
#define vkCmdSetDepthBiasEnable(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthBiasEnable(__VA_ARGS__))
#define vkCmdSetDepthBias(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetDepthBias(__VA_ARGS__))
#define vkCmdSetCullMode(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetCullMode(__VA_ARGS__))
#define vkCmdSetBlendConstants(...) (::nxrt::ft::split_count(::nxrt::ft::kCntCmdDyn), vkCmdSetBlendConstants(__VA_ARGS__))
#define vkCmdBeginRendering(...) (::nxrt::ft::split_count(::nxrt::ft::kCntPasses), vkCmdBeginRendering(__VA_ARGS__))

namespace nxrt::nvn::be::gpu {
// The replay's own names, shared by its files (they were one anonymous namespace; `using namespace replay` below keeps the lookup
// the same). Their definitions are in nvn_replay*.cpp; a name only one file uses is static there.
namespace replay {

constexpr const char* kTag = "nvn.replay";

enum Stage { kVertex = 0, kFragment = 1, kGeometry = 2, kTessCtl = 3, kTessEval = 4, kCompute = 5 };

struct GState {
    const ShaderInfo* stage[6] = {};
    struct Bind { uint64_t addr = 0, size = 0; };
    Bind vtx[16], ubo[6][16], ssbo[6][16], xfb[4];
    uint64_t tex[6][64] = {};
    uint64_t img[6][16] = {};
    Ref<TexturePool> tpool;
    Ref<SamplerPool> spool;
    VertexAttribInfo attr[16];
    uint32_t attr_count = 0;
    VertexStreamInfo stream[16];
    uint32_t stream_count = 0;
    BlendInfo blend[8];
    ColorInfo color;
    ChannelMaskInfo chmask;
    DepthStencilInfo ds;
    PolygonInfo poly;
    MultisampleInfo ms;
    float vp[16][4] = {};
    uint32_t nvp = 0;
    int32_t sc[16][4] = {};
    uint32_t nsc = 0;
    // near/far per viewport; NVN's default range is 0..1. A game-set 0..0 is a real range (the placement indicator draws
    // its outline and fill with it to pin them to depth 0, issue #120), so it is passed through, never widened.
    float dr[16][2] = {{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1},
                       {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}};
    bool depth_bounds = false;
    float db_near = 0, db_far = 1;
    uint32_t stencil_ref[2] = {0, 0}, stencil_vmask[2] = {0xff, 0xff}, stencil_wmask[2] = {0xff, 0xff};
    float blend_color[4] = {};
    float line_width = 1, point_size = 1, alpha_ref = 0;
    float bias_factor = 0, bias_units = 0, bias_clamp = 0;
    uint32_t sample_mask = 0xffffffff;
    bool rasterizer_discard = false, depth_clamp = false, prim_restart = false, render_enable = true;
    uint32_t restart_index = 0;
    // tessellation: nvnCommandBufferSetPatchSize (control points per patch; GL default 3) and the fixed levels of
    // nvnCommandBufferSetInner/OuterTessellationLevels (used by NVN only when the program has no control shader)
    uint32_t patch_size = 3;
    float tess_outer[4] = {1, 1, 1, 1}, tess_inner[2] = {1, 1};
    // render targets
    uint32_t ncolor = 0;
    Ref<Texture> color_tex[8];
    TextureViewInfo color_view[8];
    bool color_has_view[8] = {};
    uint32_t color_level[8] = {}, color_layer[8] = {}, color_layers[8] = {};   // level, first layer, layer count each colour target is attached with (trace)
    Ref<Texture> depth_tex;
    TextureViewInfo depth_view;
    bool depth_has_view = false;
    uint64_t targets_gen = 1;   // raised by every SetRenderTargets: the same count means the same targets and views
    int window_origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
    int depth_mode = NVN_DEPTH_MODE_NEAR_IS_MINUS_W;
};
extern GState S;

struct Target {
    Texture* tex = nullptr;
    uint32_t level = 0, layer = 0, layers = 1;
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    bool operator==(const Target& o) const { return tex == o.tex && level == o.level && layer == o.layer && layers == o.layers && view_format == o.view_format; }
};

struct Pass {
    bool open = false;
    uint32_t ncolor = 0;
    Target color[8];
    Target depth;
    VkExtent2D extent = {0, 0};
    VkFormat color_fmt[8] = {};
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    bool has_stencil = false;
    bool depth_ro = false;      // the depth attachment is in DEPTH_STENCIL_READ_ONLY_OPTIMAL (it is also sampled by the draws of this pass)
    uint32_t samples = 1;
    float scale = 1.0f;         // resolution scale of the attachments ([gfx] render_scale): viewports and scissors are multiplied by it
    uint8_t fbl = 0;            // colour slots in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT (their draws sample them: feedback_prepare)
    uint64_t targets_gen = 0;   // S.targets_gen the pass's targets were last found to be (ensure_pass)
    uint32_t draws = 0;         // draws recorded in the pass (GPU pass profiler)
    uint64_t verts = 0;         // vertices (indices for indexed draws) times instances of those draws
};
extern Pass P;   // owner: nvn_replay_pass.cpp (ensure_pass, end_pass)
// [nvn] pass_barriers = tracked (S38). A pass whose draws can only have written its attachments (no storage buffers or images, no stream-out)
// does not end with a barrier: its written attachments are remembered here, and the next command that could depend on them gets one. A
// layout change of such an image is that barrier for the image (narrow masks, set_narrow_barriers); a later pass that renders to it again in
// the same layout, and every command other than a pass or a layout change, get a full barrier first (end_pass).
struct PendingWrite { Ref<Texture> tex; VkImageLayout layout; };
struct PassBarriers {   // owner: nvn_replay_pass.cpp
    std::vector<PendingWrite> pending;
    bool tracked = false;       // this submission uses tracked barriers
    bool pass_unsafe = false;   // a draw of the open pass may write memory other than its attachments
    void begin_submit(bool tracked_on);
};
extern PassBarriers g_bar;
extern Frame* F;
// Descriptor sets and what they bind besides the game's resources (nvn_replay_bind.cpp). Most of it lasts one submission (begin_submit):
// the sets and upload allocations die with the submission's descriptor pools and upload ring.
struct SetState {   // owner: nvn_replay_bind.cpp
    std::unordered_map<uint64_t, VkDescriptorSet> cache;           // this submission's sets, by content
    UploadAlloc support;                                           // this submission's support buffer (render scales 1.0)
    UploadAlloc draw_support;                                      // the one the next sets bind: support, or one with render scales (support_for_draw)
    std::unordered_map<uint64_t, UploadAlloc> support_scaled;      // this submission's support buffers with render scales, by content
    std::unordered_map<uint64_t, UploadAlloc> driver_banks;        // this submission's filled c[0] banks (driver_bank_for), by content
    uint32_t draw_base_instance = 0;   // the base instance of the draw being set up, for graphics c[0] + 0x644 (0 for indirect draws)
    uint32_t bank_base_instance = 0;   // the one the bound descriptor sets hold
    // [nvn] set_reuse: bumped (rp_bump) by whatever in the replay may change what a texture binding resolves to (a pass opens or ends,
    // other render targets, a feedback or 3D copy, a GPU write, a new submission). With be::g_bind_gen (pools, texture images) it keys the
    // per-set memo of build_set. Never reset.
    uint64_t epoch = 1;
    void begin_submit(Frame& f);
};
extern SetState g_sets;
extern MaskClear g_mask;
extern bool g_mask_ready, g_mask_ok;
inline void rp_bump(ft::SplitCount src = ft::kCntCounts) { ++g_sets.epoch; ft::split_count(ft::kCntEpoch); if (src != ft::kCntCounts) ft::split_count(src); }

// Replay phase profile (frame trace only, [prof] trace): time and count per phase of the draw / dispatch setup, summed over a
// submission and written as one event per phase at its end (t0 = submission start, duration = the sum, arg = the count), so the
// per-thread zone totals of ftrace_report.py split the "replay" zone without one event per draw (thousands a frame overflow the
// event array). Off: one relaxed load per site.
enum RpPhase { kRpDraw, kRpSets, kRpSetBuild, kRpSetWrite, kRpSetBind, kRpPrep, kRpPass, kRpPipe, kRpDyn, kRpVb, kRpDispatch, kRpSet0, kRpSet1, kRpSet2, kRpSet3, kRpMiss0, kRpMiss1, kRpMiss2, kRpMiss3, kRpBuf, kRpTex, kRpTexel, kRpSame0, kRpSame1, kRpSame2, kRpSame3, kRpReuse, kRpBegin, kRpEnd, kRpOpIn, kRpOp, kRpTail, kRpTexPoll, kRpMirror, kRpOutEnd, kRpCount };
struct RpProfile {   // owner: nvn_replay.cpp (rp_flush at each submission's start and end)
    uint64_t ns[kRpCount] = {};   // ticks until rp_flush
    uint32_t n[kRpCount] = {};
    uint64_t t0 = 0;              // the submission's start (ns), 0 while the frame trace is off
    uint64_t inner = 0;           // ticks of rp.draw / rp.dispatch scopes and finished ops, so an op's own time can be told from them
    bool fine = false;            // [prof] replay_fine
};
extern RpProfile g_rp;
struct RpScope {
    int ph;
    uint64_t t0;
    explicit RpScope(int p, bool en = true) : ph(p), t0(en && ft::on() ? ft::tsc() : 0) {}
    ~RpScope() {
        if (!t0) return;
        const uint64_t dt = ft::tsc() - t0;
        g_rp.ns[ph] += dt;
        ++g_rp.n[ph];
        if (ph == kRpDraw || ph == kRpDispatch) g_rp.inner += dt;
    }
};
inline void rp_count(int ph) { if (ft::on()) ++g_rp.n[ph]; }   // a count without time (cache hits)

// What changed since the last draw. Ops set bits (mark_dirty); a draw handles and clears the groups it needs. A new submission, a
// new pass and anything that binds other pipelines sets them all (a new command buffer has no dynamic state, a new pass has other
// formats and extents).
enum : uint32_t {
    kPipe = 1, kDesc = 2, kVb = 4, kDynVp = 8, kDynRaster = 16, kDynDepth = 32, kDynStencil = 64, kDynBias = 128, kDynBlend = 256, kAllDirty = 0x1ff
};

// [nvn] bind_coalesce: fewer Vulkan calls per draw. Vertex buffers, descriptor sets and the index buffer are bound in ranges / only when they
// changed, and dynamic state is sent only when its value differs from the last one sent in this command buffer (the shadow). Vulkan keeps
// dynamic state across vkCmdBeginRendering inside one command buffer, and binding a pipeline that has the state dynamic does not reset it;
// the shadow is invalidated where a command buffer starts and where other code records graphics work into it (clears drawn with a mask,
// upscalers, SDK compute).
inline bool bind_coalesce_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "bind_coalesce", true);
        if (v) NXLOG_I("nvn.replay", "nvn: bind_coalesce on");
        return v;
    }();
    return on;
}
template <class T> struct Sh {
    T v{};
    bool ok = false;
    // true: the value differs (or is unknown) and was recorded as sent
    bool changed(const T& n) {
        if (ok && std::memcmp(&v, &n, sizeof(T)) == 0) return false;
        v = n;
        ok = true;
        return true;
    }
};
struct DynShadow {
    Sh<std::array<VkViewport, 16>> vp; uint32_t nvp = 0;
    Sh<std::array<VkRect2D, 16>> sc; uint32_t nsc = 0;
    Sh<uint32_t> cull, front, discard, depth_test, depth_write, depth_cmp, depth_bounds_test, stencil_test, bias_enable;
    Sh<float> line_width;
    Sh<std::array<float, 2>> depth_bounds;
    Sh<std::array<uint32_t, 5>> stencil_op[2];
    Sh<uint32_t> stencil_ref[2], stencil_cmask[2], stencil_wmask[2];
    Sh<std::array<float, 3>> bias;
    Sh<std::array<float, 4>> blend;
};
struct VbShadow { VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0, size = 0, stride = 0; bool ok = false; };
struct IbShadow { VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0; VkIndexType type = VK_INDEX_TYPE_NONE_KHR; bool ok = false; };

// What the replay has recorded into the current command buffer and the draw state derived from it: the dirty bits, the bound pipeline
// and program layout, dynamic topology and restart, the bind shadows. begin_submit resets what a new command buffer does not have.
struct CmdState {   // owner: nvn_replay_draw.cpp (begin_draw); the other ops set dirty bits and drop the bound pipeline
    uint32_t dirty = kAllDirty;
    bool mirror_stale = false;   // a GPU write made pages of a pool copy stale while a draw was being set up: bind again at the next draw
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    const ProgLayout* pl = nullptr;   // the bound program's layout (looked up on kPipe)
    VkPipeline pipe = VK_NULL_HANDLE;   // the graphics pipeline of the last draw set up
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    bool restart = false, restart_valid = false;
    uint8_t key_dummy_binding = 0;   // 1 + the binding that feeds the defaults of unset vertex attributes (0: none)
    bool want_depth_ro = false;      // some texture this draw samples is the current depth attachment
    bool vb_bound_in_cb = false;     // vkCmdBindVertexBuffers2 was called in this command buffer (dynamic strides need one)
    bool prog_writes = false;        // the bound program has storage buffer or storage image bindings
    int8_t fs_tonemap = -1;          // the bound fragment shader is the tone-map shader (extras::hdr_is_tonemap); -1: not asked since kPipe
    DynShadow dsh;                   // bind_coalesce: the dynamic state, vertex and index buffers last sent in this command buffer
    VbShadow vbsh[16];
    IbShadow ibsh;
    void begin_submit();
};
extern CmdState g_cmd;
inline void shadow_invalidate() {
    g_cmd.dsh = DynShadow();
    for (VbShadow& v : g_cmd.vbsh) v = VbShadow();
    g_cmd.ibsh = IbShadow();
}
// A command may write [addr, addr + size) of pool memory: device-local copies of those pages are stale, and bindings that point at them are
// made again (vertex buffers and descriptor sets).
// The ranges of pool memory the GPU writes, with the serial of the submission that did (a ring). The replay records commands: a range written
// by a submission that has not completed yet does not hold its result in game memory, so the CPU must not read it for a decision that needs
// the data (the arguments of an indirect draw it converts, cpu_view). A range that fell out of the ring while its submission was still running
// makes every range unreadable until that submission is done.
struct GpuWrite { uint64_t lo, hi, serial; };
constexpr size_t kGpuWriteRing = 1024;
struct GpuWrites {   // owner: nvn_replay.cpp (gpu_wrote); kept across submissions, emptied by replay_shutdown
    GpuWrite ring[kGpuWriteRing] = {};
    size_t next = 0, count = 0;
    uint64_t lost_serial = 0;   // the newest serial among ranges dropped from the ring
    void reset() { next = count = 0; lost_serial = 0; }
};
extern GpuWrites g_gw;
// True when no submission still in flight (the one being recorded included) may write [addr, addr + size).
inline bool gw_settled(uint64_t addr, uint64_t size) {
    const uint64_t done = poll_completed();
    if (g_gw.lost_serial > done) return false;
    for (size_t i = 0; i < g_gw.count; ++i)
        if (g_gw.ring[i].serial > done && addr < g_gw.ring[i].hi && g_gw.ring[i].lo < addr + size) return false;
    return true;
}
void gpu_wrote(uint64_t addr, uint64_t size, int use);
// Transform feedback (stream-out). One byte counter per feedback control buffer (NVN's Begin/End/Draw take its address): slot * 16 bytes
// into `counter`. vkCmdEndTransformFeedbackEXT writes the bytes of the section there; DrawTransformFeedback reads them
// (vkCmdDrawIndirectByteCountEXT). slot_written: the slot holds a count. Kept across submissions; shutdown() releases the counter buffer.
constexpr uint32_t kXfbSlots = 256, kXfbSlotBytes = 16;
struct XfbState {   // owner: nvn_replay_draw.cpp; sections begun and ended by run_list (Begin/EndXfb) and xfb_end_in_pass
    bool active = false;         // a section is open (BeginXfb .. EndXfb)
    bool began = false;          // vkCmdBeginTransformFeedbackEXT is in effect in the open pass
    bool have_counter = false;   // a counter buffer holds the bytes written so far: the next begin continues after them
    gfx::Buffer counter;
    std::unordered_map<uint64_t, uint32_t> slot_of;
    uint32_t slot_next = 0, cur = 0;
    bool slot_written[kXfbSlots] = {};
    void shutdown();
};
extern XfbState g_xfb;
// ---- counters and reports (nvn_replay_query.cpp) ---------------------------------------------------------------------------------
// Occlusion counter (NVN_COUNTER_TYPE_SAMPLES_PASSED): ResetCounter starts a Vulkan occlusion query outside any pass; it stays active across
// the passes that follow (the console counts continuously); ReportCounter ends it and copies its 64-bit result into the report.
constexpr uint32_t kOccSlots = 256;
// Windowed GPU counters for the primitive counters (13 = primitives generated, 14 = feedback primitives written): a ResetCounter whose window
// (up to the matching ReportCounter, in the same list) holds an indirect or feedback draw starts a query the GPU counts those draws with;
// the CPU counts the other draws exactly (count_primitives) but cannot know the primitives of a draw whose arguments the GPU holds.
// cw[0] pipeline statistics (input assembly primitives; a converted quad counts as its two triangles), cw[1] transform feedback stream 0 query.
struct CounterWindow { VkQueryPool pool = VK_NULL_HANDLE; bool tried = false; uint32_t next = 0; int32_t active = -1, last = -1; };
constexpr uint32_t kCounterSlots = 64;
// Conditional render (SetRenderEnableConditional): the console compares the 64-bit counter words of two reports, at the address and
// 16 bytes after it (yuzu Maxwell3D::ProcessQueryCondition: IfEqual / IfNotEqual), when the command runs. The replay runs in GPU
// order, so the counters it writes itself (ReportCounter other than samples passed, ReportValue) are known here: `reports` keeps
// them by address. kReportUnknown marks an address the GPU writes later (samples passed): a condition on it renders, as before. An
// address the replay never wrote (the game's own memory) is read from the pool.
constexpr uint64_t kReportUnknown = ~0ull;
struct QueryState {   // owner: nvn_replay_query.cpp; the ops are in run_list (ReportCounter, ResetCounter, ReportValue)
    VkQueryPool occ_pool = VK_NULL_HANDLE;
    bool occ_tried = false;
    uint32_t occ_next = 0;
    int32_t occ_active = -1, occ_last = -1;
    CounterWindow cw[2];
    // Primitive counters (NVN_COUNTER_TYPE_PRIMITIVES_GENERATED = 13, TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN = 14): counted on the CPU by every
    // draw (count_primitives), process-wide like the console's, zeroed by ResetCounter of their type, dropped at shutdown.
    uint64_t prims_generated = 0, prims_xfb = 0;
    std::unordered_map<uint64_t, uint64_t> reports;
    void end_submit();   // queries cannot outlive their command buffer (F): the counts of unreported counters are dropped
    void shutdown();     // the query pools are released
};
extern QueryState g_q;
bool occ_ready();
extern nxrt::GaugeCounter g_gauge_pipeline_binds;
// ---- frame trace and debug aids (nvn_replay_trace.cpp) ---------------------------------------------------------------------------
// Their state is private to the trace module. The draw and dispatch paths test one flag per site (both false in play) and call in.
extern bool g_trace;         // this submission is traced ([nvn] trace_frame, trace_after_fs, trace_sec, a script request)
extern bool g_trace_hooks;   // a per-draw debug aid is armed for this submission: [nvn] skip_fs_file, or a trace_after_fs trigger still waiting
#define TRACE(...) do { if (g_trace) NXLOG_I("nvn.trace", __VA_ARGS__); } while (0)
void trace_begin_submit();                           // sets g_trace and g_trace_hooks for the submission
bool trace_skip_draw();                              // g_trace_hooks: counts the draw; true: [nvn] skip_fs_file drops it
void trace_note_fs();                                // g_trace_hooks: the trace_after_fs trigger sees the draw's fragment shader
// The rest want g_trace.
void trace_dump_raw(const ProgLayout& pl);           // [nvn] trace_dump_tex: level 0 of the sampled textures as the GPU holds it (ends the pass)
void trace_draw(int mode, bool indexed, const ProgLayout& pl);
void trace_indexed(const P_DrawElements& p);
void trace_bind_program(const P_BindProgram& p);
void trace_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect, const ShaderInfo* cs);
void trace_dispatch_bindings(const ProgLayout& pl);
void trace_snap_after_draw();                        // [nvn] trace_snap
const char* tex_desc(const Texture* t, char* buf, size_t n);
// The warnings log_once writes once per run: one flag each.
enum LogOnce : uint32_t {
    kOnceCallDepth, kOnceMissingHandle, kOnceShadowColour, kOnceColorFeedback, kOnceStorageImageDummy, kOnceOutTexel, kOnceTexelBufferDummy,
    kOnceAttrTypeMismatch, kOnceAlphaTest, kOnceDrawNoVs, kOnceDrawNoTarget, kOnceDownsampleScale, kOnceCount
};
void log_once(LogOnce key, const char* msg);
// Warnings written once per (kind, texture id, shader hash, binding name). True the first time a key is seen, up to 4096 keys. The key is
// hashed in place: a call for a key already seen allocates nothing (these run per descriptor set build).
enum FirstKey : uint32_t { kFirstDummyView, kFirstStorageView, kFirstTexelNonBuf, kFirstTexelBuffer, kFirstVol3d, kFirstImageSize, kFirstXfbOp, kFirstXfbDraw };
bool first_time_key(FirstKey kind, uint64_t id, std::string_view shader = {}, std::string_view binding = {});

inline VkImageViewType view_type_for(const DescriptorBinding& b) {
    const ImageShape& s = b.image;
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 1: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}
void xfb_end_in_pass();

// GPU pass profiler: closes the interval since the previous stamp under this label (no-op unless [prof] gpu_passes).
inline void gstamp(const char* label) {
    if (gpass_on()) gpass_stamp(*F, label, 0);
}
void end_pass(bool to_pass = false);
float attach_targets(bool fix);
bool ensure_pass(bool depth_ro = false, uint8_t fbl = 0);
VkRect2D scissor_rect(const int32_t* r);

// ---- descriptor sets ---------------------------------------------------------------------------------------------------------
struct TexResolved {
    Texture* tex = nullptr;
    TextureViewInfo view;
    bool has_view = false;
    SamplerInfo smp;
    bool has_sampler = false;
};

inline uint32_t read_handle_word(int stage, int bank, int word) {
    // a texture handle the game wrote into uniform block (bank - 3)
    const int ub = bank - 3;
    if (ub < 0 || ub >= 16) return 0;
    const GState::Bind& b = S.ubo[stage][ub];
    if (!b.addr || static_cast<uint64_t>(word) * 4 + 4 > b.size) return 0;
    uint32_t v = 0;
    std::memcpy(&v, host_ptr(b.addr) + static_cast<size_t>(word) * 4, 4);
    return v;
}

// [nvn] resolve_once (default on): a texture handle is resolved (pool entries -> texture, view, sampler) once and every later binding
// of it takes that result. prepare_textures, vol3d_prepare, samples_depth_target, feedback_prepare, scene_upscale_for_draw,
// sampled_descriptor and support_for_draw call resolve_texture each (2 shared locks and a view + sampler copy: up to 5 per binding per
// draw), and most draws bind handles an earlier draw bound.
// What resolve_texture returns is a function of: the handle (S.tex of the stage, or the word in the uniform block's memory), the bound
// texture and sampler pools (S.tpool / S.spool) and their entries. Entries change only in texture_pool_register / sampler_pool_store,
// which bump be::g_bind_gen under the pool's lock after the write; texture images remade or released (tex_rescale,
// tex_enable_feedback_loop, release_texture_gpu, re-init) bump it too, though they keep the Texture* and the entry. The feedback remap
// (g_copies.fb_use), the 3D copies (g_copies.vol3d_use), passes and render targets are applied by the callers AFTER resolve_texture, so they are not
// inputs. Invariant: a slot is used only while the handle read again now, g_bind_gen (read before the resolve, so a pool write during
// it re-resolves at the next call) and the pool binding are what they were when it was filled. The pool binding is `pools`, raised
// whenever S.tpool or S.spool is set to another object (run_list) or dropped (replay_shutdown); S holds a reference to both pools, so
// neither can be freed and its address reused while the count stands. A slot's Texture* is kept alive by its pool entry.
struct ResolveSlot {
    uint64_t handle = 0, gen = 0, pools = 0;
    TexResolved r;
};
struct ResolveCache {   // owner: nvn_replay_textures.cpp (resolve_texture)
    static constexpr size_t kSlots = 512;   // direct mapped by handle
    ResolveSlot slot[kSlots];
    uint64_t pools = 1;                     // the pool binding (no slot holds 0)
};
extern ResolveCache g_rc;
bool resolve_once_on();
TexResolved resolve_texture(const BindingDesc& d);
ViewRequest make_request(const DescriptorBinding& b, const TexResolved& r);
TexResolved resolve_image(const BindingDesc& d);

// True when two texture objects are the same image: the object itself, or another object over the same storage (S4f: objects over one piece of
// memory share one image, so the one the pass renders to and the one a shader samples can be two objects).
inline bool shares_image(Texture* a, const Texture* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (!tex_gpu(a)) return false;
    return a->gpu != nullptr && a->gpu == b->gpu;
}
bool samples_depth_target(const ProgLayout& pl);

// A colour attachment cannot be sampled in the pass that renders to it (no feedback loops): the binding gets the dummy view.
inline bool is_color_feedback(Texture* t) {
    if (!t) return false;
    for (uint32_t i = 0; i < S.ncolor; ++i) {
        Texture* a = S.color_tex[i].get();
        if (a && !a->gpu) tex_gpu_attachment(a);   // an attachment gets its image (or the one it shares) here, not later in ensure_pass: the check runs before the pass begins (S4g)
        if (shares_image(t, a)) return true;
    }
    return false;
}

// A draw that samples its own colour attachment (the bloom composite of some frames reads the tone-mapped picture while it writes it back, one
// pixel at a time; the bloom mip chain renders one level of a texture while it samples the others through a view of all levels): the console
// allows that, plain Vulkan has no such loop. Two ways, per draw:
// - feedback loop ([nvn] feedback_loop, VK_EXT_attachment_feedback_loop_layout): the image (remade once with the feedback loop usage) is
//   attachment and sampled texture at once in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, with a pipeline made with the feedback loop flag. The pass
//   before the draw is ended (a barrier: the draw reads what the draws before it wrote) and the draw's pass begins in that layout. No copy. A
//   pointwise read sees the pixel before the draw writes it, as on the console.
// - copy: the draw samples a copy of the image made just before it, which holds the same values a pointwise read sees on the hardware (and the
//   right ones when the shader reads another mip level of the attachment). [nvn] feedback_copy_levels: only the levels the draw's view covers
//   that were written since the copy was last brought up to date are copied (tex_write_gen); the mip chain's draws each copy one level.
struct FbClone {
    Ref<Texture> tex;
    uint64_t last = 0;                                     // serial of the last submission that used the copy
    uint64_t gen[16] = {};                                 // the attachment's write generation of each level when it was copied (0: never)
};

// The texture is the image of a colour slot that the open pass has in the feedback loop layout (its draw samples it there).
inline bool fbl_sampled(Texture* t) {
    if (!P.open || !P.fbl || !t) return false;
    for (uint32_t i = 0; i < P.ncolor && i < 8; ++i)
        if (((P.fbl >> i) & 1) && shares_image(t, P.color[i].tex)) return true;
    return false;
}
void feedback_prepare(const ProgLayout& pl);

void flush_compute_out(int why = kOutGpuReader);       // compute shadows (below): results of direct compute bindings go back to the pool
void compute_out_guard(uint64_t addr, uint64_t size, int why = kOutGpuReader);
void out_texture_read(Texture* t);

// A sampler3D reading a texture whose storage is 2D (a 2D texture the game left bound to the slot, or a 3D texture view of 2D storage): the
// console samples it as a volume one slice deep. Vulkan cannot view a 2D image as 3D, so the draw samples a 3D copy (w x h x 1, every level) of the
// texture's layer, made before the pass and refreshed once per submission (and whenever the CPU wrote the texture).
struct Vol3dCopy {
    Ref<Texture> src;                                      // keeps the key alive while the copy exists
    Ref<Texture> tex;
    uint32_t layer = 0;
    uint64_t copied = UINT64_MAX;                          // serial of the submission of the last copy
    uint64_t last = 0;                                     // serial of the last submission that used it
};
// The copies a draw samples instead of the game's texture (feedback attachments, 3D views of 2D storage). The copies are keyed by the
// texture's address. A texture the game finalizes (every area load makes new render targets) leaves its copy behind, a full-size image
// each (S34 soak: 500 an hour, about 3 GB an hour of GPU memory). A copy that no submission has used for kFbIdleSubmits is released
// (end_submit, every 128th submission); one that is still in use stays (and one released too early is made again on its next use).
constexpr uint64_t kFbIdleSubmits = 600;
struct TexCopies {   // owner: nvn_replay_textures.cpp (feedback_prepare, vol3d_prepare)
    std::unordered_map<Texture*, FbClone> fb_clone;      // attachment -> its copy (same shape, no storage in game memory)
    std::unordered_map<Texture*, Texture*> fb_use;       // attachment -> the copy the current draw samples
    bool fb_seen = false;                                // the last check found a feedback binding: check every draw until it goes away
    std::atomic<int64_t> fb_clone_n{0};                  // fb_clone's size, mirrored for the gauge (the map itself is render thread only)
    uint8_t fbl_want = 0;                                // colour slots the current draw samples through the feedback loop layout (-> ensure_pass)
    std::unordered_map<Texture*, Vol3dCopy> vol3d;       // 2D texture -> its 3D copy
    std::unordered_map<Texture*, Texture*> vol3d_use;    // 2D texture -> the copy the current draw samples
    void begin_submit() { fb_use.clear(); fb_seen = false; }
    void end_submit(uint64_t serial);   // the idle sweeps
    void shutdown();
};
extern TexCopies g_copies;

inline bool wants_vol3d(const DescriptorBinding& b, const TexResolved& r) {
    if (!r.tex || view_type_for(b) != VK_IMAGE_VIEW_TYPE_3D) return false;
    const int target = r.tex->b.target;
    return target != NVN_TEXTURE_TARGET_3D && target != NVN_TEXTURE_TARGET_BUFFER && !target_is_multisample(target) && r.tex->b.samples <= 1 &&
           !tex_is_depth(r.tex);
}
void image_as_texture(const BindingDesc& d, const DescriptorBinding& b, const TexResolved& r, VkDescriptorImageInfo& ii);
void prepare_textures(const ProgLayout& pl);

// SPIR-V ImageFormat of a storage image -> VkFormat (UNDEFINED: Unknown or unmapped).
inline VkFormat image_format_vk(uint32_t f) {
    switch (f) {
        case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 3: return VK_FORMAT_R32_SFLOAT;
        case 4: return VK_FORMAT_R8G8B8A8_UNORM;
        case 5: return VK_FORMAT_R8G8B8A8_SNORM;
        case 6: return VK_FORMAT_R32G32_SFLOAT;
        case 7: return VK_FORMAT_R16G16_SFLOAT;
        case 8: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 9: return VK_FORMAT_R16_SFLOAT;
        case 10: return VK_FORMAT_R16G16B16A16_UNORM;
        case 11: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 12: return VK_FORMAT_R16G16_UNORM;
        case 13: return VK_FORMAT_R8G8_UNORM;
        case 14: return VK_FORMAT_R16_UNORM;
        case 15: return VK_FORMAT_R8_UNORM;
        case 16: return VK_FORMAT_R16G16B16A16_SNORM;
        case 17: return VK_FORMAT_R16G16_SNORM;
        case 18: return VK_FORMAT_R8G8_SNORM;
        case 19: return VK_FORMAT_R16_SNORM;
        case 20: return VK_FORMAT_R8_SNORM;
        case 21: return VK_FORMAT_R32G32B32A32_SINT;
        case 22: return VK_FORMAT_R16G16B16A16_SINT;
        case 23: return VK_FORMAT_R8G8B8A8_SINT;
        case 24: return VK_FORMAT_R32_SINT;
        case 25: return VK_FORMAT_R32G32_SINT;
        case 26: return VK_FORMAT_R16G16_SINT;
        case 27: return VK_FORMAT_R8G8_SINT;
        case 28: return VK_FORMAT_R16_SINT;
        case 29: return VK_FORMAT_R8_SINT;
        case 30: return VK_FORMAT_R32G32B32A32_UINT;
        case 31: return VK_FORMAT_R16G16B16A16_UINT;
        case 32: return VK_FORMAT_R8G8B8A8_UINT;
        case 33: return VK_FORMAT_R32_UINT;
        case 34: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case 35: return VK_FORMAT_R32G32_UINT;
        case 36: return VK_FORMAT_R16G16_UINT;
        case 37: return VK_FORMAT_R8G8_UINT;
        case 38: return VK_FORMAT_R16_UINT;
        case 39: return VK_FORMAT_R8_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

inline int storage_slot_for(int nvn_stage, int c0_offset) {
    // c[0] storage buffer descriptors (byte offsets, as the translator reports them): 0x110 + 0x100 * hardware stage + 0x10 * binding
    // with hardware stage 0 vertex, 3 geometry, 4 fragment; compute: 0x310 + 0x10 * binding. Checked against the translated shaders of the
    // game (vertex 0x110, geometry 0x410, fragment 0x510, compute 0x310 and 0x320).
    int base = 0;
    switch (nvn_stage) {
        case kVertex: base = 0x110; break;
        case kTessCtl: base = 0x210; break;     // tessellation: Ryujinx's hardware stage order (1, 2), not checked on a game shader
        case kTessEval: base = 0x310; break;
        case kGeometry: base = 0x410; break;
        case kFragment: base = 0x510; break;
        case kCompute: base = 0x310; break;
        default: return 0;
    }
    if (c0_offset < base) return 0;
    return (c0_offset - base) / 0x10;
}
extern bool g_shadowing;

// Ops after which the pending results may stay in the pool copy: state that touches no memory, barriers (the copies carry their own),
// dispatches (they check their own ranges) and calls of other lists (their ops are checked one by one). Anything else may read the pool.
inline bool compute_out_may_wait(Op op) {
    if (op == Op::Dispatch || op == Op::DispatchIndirect || op == Op::Barrier || op == Op::CallCommands || op == Op::PushDebugGroup || op == Op::PopDebugGroup) return true;
    return op < Op::DrawArrays && op != Op::SetRenderEnable && op != Op::SetRenderTargets;
}
void out_draw_read(uint64_t addr, uint64_t size, bool copy_ok, int why);
BufRef held_or_pool(uint64_t addr, uint64_t size, int why, bool flush);

const uint8_t* cpu_view(uint64_t addr, uint64_t size);   // below
uint64_t indirect_index_span(const P_DrawIndirect& p);
void out_before_op(const RecordReader& r);
BufRef shadow_of(uint64_t addr, VkDeviceSize size, const BufRef& host, bool writes);
// The scene upscaler's override of one draw (S14), set by scene_upscale_for_draw and read by build_set_resolved; it lives until the next
// draw that sets up descriptors. tex/view: the upscaled scene replaces the small one. near: Nearest (#232), the inputs of the tone-map
// draw that hold the reduced scene, sampled with the nearest sampler (nearest_scene_for_draw).
constexpr uint32_t kSuNearMax = 8;
struct SceneOverride {   // owner: nvn_replay_upscale.cpp
    Texture* tex = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    Texture* near[kSuNearMax] = {};
    uint32_t near_n = 0;
    bool nearest(const Texture* t) const {
        for (uint32_t i = 0; i < near_n; ++i) if (near[i] == t) return true;
        return false;
    }
    void clear() { tex = nullptr; view = VK_NULL_HANDLE; near_n = 0; }
};
extern SceneOverride g_su;
// Temporal upscaling: where the game's camera block is bound (extras::temporal_camera_block), copied at the first submission so the
// per-binding test stays a compare. stage -1: the game names none, nothing matches.
struct TsrBlock {   // owner: nvn_replay_upscale.cpp
    int stage = -1, slot = -1;
    uint64_t bytes = UINT64_MAX;   // the bytes a binding of the block must have
    uint32_t colors = UINT32_MAX;  // the colour targets of a G-buffer draw (at least)
    bool resolved = false;
};
extern TsrBlock g_tsr;
void tsr_resolve();
bool tsr_jitter(uint32_t* words, size_t n_words);
void support_for_draw(const ProgLayout& pl, bool compute);
void bind_sets(const ProgLayout& pl, VkPipelineBindPoint bp);
void temporal_gbuffer_note();
void scene_upscale_for_draw(const ProgLayout& pl);
void do_draw_arrays(const P_DrawArrays& p);
void do_draw_elements(const P_DrawElements& p);
struct QuadArgs {
    bool tried = false, ok = false, index_filled = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    gfx::Buffer index;                 // kQuadMax quads as two triangles each, 32-bit indices
    VkDeviceSize ssbo_align = 256;
    uint64_t draws = 0;
};
extern QuadArgs g_qa;
void do_draw_indirect(const P_DrawIndirect& p);
void do_draw_xfb(const P_DrawXfb& p);
void note_copy_for_cpu(uint64_t src, uint64_t dst, uint64_t size);
void do_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect);
void note_report(uint64_t address, uint64_t counter);
bool conditional_render_enabled(uint32_t mode, uint64_t address);
void do_clear_color(const P_ClearColor& p);
void do_clear_depth_stencil(const P_ClearDepthStencil& p);
void do_clear_texture(const P_ClearTexture& p);
void do_copy_buffer_texture(const P_CopyBufTex& p, bool to_texture);
void do_copy_texture_texture(const P_CopyTexTex& p);
void do_downsample(const P_Downsample& p);
void do_write_memory(uint64_t gpu_addr, const void* data, uint32_t size);
bool cw_ready(CounterWindow& w, bool xfb);
void cw_end_query(CounterWindow& w, bool xfb);
bool counter_window_has_gpu_draw(RecordReader look, uint32_t type);

}  // namespace replay
using namespace replay;

}  // namespace nxrt::nvn::be::gpu
