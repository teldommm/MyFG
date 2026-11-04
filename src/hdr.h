#pragma once
// SDR -> HDR10 (BT.2020 + PQ): один compute-проход на выходной кадр (реальный и сгенерированный).
// RGBA8-образ (sampled) -> storage-образ того же размера (A2B10G10R10 или, если нельзя, RGBA8 с дизерингом).
#include "vk_ctx.h"

struct HdrParams {
    float paper_white;   // нит, до которых поднимается белый SDR (hdr_pw)
    float peak;          // пик панели, нит (hdr_peak или авто)
    float knee;          // 0..1, с какой яркости начинается растяжение светов (hdr_knee/100)
    float boost;         // 0..1, доля запаса пик/paper white, на которую растягивается белый (hdr_boost/100)
    float gamma;         // 0 = sRGB, иначе степень (hdr_gamma/10)
    float contrast;      // 0.5..1.5 (hdr_ct/100)
    float saturation;    // 0.5..1.5 (hdr_sat/100)
    float seed;          // для дизеринга, меняется по кадрам
};

class HdrPass {
public:
    static constexpr int kSets = 24;                 // kFrames(3) * 8 проходов на кадр
    // ten_bit — выбирает вариант шейдера и формат storage-образа. false -> проход недоступен (причина в логе).
    bool init(VkCtx& vk, bool ten_bit);
    void destroy(VkCtx& vk);
    bool ready() const { return pipe_ != VK_NULL_HANDLE; }
    bool tenBit() const { return ten_; }
    VkFormat storageFormat() const { return ten_ ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM; }
    // Вид на образ-источник (RGBA8) или на hdr-кольцо (fmt = формат прохода).
    VkImageView makeView(VkCtx& vk, VkImage img, VkFormat fmt);
    // set — индекс дескриптора (0..kSets-1), уникальный среди кадров в полёте. src и dst в GENERAL.
    void record(VkCtx& vk, VkCommandBuffer cb, int set, VkImageView src, VkImageView dst,
                uint32_t w, uint32_t h, const HdrParams& p);
private:
    bool ten_ = false;
    VkSampler samp_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kSets] = {};
};
