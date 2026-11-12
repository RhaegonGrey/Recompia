// SPDX-License-Identifier: GPL-3.0-or-later
// Output pass (output_pass.h).
#include "extras/output_pass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "extras/vk_make.h"
#include "nxrt/log.h"
#include "vk_gauges.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "outpass";

#include "extras/extras_shaders.inc"

struct EncodePush {
    float pic[4];     // picture rectangle in destination UV: x, y, w, h
    float crop[4];    // source crop in texels: x, y, w, h
    float src[4];     // whole source size, z = flip
    float p[4];       // x = mode, y = paper white nits, z = peak nits, w = 1: apply the picture controls
    float adj[4];     // picture controls: contrast, saturation, black, gamma
    float cm[12];     // colour matrix rows (3 vec4), used when src[3] = 1
};
static_assert(sizeof(EncodePush) == 128, "the encode push block fits the 128 bytes every Vulkan device offers");
}  // namespace

bool OutputPass::init(gfx::Device& dev) {
    if (dev_) return true;
    dev_ = &dev;
    VkDevice d = dev.device();
    auto fail = [&](const char* what) {
        NXLOG_E(kTag, "%s failed", what);
        shutdown();
        return false;
    };

    if (!make_clamp_sampler(d, VK_FILTER_LINEAR, &linear_)) return fail("linear sampler");
    if (!make_clamp_sampler(d, VK_FILTER_NEAREST, &nearest_)) return fail("nearest sampler");

    auto set_layout = [&](std::initializer_list<VkDescriptorSetLayoutBinding> b, VkDescriptorSetLayout* out) {
        std::vector<VkDescriptorSetLayoutBinding> v(b);
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = static_cast<uint32_t>(v.size());
        li.pBindings = v.data();
        const bool ok = vkCreateDescriptorSetLayout(d, &li, nullptr, out) == VK_SUCCESS;
        if (ok) vk_gauge_descriptor_set_layouts().add(1);
        return ok;
    };
    auto pipe_layout = [&](VkDescriptorSetLayout sl, VkShaderStageFlags stages, uint32_t push, VkPipelineLayout* out) {
        VkPushConstantRange pcr = {stages, 0, push};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &sl;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pcr;
        const bool ok = vkCreatePipelineLayout(d, &li, nullptr, out) == VK_SUCCESS;
        if (ok) vk_gauge_pipeline_layouts().add(1);
        return ok;
    };
    if (!set_layout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}, &enc_set_layout_)) return fail("encode set layout");
    if (!pipe_layout(enc_set_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(EncodePush), &enc_layout_)) return fail("encode pipeline layout");

    if (!make_sampler_pool(d, kSetRing, &pool_)) return fail("descriptor pool");
    {
        VkDescriptorSetLayout ls[kSetRing];
        for (auto& x : ls) x = enc_set_layout_;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = kSetRing;
        ai.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(d, &ai, enc_sets_) != VK_SUCCESS) return fail("descriptor sets");
        vk_gauge_descriptor_sets().add(kSetRing);   // shutdown subtracts them only while enc_sets_[0] is set
    }

    vs_ = make_module(d, kOutVert, sizeof kOutVert);
    fs_ = make_module(d, kOutEncodeFrag, sizeof kOutEncodeFrag);
    if (!vs_ || !fs_) return fail("encode shader modules");

    if (!fsr_.init(dev)) NXLOG_W(kTag, "FSR 1 is not available (its pipelines failed): the picture is scaled with the plain filter");
    return true;
}

void OutputPass::shutdown() {
    if (!dev_) return;
    VkDevice d = dev_->device();
    dev_->wait_idle();
    fsr_.shutdown();
    nis_.shutdown();
    nis_tried_ = false;
    for (auto& [f, p] : enc_pipes_)
        if (p) { vkDestroyPipeline(d, p, nullptr); vk_gauge_pipelines().sub(1); }
    enc_pipes_.clear();
    if (enc_layout_) { vkDestroyPipelineLayout(d, enc_layout_, nullptr); vk_gauge_pipeline_layouts().sub(1); }
    if (pool_) { vkDestroyDescriptorPool(d, pool_, nullptr); vk_gauge_descriptor_pools().sub(1); }
    if (enc_sets_[0]) vk_gauge_descriptor_sets().sub(kSetRing);
    if (enc_set_layout_) { vkDestroyDescriptorSetLayout(d, enc_set_layout_, nullptr); vk_gauge_descriptor_set_layouts().sub(1); }
    if (vs_) { vkDestroyShaderModule(d, vs_, nullptr); vk_gauge_shader_modules().sub(1); }
    if (fs_) { vkDestroyShaderModule(d, fs_, nullptr); vk_gauge_shader_modules().sub(1); }
    if (linear_) { vkDestroySampler(d, linear_, nullptr); vk_gauge_samplers().sub(1); }
    if (nearest_) { vkDestroySampler(d, nearest_, nullptr); vk_gauge_samplers().sub(1); }
    enc_layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    enc_set_layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    linear_ = nearest_ = VK_NULL_HANDLE;
    std::fill(std::begin(enc_sets_), std::end(enc_sets_), VK_NULL_HANDLE);
    next_enc_ = 0;
    dev_ = nullptr;
}

VkPipeline OutputPass::encode_pipeline(VkFormat fmt) {
    auto it = enc_pipes_.find(fmt);
    if (it != enc_pipes_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pri.colorAttachmentCount = 1;
    pri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = enc_layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_->device(), VK_NULL_HANDLE, 1, &gpi, nullptr, &p) != VK_SUCCESS) {
        NXLOG_E(kTag, "encode pipeline for format %d failed", static_cast<int>(fmt));
        p = VK_NULL_HANDLE;
    }
    if (p) vk_gauge_pipelines().add(1);
    enc_pipes_[fmt] = p;
    return p;
}

void OutputPass::encode(VkCommandBuffer cb, VkImageView dst_view, VkFormat dst_format, VkExtent2D dst_extent, VkImageView src, VkExtent2D src_extent, const int32_t crop_in[4],
                        bool keep_aspect, bool flip_y, int mode, float paper, float peak, VkImageLayout src_layout, const float* pic, const float* cm) {
    VkPipeline pipe = encode_pipeline(dst_format);
    if (!pipe) return;
    const int32_t sw = static_cast<int32_t>(src_extent.width), sh = static_cast<int32_t>(src_extent.height);
    double cx = 0, cy = 0, cw = sw, ch = sh;
    if (crop_in && crop_in[2] > 0 && crop_in[3] > 0) {
        const int32_t x0 = std::clamp(crop_in[0], 0, sw - 1), y0 = std::clamp(crop_in[1], 0, sh - 1);
        const int32_t x1 = std::clamp(crop_in[0] + crop_in[2], x0 + 1, sw), y1 = std::clamp(crop_in[1] + crop_in[3], y0 + 1, sh);
        cx = x0;
        cy = y0;
        cw = x1 - x0;
        ch = y1 - y0;
    }
    const double dw = dst_extent.width, dh = dst_extent.height;
    double px = 0, py = 0, pw = dw, ph = dh;
    if (keep_aspect) {
        const double a = cw / ch, b = dw / dh;
        if (a > b) {
            pw = dw;
            ph = dw / a;
        } else {
            ph = dh;
            pw = dh * a;
        }
        px = (dw - pw) * 0.5;
        py = (dh - ph) * 0.5;
    }
    const bool nearest = (std::fabs(pw - cw) < 1e-9 && std::fabs(ph - ch) < 1e-9) || upscale_mode() == UpscaleMode::Nearest;

    EncodePush pc = {};
    pc.pic[0] = static_cast<float>(px / dw);
    pc.pic[1] = static_cast<float>(py / dh);
    pc.pic[2] = static_cast<float>(pw / dw);
    pc.pic[3] = static_cast<float>(ph / dh);
    pc.crop[0] = static_cast<float>(cx);
    pc.crop[1] = static_cast<float>(cy);
    pc.crop[2] = static_cast<float>(cw);
    pc.crop[3] = static_cast<float>(ch);
    pc.src[0] = static_cast<float>(sw);
    pc.src[1] = static_cast<float>(sh);
    pc.src[2] = flip_y ? 1.0f : 0.0f;
    pc.src[3] = cm ? 1.0f : 0.0f;
    if (cm) std::memcpy(pc.cm, cm, sizeof pc.cm);
    pc.p[0] = static_cast<float>(mode);
    pc.p[1] = paper;
    pc.p[2] = peak;
    pc.p[3] = pic ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) pc.adj[i] = pic ? pic[i] : (i == 2 ? 0.0f : 1.0f);

    VkDescriptorSet set = enc_sets_[next_enc_];
    next_enc_ = (next_enc_ + 1) % kSetRing;
    VkDescriptorImageInfo dii = {nearest ? nearest_ : linear_, src, src_layout};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_->device(), 1, &w, 0, nullptr);

    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = dst_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = keep_aspect ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, dst_extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, enc_layout_, 0, 1, &set, 0, nullptr);
    const VkViewport vpt = {0, 0, static_cast<float>(dw), static_cast<float>(dh), 0, 1};
    const VkRect2D scissor = {{0, 0}, dst_extent};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdPushConstants(cb, enc_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

int OutputPass::record(VkCommandBuffer cb, VkImageView dst_view, VkFormat dst_format, VkExtent2D dst_extent, const OutputParams& p) {
    if (!dev_ || !dst_extent.width || !dst_extent.height || !p.src_extent.width || !p.src_extent.height) return -1;
    int mode = 0;
    switch (p.out) {
        case OutputKind::Sdr8: mode = p.src_linear ? 1 : 0; break;
        case OutputKind::Hdr10: mode = 2; break;
        case OutputKind::ScRgb: mode = 3; break;
    }

    const float* pic = p.picture ? p.pic : nullptr;
    const float* cm = p.color_matrix ? p.cm : nullptr;

    // FSR 1: only when the picture on screen is larger than the source crop and the crop starts at the origin
    const int32_t sw = static_cast<int32_t>(p.src_extent.width), sh = static_cast<int32_t>(p.src_extent.height);
    const bool has_crop = p.crop[2] > 0 && p.crop[3] > 0;
    const double cw = has_crop ? std::min(p.crop[2], sw) : sw, ch = has_crop ? std::min(p.crop[3], sh) : sh;
    double pw = dst_extent.width, ph = dst_extent.height;
    if (p.keep_aspect) {
        const double a = cw / ch, b = static_cast<double>(dst_extent.width) / dst_extent.height;
        if (a > b) ph = pw / a;
        else pw = ph * a;
    }
    const uint32_t upw = std::max(1u, static_cast<uint32_t>(std::lround(pw))), uph = std::max(1u, static_cast<uint32_t>(std::lround(ph)));
    const bool origin_ok = !has_crop || (p.crop[0] == 0 && p.crop[1] == 0);
    if ((p.fsr1 || p.nis) && origin_ok && (upw > cw + 0.5 || uph > ch + 0.5)) {
        const VkExtent2D in_view = {static_cast<uint32_t>(cw), static_cast<uint32_t>(ch)};
        const VkExtent2D out = {upw, uph};
        if (p.nis && !nis_tried_) {
            nis_tried_ = true;
            if (!nis_.init(*dev_)) NXLOG_W(kTag, "NVIDIA Image Scaling is not available (its pipeline failed): FSR 1 is used instead");
        }
        SpatialUpscaler* su = p.nis && nis_.initialised() && nis_.supports(in_view, out) ? static_cast<SpatialUpscaler*>(&nis_)
                              : fsr_.initialised()                                   ? static_cast<SpatialUpscaler*>(&fsr_)
                                                                                     : nullptr;
        // display-referred picture: the HDR window texture is 1.0 = paper white, so no pre-scale; SDR bytes are already [0,1]
        if (VkImageView up = su ? su->upscale(cb, p.src, in_view, p.src_extent, out, p.src_linear, 1.0f, p.sharpness) : VK_NULL_HANDLE) {
            const int32_t whole[4] = {0, 0, 0, 0};
            encode(cb, dst_view, dst_format, dst_extent, up, VkExtent2D{upw, uph}, whole, p.keep_aspect, p.flip_y, mode, p.paper_white_nits, p.peak_nits, VK_IMAGE_LAYOUT_GENERAL, pic, cm);
            return mode;
        }
    }
    encode(cb, dst_view, dst_format, dst_extent, p.src, p.src_extent, p.crop, p.keep_aspect, p.flip_y, mode, p.paper_white_nits, p.peak_nits, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pic, cm);
    return mode;
}

}  // namespace nxrt::extras
