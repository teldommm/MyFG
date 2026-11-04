#pragma once
// Собственный Vulkan-контекст модуля (инстанс + устройство + одна очередь) и помощники для
// работы с AHardwareBuffer. Всё живёт в процессе игры и не пересекается с Vulkan'ом самой игры:
// обмен кадрами идёт только через AHardwareBuffer + sync_fd.
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#define MYFG_VK_FNS(X)                                                                         \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)              \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties)             \
    X(GetPhysicalDeviceFeatures) X(EnumerateDeviceExtensionProperties) X(CreateDevice)         \
    X(GetDeviceProcAddr) X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle)                  \
    X(CreateImage) X(DestroyImage) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)          \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)                       \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(QueueSubmit)             \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences)                             \
    X(CreateSemaphore) X(DestroySemaphore) X(CmdPipelineBarrier) X(CmdBlitImage)                 \
    X(GetPhysicalDeviceFeatures2) X(GetImageMemoryRequirements) X(CmdCopyImage)                \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler)                  \
    X(CreateSamplerYcbcrConversion) X(DestroySamplerYcbcrConversion)                           \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout)         \
    X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule)                      \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool)                       \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets)                   \
    X(UpdateDescriptorSets) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants)    \
    X(CmdDispatch) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)             \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CmdCopyImageToBuffer)                    \
    X(InvalidateMappedMemoryRanges) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool)  \
    X(CmdWriteTimestamp) X(GetQueryPoolResults) X(GetPhysicalDeviceFormatProperties)

struct VkFns {
#define X(n) PFN_vk##n n = nullptr;
    MYFG_VK_FNS(X)
#undef X
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAndroidHardwareBufferPropertiesANDROID = nullptr;
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR = nullptr;
    PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR = nullptr;
};

// Образ, живущий поверх AHardwareBuffer.
struct AhbImage {
    AHardwareBuffer* ahb = nullptr;
    bool owns_ahb = false;          // true — буфер выделили мы (иначе только ссылка acquire)
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    uint64_t last_use = 0;          // для LRU-кэша
    // Буфер с внешним форматом (vkFormat=UNDEFINED): читается только через сэмплер с YCbCr-конверсией.
    bool external = false;
    VkImageView view = VK_NULL_HANDLE;
};

// Обычный (не AHB) storage-образ (RGBA8 или A2B10G10R10): приёмник compute-копии внешнего формата и HDR-прохода.
struct PlainImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};

class VkCtx {
public:
    bool init();                    // false -> модуль остаётся неактивным
    void destroy();

    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkFns f;
    bool can_export_syncfd = false; // VK_KHR_external_semaphore_fd доступно
    bool ext_formats = false;       // shaderStorageImageExtendedFormats включена (нужна для 10-битного HDR-прохода)

    // Импорт чужого AHB (буфер игры). Образ с usage TRANSFER_SRC. false — формат не подходит.
    bool importAhb(AHardwareBuffer* ahb, VkImageUsageFlags usage, AhbImage* out);
    // Свой AHB + образ (кольца реальных/сгенерированных кадров). По умолчанию RGBA8; ten_bit — R10G10B10A2 (HDR-кольцо).
    bool createRingImage(uint32_t w, uint32_t h, VkImageUsageFlags usage, AhbImage* out, bool ten_bit = false);
    void destroyImage(AhbImage* im);

    // Внешние форматы (vendor/UBWC): compute-копия crop-области в PlainImage (RGBA8, GENERAL).
    bool can_ycbcr = false;
    bool createPlainImage(uint32_t w, uint32_t h, PlainImage* out, VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM);
    // Поддерживает ли формат storage-запись (optimal tiling).
    bool formatHasStorage(VkFormat fmt);
    void destroyPlainImage(PlainImage* im);
    // Записывает: обновление дескриптора (slot 0..7, по номеру кадра в полёте) + dispatch. src уже в SHADER_READ_ONLY_OPTIMAL, dst — в GENERAL.
    void recordExtCopy(VkCommandBuffer cb, const AhbImage& src, const PlainImage& dst, const int32_t crop[4], int slot);

    // Семафор -> sync_fd после submit; -1 при ошибке.
    int exportSyncFd(VkSemaphore sem);
    // sync_fd игры -> временный wait-семафор (fd забирается при успехе).
    bool importSyncFd(VkSemaphore sem, int fd);

    VkSemaphore createSemaphore(bool exportable);

    // Подбор типа памяти: первый, у которого есть все биты want. false — такого нет.
    bool findMemType(uint32_t bits, VkMemoryPropertyFlags want, uint32_t* idx);
    bool can_ts = false;            // timestamp-запросы доступны на очереди
    double ts_ns_per_tick = 0.0;
    uint64_t ts_mask = 0;
    bool queue_high_prio = false;   // очередь создана с высоким глобальным приоритетом (queue_prio)

private:
    struct ExtConv {
        uint64_t ext_fmt = 0;
        VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
        VkSampler samp = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkDescriptorSet dsets[8] = {};
    } ext_;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    bool ensureExtConv(const VkAndroidHardwareBufferFormatPropertiesANDROID& fp);
    void destroyExtConv();
    bool pickMemType(uint32_t bits, uint32_t* idx);
    bool pickLocalMemType(uint32_t bits, uint32_t* idx);
    void* lib_ = nullptr;
    VkPhysicalDeviceMemoryProperties memprops_{};
};
