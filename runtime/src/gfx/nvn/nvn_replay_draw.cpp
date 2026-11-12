// SPDX-License-Identifier: GPL-3.0-or-later
// Draws of the replay: pipeline keys, dynamic state and its shadow, begin_draw, primitive conversion, direct, indirect and feedback draws.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
CmdState g_cmd;
void CmdState::begin_submit() {
    bound_pipeline = VK_NULL_HANDLE;
    dirty = kAllDirty;
    pl = nullptr;
    pipe = VK_NULL_HANDLE;
    topo = pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    restart_valid = false;
    vb_bound_in_cb = false;
    shadow_invalidate();
    want_depth_ro = false;
    key_dummy_binding = 0;
    fs_tonemap = -1;
    fs_stretch = -1;
}
// vkCmdBindIndexBuffer unless the same (buffer, offset, type) is already bound in this command buffer.
static void bind_index_buffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) {
    if (bind_coalesce_on()) {
        if (g_cmd.ibsh.ok && g_cmd.ibsh.buf == buf && g_cmd.ibsh.off == off && g_cmd.ibsh.type == type) { ft::split_count(ft::kCntSkipped); return; }
        g_cmd.ibsh = {buf, off, type, true};
    }
    vkCmdBindIndexBuffer(cb, buf, off, type);
}
static bool g_pipe_fbl = false;        // g_cmd.pipe was made for a pass with a feedback loop attachment (GraphicsKey::fb_loop)
static uint32_t g_key_attr_count = 0;
static uint8_t g_key_binding[16] = {};

XfbState g_xfb;
static bool g_pipe_xfb = false;             // the bound pipeline is a stream-out variant
void XfbState::shutdown() {
    if (counter) device().destroy_buffer(counter);
    *this = XfbState();
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------
static VkFormat scaled_for(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED;
        case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED;
        case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED;
        case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
        case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;
        case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
        case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED;
        case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
        case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
        case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
        case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
        case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
        case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
        case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        default: return f;
    }
}

static bool is_int_format(VkFormat f) { return scaled_for(f) != f; }

// ---- viewport conversion -------------------------------------------------------------------------------------------------
VkRect2D scissor_rect(const int32_t* r) {
    int32_t x = r[0], y = r[1], w = r[2], h = r[3];
    if (P.scale != 1.0f) {     // the edges scale (a rectangle covering the target still covers it)
        const float sc = P.scale;
        const int32_t x1 = static_cast<int32_t>(std::ceil((static_cast<float>(x) + static_cast<float>(w)) * sc - 0.001f));
        const int32_t y1 = static_cast<int32_t>(std::ceil((static_cast<float>(y) + static_cast<float>(h)) * sc - 0.001f));
        x = static_cast<int32_t>(std::floor(static_cast<float>(x) * sc + 0.001f));
        y = static_cast<int32_t>(std::floor(static_cast<float>(y) * sc + 0.001f));
        w = x1 - x;
        h = y1 - y;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    w = std::max(0, std::min<int32_t>(w, static_cast<int32_t>(P.extent.width) - x));
    h = std::max(0, std::min<int32_t>(h, static_cast<int32_t>(P.extent.height) - y));
    return {{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
}

static void set_viewports_and_scissors() {
    VkCommandBuffer cb = F->cb;
    VkViewport vp[16];
    VkRect2D sc[16];
    const uint32_t nvp = S.nvp ? S.nvp : 1;
    for (uint32_t i = 0; i < nvp; ++i) {
        float x = S.vp[i][0], y = S.vp[i][1], w = S.vp[i][2], h = S.vp[i][3];
        if (!S.nvp) { x = 0; y = 0; w = static_cast<float>(P.extent.width); h = static_cast<float>(P.extent.height); }
        else if (P.scale != 1.0f) { x *= P.scale; y *= P.scale; w *= P.scale; h *= P.scale; }
        // Upper-left origin: window y is the image row from the top and NDC +y is the top edge of the rectangle (negative height).
        // Lower-left origin (the GL convention): window y 0 is memory row 0 and NDC -y is the first row, so no flip.
        const bool lower_left = S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        vp[i] = lower_left ? VkViewport{x, y, w, h, S.dr[i][0], S.dr[i][1]}
                           : VkViewport{x, y + h, w, -h, S.dr[i][0], S.dr[i][1]};
        if (i < S.nsc) sc[i] = scissor_rect(S.sc[i]);
        else sc[i] = {{0, 0}, P.extent};
    }
    if (bind_coalesce_on()) {
        // compare the whole arrays (the unused tail is zero) and the count
        std::array<VkViewport, 16> av = {};
        std::array<VkRect2D, 16> as = {};
        std::memcpy(av.data(), vp, sizeof(VkViewport) * nvp);
        std::memcpy(as.data(), sc, sizeof(VkRect2D) * nvp);
        const bool cv = g_cmd.dsh.vp.changed(av) || g_cmd.dsh.nvp != nvp;
        const bool cs = g_cmd.dsh.sc.changed(as) || g_cmd.dsh.nsc != nvp;
        g_cmd.dsh.nvp = g_cmd.dsh.nsc = nvp;
        if (cv) vkCmdSetViewportWithCount(cb, nvp, vp); else ft::split_count(ft::kCntSkipped);
        if (cs) vkCmdSetScissorWithCount(cb, nvp, sc); else ft::split_count(ft::kCntSkipped);
        return;
    }
    vkCmdSetViewportWithCount(cb, nvp, vp);
    vkCmdSetScissorWithCount(cb, nvp, sc);
}

// ---- draws -------------------------------------------------------------------------------------------------------------------
static const ShaderInfo* tcs_now();   // the program's control shader, or the generated pass-through one (below)
static bool build_key(GraphicsKey& key, VkPrimitiveTopology topo) {
    std::memset(&key, 0, sizeof key);
    key.stage[0] = S.stage[kVertex];
    key.stage[1] = S.stage[kFragment];
    key.stage[2] = S.stage[kGeometry];
    if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {      // begin_draw checked: both tessellation stages, patches
        key.tess[0] = tcs_now();
        key.tess[1] = S.stage[kTessEval];
        key.patch_control_points = static_cast<uint8_t>(std::clamp<uint32_t>(S.patch_size, 1, 32));
    }
    const ShaderInfo* vs = key.stage[0];
    // attributes: those the vertex shader reads. An input array takes one location per element. A location the game did not set up
    // (no attribute, or a disabled one) still has to exist in the pipeline; it reads (0, 0, 0, 1) from a dummy binding.
    struct Missing { uint32_t location; ScalarType type; };
    Missing missing[32];
    uint32_t nmissing = 0;
    for (const ShaderIo& in : vs->refl.inputs) {
        const uint32_t elems = in.array ? in.array : 1;
        for (uint32_t e = 0; e < elems; ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 32) break;
            VkFormat f = VK_FORMAT_UNDEFINED;
            const VertexAttribInfo* a = nullptr;
            if (loc < S.attr_count && loc < 16) {
                a = &S.attr[loc];
                f = vertex_format(a->format);
            }
            if (f == VK_FORMAT_UNDEFINED) {
                if (nmissing < 32) missing[nmissing++] = {loc, in.type};
                continue;
            }
            // a float input fed by an integer attribute gets the raw bits: the integer format stays and the pipeline patches the shader
            // (spirv_int_attributes); stream-out draws use the shader's stream-out variant, which is not patched: scaled floats there
            if (in.type == ScalarType::Float && is_int_format(f) && g_xfb.active) f = scaled_for(f);
            else if ((in.type == ScalarType::Uint || in.type == ScalarType::Sint) && !vk_format_is_integer(f))   // 32-bit integers too
                log_once(kOnceAttrTypeMismatch, "a vertex attribute with a float format feeds an integer shader input (needs a SPIR-V variant)");
            if (key.attr_count >= 16) break;
            KeyAttr& k = key.attr[key.attr_count++];
            k.location = static_cast<uint8_t>(loc);
            k.binding = static_cast<uint8_t>(a->stream & 15);
            k.offset = static_cast<uint16_t>(a->relative_offset);
            k.format = f;
            key.binding_instance[k.binding] = S.stream[k.binding].divisor > 0 ? 1 : 0;
        }
    }
    if (nmissing) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < key.attr_count; ++i) used |= 1u << key.attr[i].binding;
        uint32_t free_binding = 0;
        while (free_binding < 16 && (used >> free_binding & 1)) ++free_binding;
        if (free_binding < 16) {
            key.dummy_binding = static_cast<uint8_t>(free_binding + 1);
            key.binding_instance[free_binding] = 0;
            for (uint32_t i = 0; i < nmissing && key.attr_count < 16; ++i) {
                KeyAttr& k = key.attr[key.attr_count++];
                k.location = static_cast<uint8_t>(missing[i].location);
                k.binding = static_cast<uint8_t>(free_binding);
                const bool is_float = missing[i].type != ScalarType::Uint && missing[i].type != ScalarType::Sint;
                k.offset = is_float ? 0 : 16;
                k.format = is_float ? VK_FORMAT_R32G32B32A32_SFLOAT : (missing[i].type == ScalarType::Uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SINT);
            }
        }
    }
    key.color_count = P.ncolor;
    if (!P.ncolor && S.stage[kFragment] && !S.stage[kFragment]->refl.outputs.empty()) key.no_out = 1;   // a depth-only pass: no outputs
    if (!key.no_out && device().features().dynamic_rendering_unused_attachments) {
        // the fragment shader may write outputs the pass has no attachment for: they are listed as unused (format UNDEFINED)
        if (const ShaderInfo* fs = S.stage[kFragment])
            for (const ShaderIo& o : fs->refl.outputs)
                for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
                    if (o.location + e < 8 && o.location + e >= key.color_count) key.color_count = o.location + e + 1;
    }
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        if (i >= P.ncolor) {
            key.color_format[i] = VK_FORMAT_UNDEFINED;
            key.write_mask[i] = 0;
            continue;
        }
        key.color_format[i] = P.color_fmt[i];
        key.blend_enable[i] = (S.color.blend_enable_mask >> i) & 1;
        const BlendInfo& b = S.blend[i];
        key.blend_src_rgb[i] = b.src_rgb ? vk_blend_factor(b.src_rgb) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_rgb[i] = b.dst_rgb ? vk_blend_factor(b.dst_rgb) : VK_BLEND_FACTOR_ZERO;
        key.blend_src_a[i] = b.src_a ? vk_blend_factor(b.src_a) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_a[i] = b.dst_a ? vk_blend_factor(b.dst_a) : VK_BLEND_FACTOR_ZERO;
        key.blend_op_rgb[i] = b.eq_rgb ? vk_blend_op(b.eq_rgb) : VK_BLEND_OP_ADD;
        key.blend_op_a[i] = b.eq_a ? vk_blend_op(b.eq_a) : VK_BLEND_OP_ADD;
        key.write_mask[i] = S.chmask.mask[i] & 0xf;
        if (P.color_fmt[i] == VK_FORMAT_UNDEFINED) key.write_mask[i] = 0;
        auto uses_src1 = [](VkBlendFactor f) {
            return f == VK_BLEND_FACTOR_SRC1_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR || f == VK_BLEND_FACTOR_SRC1_ALPHA ||
                   f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        };
        if (key.blend_enable[i] && (uses_src1(static_cast<VkBlendFactor>(key.blend_src_rgb[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_rgb[i])) ||
                                    uses_src1(static_cast<VkBlendFactor>(key.blend_src_a[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_a[i]))))
            key.dual_src = 1;
    }
    key.xfb = g_xfb.active && device().features().transform_feedback && S.stage[kVertex] && xfb_stride(S.stage[kVertex]) ? 1 : 0;
    key.depth_format = P.depth_fmt;
    key.stencil_format = P.has_stencil ? P.depth_fmt : VK_FORMAT_UNDEFINED;
    key.samples = P.samples;
    key.topology = static_cast<uint8_t>(topo);
    key.polygon_mode = static_cast<uint8_t>(vk_polygon_mode(S.poly.polygon_mode));
    if (key.polygon_mode != VK_POLYGON_MODE_FILL && !device().features().fill_mode_non_solid) key.polygon_mode = VK_POLYGON_MODE_FILL;
    key.depth_clamp = S.depth_clamp && device().features().depth_clamp;
    key.alpha_to_coverage = S.ms.alpha_to_coverage && S.ms.enable ? 1 : 0;
    key.depth_neg_one = S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? 1 : 0;
    if (S.color.logic_op != 0 && S.color.logic_op != NVN_LOGIC_OP_COPY && device().features().logic_op) {
        key.logic_op_enable = 1;
        key.logic_op = static_cast<uint8_t>(vk_logic_op(S.color.logic_op));
        for (uint32_t i = 0; i < 8; ++i) key.blend_enable[i] = 0;
    }
    if (S.color.alpha_test != 0 && S.color.alpha_test != NVN_ALPHA_FUNC_ALWAYS) log_once(kOnceAlphaTest, "alpha test is enabled by the game: not implemented (needs a shader variant)");
    return true;
}

static void set_dynamic_state(uint32_t dirty, VkPrimitiveTopology topo, bool indexed, int index_type) {
    VkCommandBuffer cb = F->cb;
    const bool co = bind_coalesce_on();
    // true: send the value (always when the key is off)
    auto ch = [co](auto& sh, const auto& v) {
        if (!co || sh.changed(v)) return true;
        ft::split_count(ft::kCntSkipped);
        return false;
    };
    if (dirty & kDynVp) set_viewports_and_scissors();
    if (dirty & kDynRaster) {
        const VkCullModeFlags cull = vk_cull_mode(S.poly.cull_face);
        if (ch(g_cmd.dsh.cull, static_cast<uint32_t>(cull))) vkCmdSetCullMode(cb, cull);
        VkFrontFace ff = vk_front_face(S.poly.front_face);
        if (S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) ff = ff == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // unflipped viewport
        if (ch(g_cmd.dsh.front, static_cast<uint32_t>(ff))) vkCmdSetFrontFace(cb, ff);
        if (ch(g_cmd.dsh.discard, static_cast<uint32_t>(S.rasterizer_discard ? 1 : 0))) vkCmdSetRasterizerDiscardEnable(cb, S.rasterizer_discard);
        const float lw = device().features().wide_lines ? std::max(1.0f, S.line_width) : 1.0f;
        if (ch(g_cmd.dsh.line_width, lw)) vkCmdSetLineWidth(cb, lw);
    }
    if (topo != g_cmd.topo) {
        vkCmdSetPrimitiveTopology(cb, topo);
        g_cmd.topo = topo;
    }
    const bool restart = S.prim_restart && indexed &&
                         ((index_type == NVN_INDEX_TYPE_UNSIGNED_INT && S.restart_index == 0xffffffffu) || (index_type == NVN_INDEX_TYPE_UNSIGNED_SHORT && S.restart_index == 0xffffu) ||
                          (index_type == NVN_INDEX_TYPE_UNSIGNED_BYTE && S.restart_index == 0xffu));
    if (!g_cmd.restart_valid || restart != g_cmd.restart || (!co && (dirty & kDynRaster))) {
        vkCmdSetPrimitiveRestartEnable(cb, restart);
        g_cmd.restart = restart;
        g_cmd.restart_valid = true;
    }
    const bool have_depth = P.depth_fmt != VK_FORMAT_UNDEFINED;
    if (dirty & kDynDepth) {
        const bool dt = have_depth && S.ds.depth_test, dw = have_depth && !P.depth_ro && S.ds.depth_test && S.ds.depth_write;
        const VkCompareOp dc = vk_compare_op(S.ds.depth_func ? S.ds.depth_func : NVN_DEPTH_FUNC_LESS);
        const bool dbt = have_depth && S.depth_bounds && device().features().depth_bounds;
        if (ch(g_cmd.dsh.depth_test, static_cast<uint32_t>(dt))) vkCmdSetDepthTestEnable(cb, dt);
        if (ch(g_cmd.dsh.depth_write, static_cast<uint32_t>(dw))) vkCmdSetDepthWriteEnable(cb, dw);
        if (ch(g_cmd.dsh.depth_cmp, static_cast<uint32_t>(dc))) vkCmdSetDepthCompareOp(cb, dc);
        if (ch(g_cmd.dsh.depth_bounds_test, static_cast<uint32_t>(dbt))) vkCmdSetDepthBoundsTestEnable(cb, dbt);
        if (S.depth_bounds && device().features().depth_bounds && ch(g_cmd.dsh.depth_bounds, std::array<float, 2>{S.db_near, S.db_far})) vkCmdSetDepthBounds(cb, S.db_near, S.db_far);
    }
    if (dirty & kDynStencil) {
        const bool ste = P.has_stencil && S.ds.stencil_test;
        if (ch(g_cmd.dsh.stencil_test, static_cast<uint32_t>(ste))) vkCmdSetStencilTestEnable(cb, ste);
        for (int f = 0; f < 2; ++f) {
            const VkStencilFaceFlags face = f == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
            const std::array<uint32_t, 5> op = {static_cast<uint32_t>(face), static_cast<uint32_t>(vk_stencil_op(S.ds.stencil_fail[f])), static_cast<uint32_t>(vk_stencil_op(S.ds.depth_pass[f])),
                                                static_cast<uint32_t>(vk_stencil_op(S.ds.depth_fail[f])), static_cast<uint32_t>(vk_compare_op(S.ds.stencil_func[f] ? S.ds.stencil_func[f] : NVN_STENCIL_FUNC_ALWAYS))};
            if (ch(g_cmd.dsh.stencil_op[f], op))
                vkCmdSetStencilOp(cb, face, static_cast<VkStencilOp>(op[1]), static_cast<VkStencilOp>(op[2]), static_cast<VkStencilOp>(op[3]), static_cast<VkCompareOp>(op[4]));
            if (ch(g_cmd.dsh.stencil_ref[f], static_cast<uint32_t>(S.stencil_ref[f]))) vkCmdSetStencilReference(cb, face, S.stencil_ref[f]);
            if (ch(g_cmd.dsh.stencil_cmask[f], static_cast<uint32_t>(S.stencil_vmask[f]))) vkCmdSetStencilCompareMask(cb, face, S.stencil_vmask[f]);
            const uint32_t wm = P.depth_ro ? 0u : static_cast<uint32_t>(S.stencil_wmask[f]);
            if (ch(g_cmd.dsh.stencil_wmask[f], wm)) vkCmdSetStencilWriteMask(cb, face, wm);
        }
    }
    if (dirty & kDynBias) {
        const int off = S.poly.offset_enables;
        const int mode = S.poly.polygon_mode;
        const bool bias = (mode == NVN_POLYGON_MODE_FILL && (off & NVN_POLYGON_OFFSET_ENABLE_FILL)) ||
                          (mode == NVN_POLYGON_MODE_LINE && (off & NVN_POLYGON_OFFSET_ENABLE_LINE)) ||
                          (mode == NVN_POLYGON_MODE_POINT && (off & NVN_POLYGON_OFFSET_ENABLE_POINT));
        if (ch(g_cmd.dsh.bias_enable, static_cast<uint32_t>(bias))) vkCmdSetDepthBiasEnable(cb, bias);
        const float bclamp = device().features().depth_bias_clamp ? S.bias_clamp : 0.0f;
        if (ch(g_cmd.dsh.bias, std::array<float, 3>{S.bias_units, bclamp, S.bias_factor})) vkCmdSetDepthBias(cb, S.bias_units, bclamp, S.bias_factor);
    }
    if (dirty & kDynBlend) {
        const std::array<float, 4> bc = {S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]};
        if (ch(g_cmd.dsh.blend, bc)) vkCmdSetBlendConstants(cb, S.blend_color);
    }
}

// Evaluation-only programs (NVN tessellates with the fixed SetInner/OuterTessellationLevels): a generated pass-through control shader
// (nvn_tess_gen.h), one per (vertex shader, evaluation shader, patch size). It copies each per-vertex input of the evaluation shader from the
// vertex shader output at the same location (zero where the vertex shader writes none) and gl_Position, zeroes the per-patch inputs, and reads
// the levels from push constants (pushed before each such draw, so level changes need no new pipeline). Null when an interface variable has a
// type the generator does not handle.
static const ShaderInfo* g_gen_tcs = nullptr;   // the generated control shader of the current draw (null: the program has its own, or none)
static const ShaderInfo* passthrough_tcs_for(const ShaderInfo* vs, const ShaderInfo* tes) {
    struct Key {
        const ShaderInfo* vs; const ShaderInfo* tes; uint32_t n;
        bool operator<(const Key& o) const { return std::tie(vs, tes, n) < std::tie(o.vs, o.tes, o.n); }
    };
    static std::map<Key, std::unique_ptr<ShaderInfo>> cache;
    const uint32_t n = std::clamp<uint32_t>(S.patch_size, 1, 32);
    const Key key{vs, tes, n};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    auto to_io = [](const ShaderIo& v, tessgen::Io& o) {
        if (v.components < 1 || v.components > 4) return false;
        if (v.type == ScalarType::Float) o.kind = tessgen::Kind::Float;
        else if (v.type == ScalarType::Sint) o.kind = tessgen::Kind::Sint;
        else if (v.type == ScalarType::Uint) o.kind = tessgen::Kind::Uint;
        else return false;
        o.location = v.location;
        o.component = v.component;
        o.components = v.components;
        return true;
    };
    std::vector<tessgen::Io> vs_out, per_vertex, per_patch;
    bool ok = true;
    for (const ShaderIo& o : vs->refl.outputs) {   // an output array (several locations) is not matched: its readers get zero
        tessgen::Io io;
        if (o.array <= 1 && to_io(o, io)) vs_out.push_back(io);
    }
    for (const ShaderIo& in : tes->refl.inputs) {
        tessgen::Io io;
        if (!to_io(in, io)) { ok = false; break; }
        (in.array > 1 ? per_vertex : per_patch).push_back(io);   // in an evaluation shader a non-array input is per patch
    }
    std::unique_ptr<ShaderInfo> si;
    if (ok) {
        const std::vector<tessgen::Copy> copies = tessgen::match(vs_out, per_vertex);
        size_t copied = 0;
        for (const tessgen::Copy& c : copies) copied += c.has_src;
        si = std::make_unique<ShaderInfo>();
        si->hash = "tcs-pass-" + vs->hash + "-" + tes->hash + "-" + std::to_string(n);
        si->stage = ShaderStage::TessControl;
        // gl_ClipDistance / gl_CullDistance: copied when the vertex shader writes them and the evaluation shader reads gl_in[]'s
        auto uses = [](const std::vector<uint32_t>& v, uint32_t b) { return std::find(v.begin(), v.end(), b) != v.end(); };
        constexpr uint32_t kBuiltInClip = 3, kBuiltInCull = 4;
        const bool clip = uses(vs->refl.builtins_used_out, kBuiltInClip) && uses(tes->refl.builtins_used_in, kBuiltInClip);
        const bool cull = uses(vs->refl.builtins_used_out, kBuiltInCull) && uses(tes->refl.builtins_used_in, kBuiltInCull);
        si->spirv = tessgen::passthrough_tcs(copies, per_patch, n, 7, 1, clip, cull);
        si->push_bytes = tessgen::kPushBytes;
        std::string err;
        if (!reflect_spirv(si->spirv.data(), si->spirv.size(), si->refl, &err)) {
            NXLOG_W(kTag, "generated control shader for %s does not reflect: %s", tes->hash.c_str(), err.c_str());
            si.reset();
        } else {
            NXLOG_I(kTag, "evaluation shader %s (vs %s): pass-through control shader, %u vertices, %zu per-vertex locations (%zu from the vertex shader, %zu zero), %zu per-patch%s%s",
                    tes->hash.c_str(), vs->hash.c_str(), n, copies.size(), copied, copies.size() - copied, per_patch.size(), clip ? ", clip distances" : "",
                    cull ? ", cull distances" : "");
        }
    } else {
        NXLOG_W(kTag, "evaluation shader %s: an input is not a 1..4 component float/int, no pass-through control shader", tes->hash.c_str());
    }
    const ShaderInfo* out = si.get();
    cache.emplace(key, std::move(si));
    return out;
}
static const ShaderInfo* tcs_now() { return S.stage[kTessCtl] ? S.stage[kTessCtl] : g_gen_tcs; }

// Prepares everything a draw needs and binds it. False: skip the draw.
static bool begin_draw(int mode, bool indexed, int index_type, VkPrimitiveTopology* topo_out) {
    RpScope rp_all(kRpDraw);
    PoolReasonScope pr_(kReasonVertex);
    const ShaderInfo* vs = S.stage[kVertex];
    if (!vs) { log_once(kOnceDrawNoVs, "draw without a vertex shader skipped"); return false; }
    if (!S.render_enable) return false;
    if (key_check_on()) key_check_draw(counters().presents.load(std::memory_order_relaxed));
    if (g_trace_hooks && trace_skip_draw()) return false;
    // Tessellation: Vulkan runs it only with both shaders and a patch list. NVN also allows an evaluation shader alone (fixed levels from
    // SetInner/OuterTessellationLevels): those draws get a generated pass-through control shader (passthrough_tcs_for).
    if (S.stage[kTessCtl] || S.stage[kTessEval] || mode == NVN_DRAW_PRIMITIVE_PATCHES) {
        if (mode != NVN_DRAW_PRIMITIVE_PATCHES) { note_unsupported("tessellation draw: tessellation program drawn without patches"); return false; }
        if (!S.stage[kTessEval]) { note_unsupported("tessellation draw: patches without a tessellation evaluation shader"); return false; }
        if (!S.stage[kTessCtl]) {
            const ShaderInfo* gen = passthrough_tcs_for(vs, S.stage[kTessEval]);
            if (!gen) { note_unsupported("tessellation draw: evaluation shader without a control shader, no pass-through could be made"); return false; }
            if (gen != g_gen_tcs) g_cmd.dirty |= kPipe;
            g_gen_tcs = gen;
        }
    } else {
        g_gen_tcs = nullptr;
    }
    // The layout depends on the stages only: they change with BindProgram (kPipe) and the generated control shader (kPipe above);
    // begin_submit and the full resets set kPipe too. kDesc alone (a new binding) keeps the same layout.
    if (g_cmd.dirty & kPipe) {
        const ShaderInfo* stages[6] = {vs, S.stage[kFragment], S.stage[kGeometry], tcs_now(), S.stage[kTessEval], nullptr};
        g_cmd.pl = prog_layout(stages);
        g_cmd.fs_tonemap = -1;
        g_cmd.fs_stretch = -1;
    }
    if (g_trace_hooks) trace_note_fs();
    const ProgLayout* pl = g_cmd.pl;
    if (!pl) return false;
    if (pl->c0_graphics && g_sets.draw_base_instance != g_sets.bank_base_instance) g_cmd.dirty |= kDesc;   // c[0] + 0x644 changed: new sets
    if (extras::temporal_wanted()) temporal_gbuffer_note();
    if (g_cmd.dirty & (kDesc | kPipe)) {
        RpScope rp_(kRpPrep);
        g_cmd.prog_writes = false;
        for (const BindingDesc& d : pl->bindings) g_cmd.prog_writes |= d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        attach_targets(false);
        g_cmd.want_depth_ro = samples_depth_target(*pl);
        prepare_textures(*pl);
        if (g_trace) trace_dump_raw(*pl);
        // tonemap_for_draw acts only on a tone-map draw into a colour target, scene_stretch_for_draw only on the game's scene stretch
        // shader; every other draw clears the override. Whether the fragment shader is one of them is asked once per bound program
        // (hdr_is_tonemap: a mutex and a map of hash strings), at its first draw into a colour target.
        const ShaderInfo* fs = S.stage[kFragment];
        const bool colour = fs && S.ncolor > 0 && S.color_tex[0];
        if (colour && g_cmd.fs_tonemap < 0) g_cmd.fs_tonemap = extras::hdr_is_tonemap(fs->spirv, fs->hash) ? 1 : 0;
        if (colour && g_cmd.fs_stretch < 0) g_cmd.fs_stretch = extras::is_scene_stretch(fs->hash) ? 1 : 0;
        if (colour && g_cmd.fs_tonemap > 0) tonemap_for_draw(*pl);
        else if (colour && g_cmd.fs_stretch > 0) scene_stretch_for_draw(*pl);
        else g_su.clear();
    }
    if (g_copies.fb_seen || (g_cmd.dirty & (kDesc | kPipe))) feedback_prepare(*pl);
    bool pass_ok;
    { RpScope rp_(kRpPass); pass_ok = ensure_pass(g_cmd.want_depth_ro, g_copies.fbl_want); }
    if (!pass_ok) { log_once(kOnceDrawNoTarget, "draw without a usable render target skipped"); return false; }
    if (ui_skip_draw(vs)) return false;   // after the pass is begun, so its clears and loads still happen (nxrt/ui_layer.h)
    if (g_cmd.prog_writes || g_xfb.active) g_bar.pass_unsafe = true;
    const VkPrimitiveTopology topo = vk_topology(mode);
    if ((g_cmd.dirty & kPipe) || topo != g_cmd.pipe_topo || !g_cmd.pipe || g_pipe_fbl != (P.fbl != 0)) {
        RpScope rp_(kRpPipe);
        GraphicsKey key;
        build_key(key, topo);
        key.fb_loop = P.fbl != 0 ? 1 : 0;   // the pass has an attachment in the feedback loop layout
        g_pipe_fbl = key.fb_loop != 0;
        g_cmd.pipe = graphics_pipeline(key, *pl);
        g_pipe_xfb = key.xfb != 0;
        g_cmd.pipe_topo = topo;
        g_key_attr_count = key.attr_count;
        g_cmd.key_dummy_binding = key.dummy_binding;
        for (uint32_t i = 0; i < key.attr_count; ++i) g_key_binding[i] = key.attr[i].binding;
    }
    if (!g_cmd.pipe) return false;
    // a draw inside a stream-out section that has no stream-out variant and nothing to render to has no effect
    if (g_xfb.active && !g_pipe_xfb && !P.ncolor && !S.depth_tex) return false;
    if (g_trace) trace_draw(mode, indexed, *pl);
    if (g_xfb.active) {
        if (first_time_key(kFirstXfbDraw, 0, S.stage[kVertex]->hash))
            NXLOG_I("nvn.xfb", "draw under transform feedback: vs %s mode %d attrs %u xfb0 %llx/%llu", S.stage[kVertex]->hash.c_str(), mode, S.attr_count,
                    static_cast<unsigned long long>(S.xfb[0].addr), static_cast<unsigned long long>(S.xfb[0].size));
    }
    if (g_cmd.pipe != g_cmd.bound_pipeline) {
        xfb_end_in_pass();     // the pipeline cannot change while feedback is active; the byte counter carries the position over
        vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_cmd.pipe);
        g_gauge_pipeline_binds.add(1);
        g_cmd.bound_pipeline = g_cmd.pipe;
    }
    if (g_xfb.active && g_pipe_xfb && !g_xfb.began && device().features().transform_feedback && S.xfb[0].addr) {
        BufRef xb = resolve_buffer(S.xfb[0].addr, S.xfb[0].size);
        if (xb.ok) {
            gpu_wrote(S.xfb[0].addr, S.xfb[0].size ? S.xfb[0].size : xb.avail, kWriteXfb);
            if (!g_xfb.counter) {
                if (device().create_buffer(kXfbSlots * kXfbSlotBytes, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Upload,
                                           &g_xfb.counter, "nvn xfb counter") != VK_SUCCESS)
                    g_xfb.counter = gfx::Buffer();
            }
            const VkDeviceSize size = std::min<VkDeviceSize>(S.xfb[0].size ? S.xfb[0].size : xb.avail, xb.avail) & ~static_cast<VkDeviceSize>(3);
            vkCmdBindTransformFeedbackBuffersEXT(F->cb, 0, 1, &xb.buffer, &xb.offset, &size);
            VkBuffer cbuf = g_xfb.counter.buffer;
            const VkDeviceSize coff = static_cast<VkDeviceSize>(g_xfb.cur) * kXfbSlotBytes;
            if (g_xfb.have_counter && cbuf) vkCmdBeginTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &coff);
            else vkCmdBeginTransformFeedbackEXT(F->cb, 0, 0, nullptr, nullptr);
            g_xfb.began = true;
        } else {
            note_unsupported("transform feedback into an address outside every pool");
        }
    }
    { RpScope rp_(kRpDyn); set_dynamic_state(g_cmd.dirty, topo, indexed, index_type); }
    if (g_cmd.dirty & kDesc) {
        RpScope rp_(kRpSets);
        support_for_draw(*pl, false);
        bind_sets(*pl, VK_PIPELINE_BIND_POINT_GRAPHICS);
        g_sets.bank_base_instance = g_sets.draw_base_instance;
    }
    if (pl->push_bytes && g_gen_tcs) {
        // the pass-through control shader's tessellation levels (NVN's fixed SetInner/OuterTessellationLevels), every draw: cheap, and
        // the layout may have changed since the last push
        tessgen::TessPush push = {};
        std::memcpy(push.outer, S.tess_outer, sizeof push.outer);
        std::memcpy(push.inner, S.tess_inner, sizeof push.inner);
        vkCmdPushConstants(F->cb, pl->layout, pl->push_stages, 0, std::min<uint32_t>(pl->push_bytes, sizeof push), &push);
        static std::set<const ShaderInfo*> logged;
        if (logged.insert(g_gen_tcs).second)
            NXLOG_I(kTag, "tessellation draw: evaluation shader %s drawn with pass-through control shader %s, patch size %u, levels %g %g %g %g / %g %g",
                    S.stage[kTessEval]->hash.c_str(), g_gen_tcs->hash.c_str(), static_cast<unsigned>(std::clamp<uint32_t>(S.patch_size, 1, 32)),
                    S.tess_outer[0], S.tess_outer[1], S.tess_outer[2], S.tess_outer[3], S.tess_inner[0], S.tess_inner[1]);
    }
    if (g_cmd.dirty & (kVb | kPipe)) {
        RpScope rp_(kRpVb);
        const bool co = bind_coalesce_on();
        // pending run of bindings to bind with one call
        VkBuffer rb[16];
        VkDeviceSize ro[16], rs[16], rst[16];
        uint32_t rfirst = 0, rn = 0;
        auto flush = [&] {
            if (!rn) return;
            vkCmdBindVertexBuffers2(F->cb, rfirst, rn, rb, ro, rs, rst);
            if (rn > 1) ft::split_count(ft::kCntMerged, rn - 1);
            g_cmd.vb_bound_in_cb = true;
            rn = 0;
        };
        auto add = [&](uint32_t binding, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkDeviceSize stride) {
            if (co && binding < 16) {
                VbShadow& sh = g_cmd.vbsh[binding];
                if (sh.ok && sh.buf == buf && sh.off == off && sh.size == size && sh.stride == stride) { ft::split_count(ft::kCntSkipped); flush(); return; }
                sh = {buf, off, size, stride, true};
                if (rn && rfirst + rn == binding) {
                    rb[rn] = buf; ro[rn] = off; rs[rn] = size; rst[rn] = stride; ++rn;
                    return;
                }
                flush();
                rfirst = binding;
                rb[0] = buf; ro[0] = off; rs[0] = size; rst[0] = stride;
                rn = 1;
                return;
            }
            vkCmdBindVertexBuffers2(F->cb, binding, 1, &buf, &off, &size, &stride);
            g_cmd.vb_bound_in_cb = true;
        };
        // bindings in ascending order so that neighbours merge
        uint32_t order[16], norder = 0;
        for (uint32_t i = 0; i < g_key_attr_count; ++i) {
            const uint32_t binding = g_key_binding[i];
            bool done = false;
            for (uint32_t j = 0; j < norder; ++j) if (order[j] == binding) done = true;
            if (!done && norder < 16) order[norder++] = binding;
        }
        if (co) std::sort(order, order + norder);
        for (uint32_t k = 0; k < norder; ++k) {
            const uint32_t binding = order[k];
            if (g_cmd.key_dummy_binding && binding == static_cast<uint32_t>(g_cmd.key_dummy_binding - 1)) {
                add(binding, dummy_buffer(), kDummyAttrFloatOffset, 32, 0);
                continue;
            }
            const GState::Bind& vb = S.vtx[binding];
            BufRef br = vb.addr ? resolve_read(vb.addr, vb.size, kReadVertex) : BufRef{dummy_buffer(), 0, 65536, false};
            VkDeviceSize size = vb.size ? std::min<VkDeviceSize>(vb.size, br.avail) : br.avail;
            VkDeviceSize stride = static_cast<VkDeviceSize>(S.stream[binding].stride);
            add(binding, br.buffer, br.offset, size, stride);
        }
        flush();
        if (!g_cmd.vb_bound_in_cb) {
            // the pipeline's vertex binding strides are dynamic: a command buffer that never bound a vertex buffer must still bind one
            VkBuffer dummy = dummy_buffer();
            VkDeviceSize off = 0, size = 65536, stride = 0;
            vkCmdBindVertexBuffers2(F->cb, 0, 1, &dummy, &off, &size, &stride);
            g_cmd.vb_bound_in_cb = true;
            if (co) g_cmd.vbsh[0] = {dummy, off, size, stride, true};
        }
    }
    g_cmd.dirty = g_cmd.mirror_stale ? (kVb | kDesc) : 0;     // a storage buffer of this draw made copied pages stale: the next draw binds again
    g_cmd.mirror_stale = false;
    *topo_out = topo;
    return true;
}

// Counts a draw for the primitive counters, before the replay decides whether to skip it: the console generates the primitives of every
// draw it runs, and a draw of a feedback section also writes them (14). A draw with rendering disabled is not run by the console: not counted.
static void count_primitives(uint32_t mode, uint32_t count, uint32_t instances) {
    if (!S.render_enable) return;
    const uint64_t per = mode == NVN_DRAW_PRIMITIVE_PATCHES ? count / std::max<uint32_t>(1, S.patch_size) : primitives_for(static_cast<int>(mode), count);   // patches: input patches
    const uint64_t prims = per * std::max<uint32_t>(1, instances);
    g_q.prims_generated += prims;
    if (g_xfb.active) g_q.prims_xfb += prims;
}

// The number of indices bind_prim_indices makes from n vertices.
static uint64_t prim_index_count(PrimConversion conv, uint32_t n) {
    switch (conv) {
        case PrimConversion::Quads: return static_cast<uint64_t>(n / 4) * 6;
        case PrimConversion::QuadStrip: return n >= 4 ? static_cast<uint64_t>((n - 4) / 2 + 1) * 6 : 0;
        case PrimConversion::Polygon: return n >= 3 ? static_cast<uint64_t>(n - 2) * 3 : 0;
        case PrimConversion::LineLoop: return n ? static_cast<uint64_t>(n) + 1 : 0;
        default: return n;
    }
}

// Quads, quad strips, polygons and line loops, which Vulkan has no topology for, as the triangle (or line strip) index list of the same
// vertices, written straight into the upload buffer and bound as 32-bit indices (also widens 8-bit indices: conv None). `in(i)` gives the
// i-th vertex index of the draw (for DrawArrays: first + i); each is read once, in order. Shared by the direct and the indirect draws.
// Returns the index count; 0: nothing to draw, or no room in the upload buffer (nothing bound).
template <class Src>
static uint64_t bind_prim_indices(PrimConversion conv, uint32_t n, Src in) {
    const uint64_t m = prim_index_count(conv, n);
    if (!m) return 0;
    UploadAlloc u = F->upload(m * 4, 4);
    if (!u) return 0;
    uint8_t* o = u.ptr;
    auto put = [&o](uint32_t v) { std::memcpy(o, &v, 4); o += 4; };
    switch (conv) {
        case PrimConversion::Quads:
            for (uint32_t q = 0; q + 3 < n; q += 4) {
                const uint32_t a = in(q), b = in(q + 1), c = in(q + 2), d = in(q + 3);
                put(a); put(b); put(c); put(a); put(c); put(d);
            }
            break;
        case PrimConversion::QuadStrip: {   // n >= 4
            uint32_t a = in(0), b = in(1);
            for (uint32_t q = 0; q + 3 < n; q += 2) {
                const uint32_t c = in(q + 2), d = in(q + 3);
                put(a); put(b); put(c); put(c); put(b); put(d);
                a = c; b = d;
            }
            break;
        }
        case PrimConversion::Polygon: {     // n >= 3
            const uint32_t f = in(0);
            uint32_t prev = in(1);
            for (uint32_t k = 1; k + 1 < n; ++k) {
                const uint32_t next = in(k + 1);
                put(f); put(prev); put(next);
                prev = next;
            }
            break;
        }
        case PrimConversion::LineLoop: {    // n >= 1
            const uint32_t f = in(0);
            put(f);
            for (uint32_t i = 1; i < n; ++i) put(in(i));
            put(f);
            break;
        }
        default:
            for (uint32_t i = 0; i < n; ++i) put(in(i));
            break;
    }
    bind_index_buffer(F->cb, u.buffer, u.offset, VK_INDEX_TYPE_UINT32);
    return m;
}

// The i-th index of an index list of isz bytes per index, widened to 32 bits.
static inline uint32_t read_index(const uint8_t* src, uint32_t isz, uint32_t i) {
    if (isz == 1) return src[i];
    if (isz == 2) { uint16_t t; std::memcpy(&t, src + i * 2, 2); return t; }
    uint32_t v; std::memcpy(&v, src + i * 4, 4); return v;
}

// [gfx] gpu_checkpoints = all: a checkpoint before the draw names it (vertex and fragment shader, counts) if the device is lost while it runs.
// One compare when off or without NVIDIA's extension. Indirect: a, b = the guest address of the arguments, c = the draw count limit.
static void checkpoint_draw(uint32_t a, uint32_t b, uint32_t c, bool indirect) {
    gfx::Device& d = device();
    if (!d.checkpoints_on(gfx::Device::Checkpoint::Draw)) return;
    d.checkpoint(F->cb, gfx::Device::Checkpoint::Draw, S.stage[kVertex] ? &S.stage[kVertex]->hash : nullptr,
                 S.stage[kFragment] ? &S.stage[kFragment]->hash : nullptr, a, b, c, indirect);
}

void do_draw_arrays(const P_DrawArrays& p) {
    count_primitives(p.mode, p.count, p.instance_count);
    g_sets.draw_base_instance = p.base_instance;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    checkpoint_draw(p.count, std::max<uint32_t>(1, p.instance_count), 0, false);
    if (conv == PrimConversion::None) {
        TRACE("    vertices %u instances %u first %u", p.count, p.instance_count, p.first);
        vkCmdDraw(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), p.first, p.base_instance);
    } else {
        // QUADS / QUAD_STRIP / POLYGON / LINE_LOOP: generate an index buffer
        const uint32_t first = p.first;
        const uint64_t m = bind_prim_indices(conv, p.count, [first](uint32_t k) { return first + k; });
        if (!m) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(m), std::max<uint32_t>(1, p.instance_count), 0, 0, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    P.draws++;
    P.verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    if (g_trace) trace_snap_after_draw();
}

void do_draw_elements(const P_DrawElements& p) {
    PoolReasonScope pr_(kReasonIndex);
    count_primitives(p.mode, p.count, p.instance_count);
    g_sets.draw_base_instance = p.base_instance;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), true, static_cast<int>(p.type), &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (g_trace) trace_indexed(p);
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    const uint8_t* src = host_ptr(p.index_address);
    checkpoint_draw(p.count, std::max<uint32_t>(1, p.instance_count), 0, false);
    if (conv != PrimConversion::None || isz == 1) {
        // widen to 32 bit and/or convert the primitive
        const uint64_t m = bind_prim_indices(conv, p.count, [src, isz](uint32_t i) { return read_index(src, isz, i); });
        if (!m) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(m), std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    } else {
        BufRef br = resolve_read(p.index_address, static_cast<uint64_t>(p.count) * isz, kReadIndex);
        bind_index_buffer(F->cb, br.buffer, br.offset, vk_index_type(static_cast<int>(p.type)));
        vkCmdDrawIndexed(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    P.draws++;
    P.verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    if (g_trace) trace_snap_after_draw();
}

// The bytes at a GPU address as the CPU sees them, when they are current: inside a pool, and not a range a submission that has not completed
// may write (gw_settled). Null otherwise. For decisions that need data the GPU would otherwise hold (indirect arguments, draw counts).
// Compute results held in the device copy ([nvn] compute_out_lazy) are newer than the pool: they are fetched back first (out_fetch: a
// completed submission's result, or one whose copy-back is recorded but has not run); one the submission being recorded writes cannot be
// read (null). Without the fetch an unguarded held result would be read stale, and a guarded one through a fault.
const uint8_t* cpu_view(uint64_t addr, uint64_t size) {
    if (!addr || !size) return nullptr;
    const PoolHit hit = pool_lookup(addr);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + size > hit.pool->size) return nullptr;
    if (!gw_settled(addr, size)) return nullptr;
    if (out_any()) {
        out_fetch(addr, size, false);
        if (out_overlaps(addr, size)) return nullptr;
    }
    return host_ptr(addr);
}

// ---- indirect quads on the GPU ([nvn] indirect_quads_gpu, default on) ----------------------------------------------------------
// Based on pokopia-native (MIT) windows/src/nxrt/vk/vk_replay.c + shaders/quad_args.comp @27a6df0 (quad_args).
// A single non-indexed indirect quad draw whose arguments a command earlier in the same submission writes (particle effects: a compute
// pass counts the live quads) cannot be converted by the CPU (cpu_view: null) and was skipped (#120). A one-thread dispatch
// (nvn_quad_args_shader.inc) turns the arguments into those of an indexed indirect draw over a shared quad index list (0,1,2, 0,2,3, ...
// for 65536 quads, made once), outside the pass: begin_draw reopens it, loading what the targets hold. Counts are clamped to 65536 quads
// and 65536 instances, so a count the GPU has not written yet cannot keep it busy past the OS timeout.
#include "nvn_quad_args_shader.inc"
constexpr uint32_t kQuadMax = 65536;
QuadArgs g_qa;

static bool indirect_quads_gpu_on() {
    static const bool on = nxrt::config_get_bool("nvn", "indirect_quads_gpu", true);
    return on;
}

static bool quad_args_init() {
    QuadArgs& q = g_qa;
    if (q.tried) return q.ok;
    q.tried = true;
    VkDevice d = vkdev();
    q.ssbo_align = std::max<VkDeviceSize>(16, device().properties().limits.minStorageBufferOffsetAlignment);
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &q.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kQuadArgsComp;
    mi.pCode = kQuadArgsComp;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &q.set_layout) != VK_SUCCESS || vkCreatePipelineLayout(d, &pli, nullptr, &q.layout) != VK_SUCCESS ||
        vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) {
        NXLOG_W("nvn.replay", "indirect quads: the argument shader could not be made: those draws take the CPU path");
        return false;
    }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = q.layout;
    const VkResult pr = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &q.pipe);
    vkDestroyShaderModule(d, m, nullptr);
    if (pr != VK_SUCCESS ||
        device().create_buffer(static_cast<VkDeviceSize>(kQuadMax) * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               gfx::MemoryUse::GpuOnly, &q.index, "nvn quad indices") != VK_SUCCESS) {
        q.pipe = pr == VK_SUCCESS ? q.pipe : VK_NULL_HANDLE;
        NXLOG_W("nvn.replay", "indirect quads: the argument pipeline or the quad index buffer could not be made: those draws take the CPU path");
        return false;
    }
    q.ok = true;
    NXLOG_I("nvn.replay", "indirect quads whose arguments the GPU writes: made into an indexed indirect draw on the GPU ([nvn] indirect_quads_gpu)");
    return true;
}

// Records the dispatch that makes the indexed draw's 20 bytes of arguments from the 16 at p.indirect_address (ending the pass); *out = where
// they are. False: nothing recorded (the draw takes the CPU path).
static bool quad_args_gpu(const P_DrawIndirect& p, BufRef* out) {
    if (!quad_args_init()) return false;
    QuadArgs& q = g_qa;
    const BufRef src = held_or_pool(p.indirect_address, 16, kOutIndirect, false);   // a held compute result: the copy (out_before_op)
    if (!src.ok || src.offset % 4) return false;
    const VkDeviceSize base = src.offset / q.ssbo_align * q.ssbo_align;
    const UploadAlloc dst = F->upload(20, q.ssbo_align);
    if (!dst) return false;
    const VkDescriptorSet set = F->alloc_set(q.set_layout);
    if (!set) return false;
    const VkDescriptorBufferInfo bi[2] = {{src.buffer, base, src.offset - base + 16}, {dst.buffer, dst.offset, 20}};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vkdev(), 2, w, 0, nullptr);
    end_pass();
    if (!q.index_filled) {   // the shared quad index list, once (copied from this submission's staging)
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(kQuadMax) * 6 * 4;
        const UploadAlloc st = F->staging(bytes);
        if (!st) return false;
        uint32_t* ix = reinterpret_cast<uint32_t*>(st.ptr);
        for (uint32_t k = 0; k < kQuadMax; ++k) {
            const uint32_t v = k * 4;
            uint32_t* t = ix + static_cast<size_t>(k) * 6;
            t[0] = v; t[1] = v + 1; t[2] = v + 2; t[3] = v; t[4] = v + 2; t[5] = v + 3;
        }
        const VkBufferCopy r = {st.offset, 0, bytes};
        vkCmdCopyBuffer(F->cb, st.buffer, q.index.buffer, 1, &r);
        q.index_filled = true;
    }
    memory_barrier(F->cb);   // the GPU's own write of the arguments earlier in this submission (and the index copy) before the dispatch
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, q.pipe);
    vkCmdBindDescriptorSets(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, q.layout, 0, 1, &set, 0, nullptr);
    const uint32_t at = static_cast<uint32_t>((src.offset - base) / 4);
    vkCmdPushConstants(F->cb, q.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &at);
    vkCmdDispatch(F->cb, 1, 1, 1);
    memory_barrier(F->cb);   // the dispatch's write before the indirect read
    g_cmd.bound_pipeline = VK_NULL_HANDLE;
    out->buffer = dst.buffer;
    out->offset = dst.offset;
    out->ok = true;
    return true;
}

// An indirect draw Vulkan cannot take as it is (quads, quad strips, polygons and line loops have no topology; 8-bit indices need
// VK_EXT_index_type_uint8): each draw's arguments (and its indices) are read by the CPU and the draw is made as a direct indexed draw of
// the converted 32-bit index list, like DrawArrays / DrawElements. That needs the arguments in memory the CPU can read: arguments a compute
// shader or a copy wrote and no completed submission has handed back are not (skipped, logged). A multi-draw reads its count the same way.
static void draw_indirect_cpu(const P_DrawIndirect& p, PrimConversion conv, uint32_t isz, bool indexed) {
    static bool noted = false;
    if (!noted) {
        noted = true;
        NXLOG_I("nvn.replay", "replay: indirect draw by CPU index conversion (first): mode %u, %s, index size %u, %s", p.mode, indexed ? "indexed" : "arrays", isz,
                p.multi ? "multi-draw" : "single");
    }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t stride = p.stride > 0 ? static_cast<uint64_t>(p.stride) : cmd_size;
    uint32_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = cpu_view(p.count_address, 4);
        if (!c) { note_unsupported("indirect draw conversion: the draw count is not readable by the CPU (written by the GPU)"); counters().draws_skipped.fetch_add(1); return; }
        std::memcpy(&n, c, 4);
        n = std::min(n, p.max_count);
    }
    bool drew = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = cpu_view(p.indirect_address + static_cast<uint64_t>(i) * stride, cmd_size);
        if (!a) { note_unsupported("indirect draw conversion: the arguments are not readable by the CPU (written by the GPU)"); break; }
        uint32_t v[5] = {};
        std::memcpy(v, a, cmd_size);
        const uint32_t count = v[0], instances = v[1];
        if (!count || !instances) continue;
        if (!indirect_args_plausible(count, instances)) { note_unsupported("indirect draw conversion: implausible arguments (stale guest memory?)"); continue; }
        int32_t base_vertex = 0;
        uint32_t base_instance = v[3];
        const uint8_t* src = nullptr;
        if (indexed) {
            base_vertex = static_cast<int32_t>(v[3]);
            base_instance = v[4];
            src = cpu_view(p.index_address + static_cast<uint64_t>(v[2]) * isz, static_cast<uint64_t>(count) * isz);
            if (!src) { note_unsupported("indirect draw conversion: the indices are not readable by the CPU (written by the GPU)"); break; }
        }
        count_primitives(p.mode, count, instances);
        const uint32_t first = v[2];
        const uint64_t m = src ? bind_prim_indices(conv, count, [src, isz](uint32_t k) { return read_index(src, isz, k); })
                               : bind_prim_indices(conv, count, [first](uint32_t k) { return first + k; });
        if (!m) continue;
        checkpoint_draw(count, instances, 0, false);   // the CPU read the arguments: a direct draw
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(m), instances, 0, base_vertex, base_instance);
        drew = true;
    }
    if (drew) {
        counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
        P.draws++;
    } else {
        counters().draws_skipped.fetch_add(1);
    }
    if (g_trace) trace_snap_after_draw();
}

void do_draw_indirect(const P_DrawIndirect& p) {
    PoolReasonScope pr_(kReasonIndirect);
    const bool indexed = p.type != 0xffffffffu;
    // the base instance is in the arguments, which the GPU may write and a multi-draw holds one of per draw: the vertex shaders read it
    // as Vulkan's BaseInstance (spirv_base_instance_from_c0), so the c[0] word is not needed and stays 0
    g_sets.draw_base_instance = 0;
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    // indirect quads whose arguments the CPU cannot read (the GPU writes them): the indexed arguments made on the GPU, before the pass opens
    BufRef qa{};
    if (conv == PrimConversion::Quads && !indexed && !p.multi && indirect_quads_gpu_on() && S.render_enable && S.stage[kVertex] &&
        !cpu_view(p.indirect_address, 16))
        quad_args_gpu(p, &qa);
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), indexed, indexed ? static_cast<int>(p.type) : 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (qa.ok) {
        bind_index_buffer(F->cb, g_qa.index.buffer, 0, VK_INDEX_TYPE_UINT32);
        checkpoint_draw(static_cast<uint32_t>(p.indirect_address), static_cast<uint32_t>(p.indirect_address >> 32), 1, true);
        vkCmdDrawIndexedIndirect(F->cb, qa.buffer, qa.offset, 1, 20);
        const uint64_t n = ++g_qa.draws;
        if ((n & (n - 1)) == 0)   // 1, 2, 4, 8, ...
            NXLOG_I("nvn.replay", "indirect quads drawn with GPU-made arguments: %llu so far", static_cast<unsigned long long>(n));
        counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
        P.draws++;
        if (g_trace) trace_snap_after_draw();
        return;
    }
    const uint32_t isz = indexed ? index_size_bytes(static_cast<int>(p.type)) : 0;
    if (conv != PrimConversion::None || (indexed && isz == 1 && !device().features().index_type_uint8)) {
        draw_indirect_cpu(p, conv, isz, indexed);
        return;
    }
    // GPU path. The primitives of these draws are not known to the CPU: the primitive counters see them through a query window (ResetCounter).
    static bool noted_indirect = false, noted_u8 = false;
    if (!noted_indirect) { noted_indirect = true; NXLOG_I("nvn.replay", "replay: indirect draw (first): mode %u, %s%s", p.mode, indexed ? "indexed" : "arrays", p.multi ? ", multi-draw" : ""); }
    if (indexed && isz == 1 && !noted_u8) { noted_u8 = true; NXLOG_I("nvn.replay", "replay: indirect draw with 8-bit indices (first): VK_EXT_index_type_uint8"); }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t n_cmds = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
    BufRef ib = held_or_pool(p.indirect_address, (p.stride ? static_cast<uint64_t>(p.stride) : cmd_size) * (n_cmds - 1) + cmd_size, kOutIndirect, false);
    if (indexed) {
        BufRef xb = resolve_buffer(p.index_address, 4);
        if (pool_copy_on()) pool_copy_touch(p.index_address, indirect_index_span(p));   // [nvn] pool_copy: every index the draw may read
        bind_index_buffer(F->cb, xb.buffer, xb.offset, vk_index_type(static_cast<int>(p.type)));
    }
    checkpoint_draw(static_cast<uint32_t>(p.indirect_address), static_cast<uint32_t>(p.indirect_address >> 32), static_cast<uint32_t>(n_cmds), true);
    if (p.multi && p.count_address) {
        BufRef cb = held_or_pool(p.count_address, 4, kOutIndirect, false);
        const uint32_t stride = static_cast<uint32_t>(p.stride ? p.stride : cmd_size);
        if (indexed) vkCmdDrawIndexedIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
        else vkCmdDrawIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
    } else if (indexed) {
        vkCmdDrawIndexedIndirect(F->cb, ib.buffer, ib.offset, 1, 20);
    } else {
        vkCmdDrawIndirect(F->cb, ib.buffer, ib.offset, 1, 16);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    P.draws++;
    if (g_trace) trace_snap_after_draw();
}

// DrawTransformFeedback(mode, control): draws what the feedback section begun with this control buffer wrote, as many vertices as it wrote
// (vkCmdDrawIndirectByteCountEXT over the section's byte counter, divided by the stride of the vertex stream that reads the feedback buffer). The game
// binds the feedback buffer as an ordinary vertex buffer. The feedback writes are ordered before the vertex fetch by ending the pass first.
void do_draw_xfb(const P_DrawXfb& p) {
    static bool noted = false;
    if (!noted) { noted = true; NXLOG_I("nvn.xfb", "replay: transform feedback draw (first): mode %u control %llx", p.mode, static_cast<unsigned long long>(p.control)); }
    if (!S.render_enable) return;
    if (!device().features().transform_feedback) { note_unsupported("DrawTransformFeedback (the device has no VK_EXT_transform_feedback)"); counters().draws_skipped.fetch_add(1); return; }
    const auto it = g_xfb.slot_of.find(p.control);
    if (!g_xfb.counter || it == g_xfb.slot_of.end() || !g_xfb.slot_written[it->second]) {
        TRACE("DrawTransformFeedback control %llx: the section wrote nothing", static_cast<unsigned long long>(p.control));   // zero bytes: zero vertices
        counters().draws_skipped.fetch_add(1);
        return;
    }
    if (prim_conversion(static_cast<int>(p.mode)) != PrimConversion::None) { note_unsupported("DrawTransformFeedback of a quad/polygon/line-loop primitive"); return; }
    const uint32_t slot = it->second;
    end_pass();
    memory_barrier(F->cb);   // the section's writes (feedback buffer, counter) before the vertex fetch and the indirect read
    g_sets.draw_base_instance = 0;   // vkCmdDrawIndirectByteCountEXT draws from instance 0
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    uint32_t stride = g_key_attr_count ? S.stream[g_key_binding[0]].stride : 0;
    for (uint32_t b = 0; !stride && b < 16; ++b) stride = S.stream[b].stride;
    if (!stride) { note_unsupported("DrawTransformFeedback with no vertex stream stride"); counters().draws_skipped.fetch_add(1); return; }
    checkpoint_draw(static_cast<uint32_t>(p.control), static_cast<uint32_t>(p.control >> 32), 1, true);   // the count is the section's byte counter
    vkCmdDrawIndirectByteCountEXT(F->cb, 1, 0, g_xfb.counter.buffer, static_cast<VkDeviceSize>(slot) * kXfbSlotBytes, 0, stride);
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    P.draws++;
    if (g_trace) trace_snap_after_draw();
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
