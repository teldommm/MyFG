#include "hdr.h"
#include "common.h"
#include "hdr_spv.h"

bool HdrPass::init(VkCtx& vk, bool ten_bit) {
    if (pipe_) return true;
    ten_ = ten_bit;
    auto& f = vk.f;
    VkDevice dev = vk.dev;
    VkSamplerCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;          // шейдер читает texelFetch, фильтрация не нужна
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = 0.0f;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkResult r = f.CreateSampler(dev, &sc, nullptr, &samp_);
    if (r != VK_SUCCESS) { LOGW("hdr: sampler failed %d", (int)r); destroy(vk); return false; }

    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0].binding = 0; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb[0].descriptorCount = 1;
    lb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; lb[0].pImmutableSamplers = &samp_;
    lb[1].binding = 1; lb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; lb[1].descriptorCount = 1;
    lb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 2; dl.pBindings = lb;
    if (f.CreateDescriptorSetLayout(dev, &dl, nullptr, &dsl_) != VK_SUCCESS) { LOGW("hdr: set layout failed"); destroy(vk); return false; }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 40};   // 2 x vec4 + uvec2
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl_;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (f.CreatePipelineLayout(dev, &pli, nullptr, &pl_) != VK_SUCCESS) { LOGW("hdr: pipeline layout failed"); destroy(vk); return false; }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = ten_ ? sizeof kHdrSpv10 : sizeof kHdrSpv8;
    smi.pCode = ten_ ? kHdrSpv10 : kHdrSpv8;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (f.CreateShaderModule(dev, &smi, nullptr, &sm) != VK_SUCCESS) { LOGW("hdr: shader module failed"); destroy(vk); return false; }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm; cpi.stage.pName = "main";
    cpi.layout = pl_;
    r = f.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe_);
    f.DestroyShaderModule(dev, sm, nullptr);
    if (r != VK_SUCCESS) { pipe_ = VK_NULL_HANDLE; LOGW("hdr: compute pipeline failed %d", (int)r); destroy(vk); return false; }

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets}};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = kSets; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
    if (f.CreateDescriptorPool(dev, &dp, nullptr, &pool_) != VK_SUCCESS) { LOGW("hdr: descriptor pool failed"); destroy(vk); return false; }
    VkDescriptorSetLayout ls[kSets];
    for (auto& l : ls) l = dsl_;
    VkDescriptorSetAllocateInfo da{};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = pool_; da.descriptorSetCount = kSets; da.pSetLayouts = ls;
    if (f.AllocateDescriptorSets(dev, &da, sets_) != VK_SUCCESS) { LOGW("hdr: descriptor sets failed"); destroy(vk); return false; }
    LOGD("hdr: pass ready (%s)", ten_ ? "10-bit" : "8-bit + dither");
    return true;
}

void HdrPass::destroy(VkCtx& vk) {
    auto& f = vk.f;
    VkDevice dev = vk.dev;
    if (pool_) f.DestroyDescriptorPool(dev, pool_, nullptr);       // наборы освобождаются вместе с пулом
    if (pipe_) f.DestroyPipeline(dev, pipe_, nullptr);
    if (pl_) f.DestroyPipelineLayout(dev, pl_, nullptr);
    if (dsl_) f.DestroyDescriptorSetLayout(dev, dsl_, nullptr);
    if (samp_) f.DestroySampler(dev, samp_, nullptr);
    pool_ = VK_NULL_HANDLE; pipe_ = VK_NULL_HANDLE; pl_ = VK_NULL_HANDLE; dsl_ = VK_NULL_HANDLE; samp_ = VK_NULL_HANDLE;
    for (auto& s : sets_) s = VK_NULL_HANDLE;
}

VkImageView HdrPass::makeView(VkCtx& vk, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vk.f.CreateImageView(vk.dev, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

void HdrPass::record(VkCtx& vk, VkCommandBuffer cb, int set, VkImageView src, VkImageView dst,
                     uint32_t w, uint32_t h, const HdrParams& p) {
    auto& f = vk.f;
    VkDescriptorImageInfo si{samp_, src, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo di{VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2]{};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = sets_[set]; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &si;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[1].dstSet = sets_[set]; wr[1].dstBinding = 1; wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &di;
    f.UpdateDescriptorSets(vk.dev, 2, wr, 0, nullptr);

    struct PC { float a[4]; float b[4]; uint32_t sz[2]; } pc;
    pc.a[0] = p.paper_white; pc.a[1] = p.peak; pc.a[2] = p.knee; pc.a[3] = p.boost;
    pc.b[0] = p.gamma; pc.b[1] = p.contrast; pc.b[2] = p.saturation; pc.b[3] = p.seed;
    pc.sz[0] = w; pc.sz[1] = h;
    f.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    f.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_, 0, 1, &sets_[set], 0, nullptr);
    f.CmdPushConstants(cb, pl_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    f.CmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
}
