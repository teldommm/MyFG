#include "vk_ctx.h"
#include "common.h"
#include "config.h"
#include "ext_blit_spv.h"
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <vector>

bool VkCtx::init() {
    lib_ = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib_) { LOGE("vk: dlopen(libvulkan.so) failed"); return false; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib_, "vkGetInstanceProcAddr");
    if (!gipa) return false;

    auto createInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!createInstance) return false;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "myfg";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkResult r = createInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) { LOGE("vk: vkCreateInstance failed %d", (int)r); return false; }

    // Таблица через gipa(instance): loader отдаёт trampolines, работающие с любым VkDevice.
#define X(n) f.n = (PFN_vk##n)gipa(inst, "vk" #n);
    MYFG_VK_FNS(X)
#undef X
    if (!f.EnumeratePhysicalDevices || !f.CreateDevice || !f.QueueSubmit) { LOGE("vk: core entry points missing"); return false; }

    uint32_t n = 0;
    f.EnumeratePhysicalDevices(inst, &n, nullptr);
    if (!n) { LOGE("vk: no physical devices"); return false; }
    std::vector<VkPhysicalDevice> devs(n);
    f.EnumeratePhysicalDevices(inst, &n, devs.data());

    static const char* kRequired[] = {
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    const char* kSyncFd = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;

    for (uint32_t i = 0; i < n && phys == VK_NULL_HANDLE; i++) {
        VkPhysicalDeviceProperties pr;
        f.GetPhysicalDeviceProperties(devs[i], &pr);
        if (pr.apiVersion < VK_API_VERSION_1_1) continue;

        uint32_t ec = 0;
        f.EnumerateDeviceExtensionProperties(devs[i], nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> exts(ec);
        f.EnumerateDeviceExtensionProperties(devs[i], nullptr, &ec, exts.data());
        auto has = [&](const char* name) {
            for (auto& e : exts) if (!strcmp(e.extensionName, name)) return true;
            return false;
        };
        bool ok = true;
        for (const char* req : kRequired) if (!has(req)) { ok = false; LOGW("vk: %s lacks %s", pr.deviceName, req); }
        if (!ok) continue;

        uint32_t qc = 0;
        f.GetPhysicalDeviceQueueFamilyProperties(devs[i], &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qc);
        f.GetPhysicalDeviceQueueFamilyProperties(devs[i], &qc, qs.data());
        int fam = -1;
        for (uint32_t k = 0; k < qc; k++)
            if ((qs[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qs[k].queueFlags & VK_QUEUE_COMPUTE_BIT)) { fam = (int)k; break; }
        if (fam < 0) continue;

        std::vector<const char*> en(kRequired, kRequired + 2);
        bool sf = has(kSyncFd);
        if (sf) en.push_back(kSyncFd);

        VkPhysicalDeviceFeatures feats{}, want{};
        f.GetPhysicalDeviceFeatures(devs[i], &feats);
        want.shaderStorageImageExtendedFormats = feats.shaderStorageImageExtendedFormats;   // нужно шейдерам WFG

        // Сэмплер с YCbCr-конверсией: единственный способ прочитать буфер с внешним (вендорским) форматом
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycf{};
        ycf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        if (f.GetPhysicalDeviceFeatures2) {
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &ycf;
            f.GetPhysicalDeviceFeatures2(devs[i], &f2);
        }
        VkPhysicalDeviceSamplerYcbcrConversionFeatures ycen{};
        ycen.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        ycen.samplerYcbcrConversion = ycf.samplerYcbcrConversion;

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = (uint32_t)fam;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = ycf.samplerYcbcrConversion ? &ycen : nullptr;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = (uint32_t)en.size();
        dci.ppEnabledExtensionNames = en.data();
        dci.pEnabledFeatures = &want;

        // Высокий глобальный приоритет очереди (VK_KHR/EXT_global_priority): конвейер не должен ждать за
        // тяжёлыми шейдерами игры. Драйвер может отказать (нет прав) — тогда создаём как обычно.
        VkResult dr = VK_ERROR_INITIALIZATION_FAILED;
        queue_high_prio = false;
        const char* gpExt = has("VK_KHR_global_priority") ? "VK_KHR_global_priority"
                          : has("VK_EXT_global_priority") ? "VK_EXT_global_priority" : nullptr;
        if (g_cfg.queue_prio && gpExt) {
            VkDeviceQueueGlobalPriorityCreateInfoKHR gp{};
            gp.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR;
            gp.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
            VkDeviceQueueCreateInfo qi2 = qi;
            qi2.pNext = &gp;
            std::vector<const char*> en2 = en;
            en2.push_back(gpExt);
            VkDeviceCreateInfo dci2 = dci;
            dci2.pQueueCreateInfos = &qi2;
            dci2.enabledExtensionCount = (uint32_t)en2.size();
            dci2.ppEnabledExtensionNames = en2.data();
            dr = f.CreateDevice(devs[i], &dci2, nullptr, &dev);
            if (dr == VK_SUCCESS) queue_high_prio = true;
            else { LOGW("vk: high queue priority refused (%d), using default", (int)dr); dev = VK_NULL_HANDLE; }
        }
        if (!queue_high_prio) dr = f.CreateDevice(devs[i], &dci, nullptr, &dev);
        if (dr != VK_SUCCESS) { LOGW("vk: vkCreateDevice on %s failed %d", pr.deviceName, (int)dr); dev = VK_NULL_HANDLE; continue; }
        phys = devs[i];
        qfam = (uint32_t)fam;
        can_export_syncfd = sf;
        can_ycbcr = ycf.samplerYcbcrConversion == VK_TRUE;
        ext_formats = want.shaderStorageImageExtendedFormats == VK_TRUE;
        LOGI("vk: device %s (api %u.%u.%u) queue family %u%s, sync_fd=%d, extFormats=%d ycbcr=%d", pr.deviceName,
             VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion), VK_VERSION_PATCH(pr.apiVersion),
             qfam, queue_high_prio ? " (high priority)" : "", sf, (int)want.shaderStorageImageExtendedFormats, (int)can_ycbcr);
    }
    if (phys == VK_NULL_HANDLE) { LOGE("vk: no suitable device"); return false; }

    f.GetDeviceQueue(dev, qfam, 0, &queue);
    {   // таймстампы для замера времени GPU (адаптивная нагрузка)
        VkPhysicalDeviceProperties pp;
        f.GetPhysicalDeviceProperties(phys, &pp);
        uint32_t qc = 0;
        f.GetPhysicalDeviceQueueFamilyProperties(phys, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qc);
        f.GetPhysicalDeviceQueueFamilyProperties(phys, &qc, qs.data());
        const uint32_t bits = qfam < qc ? qs[qfam].timestampValidBits : 0;
        if (bits >= 32 && pp.limits.timestampPeriod > 0.0f && f.CreateQueryPool && f.CmdWriteTimestamp && f.GetQueryPoolResults) {
            can_ts = true;
            ts_ns_per_tick = (double)pp.limits.timestampPeriod;
            ts_mask = bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
        }
        LOGD("vk: timestamps %s (period %.2f ns, %u bits)", can_ts ? "on" : "off", (double)pp.limits.timestampPeriod, bits);
    }
    f.GetPhysicalDeviceMemoryProperties(phys, &memprops_);
    f.GetAndroidHardwareBufferPropertiesANDROID =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)f.GetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (can_export_syncfd) {
        f.GetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)f.GetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
        f.ImportSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR)f.GetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
        if (!f.GetSemaphoreFdKHR || !f.ImportSemaphoreFdKHR) can_export_syncfd = false;
    }
    if (!f.GetAndroidHardwareBufferPropertiesANDROID) { LOGE("vk: AHB properties entry point missing"); return false; }
    return true;
}

void VkCtx::destroy() {
    if (dev && f.DeviceWaitIdle) f.DeviceWaitIdle(dev);
    destroyExtConv();
    if (dev && f.DestroyDevice) f.DestroyDevice(dev, nullptr);
    if (inst && f.DestroyInstance) f.DestroyInstance(inst, nullptr);
    dev = VK_NULL_HANDLE; inst = VK_NULL_HANDLE; phys = VK_NULL_HANDLE;
}

bool VkCtx::findMemType(uint32_t bits, VkMemoryPropertyFlags want, uint32_t* idx) {
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want) { *idx = i; return true; }
    return false;
}

bool VkCtx::pickMemType(uint32_t bits, uint32_t* idx) {
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
        if (bits & (1u << i)) { *idx = i; return true; }
    return false;
}

bool VkCtx::pickLocalMemType(uint32_t bits, uint32_t* idx) {
    for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { *idx = i; return true; }
    return pickMemType(bits, idx);
}

void VkCtx::destroyExtConv() {
    if (!dev) return;
    if (ext_.pipe) f.DestroyPipeline(dev, ext_.pipe, nullptr);
    if (ext_.pl) f.DestroyPipelineLayout(dev, ext_.pl, nullptr);
    if (dpool_) f.DestroyDescriptorPool(dev, dpool_, nullptr);      // освобождает и наборы
    if (ext_.dsl) f.DestroyDescriptorSetLayout(dev, ext_.dsl, nullptr);
    if (ext_.samp) f.DestroySampler(dev, ext_.samp, nullptr);
    if (ext_.conv) f.DestroySamplerYcbcrConversion(dev, ext_.conv, nullptr);
    ext_ = ExtConv();
    dpool_ = VK_NULL_HANDLE;
}

// Конверсия/сэмплер/pipeline под один externalFormat (в игре формат один; другой — отказ).
bool VkCtx::ensureExtConv(const VkAndroidHardwareBufferFormatPropertiesANDROID& fp) {
    if (ext_.conv) return ext_.ext_fmt == fp.externalFormat;
    if (!can_ycbcr || !f.CreateSamplerYcbcrConversion || !f.CreateComputePipelines) { LOGW("ext: ycbcr conversion unsupported"); return false; }

    VkExternalFormatANDROID ef{};
    ef.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    ef.externalFormat = fp.externalFormat;
    VkSamplerYcbcrConversionCreateInfo cc{};
    cc.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    cc.pNext = &ef;
    cc.format = VK_FORMAT_UNDEFINED;
    cc.ycbcrModel = fp.suggestedYcbcrModel;
    cc.ycbcrRange = fp.suggestedYcbcrRange;
    cc.components = fp.samplerYcbcrConversionComponents;
    cc.xChromaOffset = fp.suggestedXChromaOffset;
    cc.yChromaOffset = fp.suggestedYChromaOffset;
    cc.chromaFilter = VK_FILTER_NEAREST;
    cc.forceExplicitReconstruction = VK_FALSE;
    VkResult r = f.CreateSamplerYcbcrConversion(dev, &cc, nullptr, &ext_.conv);
    if (r != VK_SUCCESS) { LOGW("ext: vkCreateSamplerYcbcrConversion failed %d", (int)r); ext_ = ExtConv(); return false; }
    ext_.ext_fmt = fp.externalFormat;
    LOGD("ext: ycbcr conversion model=%d range=%d comps=(%d,%d,%d,%d) chromaOff=(%d,%d)", (int)cc.ycbcrModel, (int)cc.ycbcrRange,
         (int)cc.components.r, (int)cc.components.g, (int)cc.components.b, (int)cc.components.a, (int)cc.xChromaOffset, (int)cc.yChromaOffset);

    VkSamplerYcbcrConversionInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    sci.conversion = ext_.conv;
    VkSamplerCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.pNext = &sci;
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = 0.0f;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    r = f.CreateSampler(dev, &sc, nullptr, &ext_.samp);
    if (r != VK_SUCCESS) { LOGW("ext: vkCreateSampler failed %d", (int)r); destroyExtConv(); return false; }

    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0].binding = 0; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb[0].descriptorCount = 1;
    lb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; lb[0].pImmutableSamplers = &ext_.samp;
    lb[1].binding = 1; lb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; lb[1].descriptorCount = 1;
    lb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 2; dl.pBindings = lb;
    r = f.CreateDescriptorSetLayout(dev, &dl, nullptr, &ext_.dsl);
    if (r != VK_SUCCESS) { LOGW("ext: descriptor set layout failed %d", (int)r); destroyExtConv(); return false; }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &ext_.dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    r = f.CreatePipelineLayout(dev, &pli, nullptr, &ext_.pl);
    if (r != VK_SUCCESS) { LOGW("ext: pipeline layout failed %d", (int)r); destroyExtConv(); return false; }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof kExtBlitSpv; smi.pCode = kExtBlitSpv;
    VkShaderModule sm = VK_NULL_HANDLE;
    r = f.CreateShaderModule(dev, &smi, nullptr, &sm);
    if (r != VK_SUCCESS) { LOGW("ext: shader module failed %d", (int)r); destroyExtConv(); return false; }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm; cpi.stage.pName = "main";
    cpi.layout = ext_.pl;
    r = f.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &ext_.pipe);
    f.DestroyShaderModule(dev, sm, nullptr);
    if (r != VK_SUCCESS) { LOGW("ext: compute pipeline failed %d", (int)r); destroyExtConv(); return false; }

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 8; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
    r = f.CreateDescriptorPool(dev, &dp, nullptr, &dpool_);
    if (r != VK_SUCCESS) { LOGW("ext: descriptor pool failed %d", (int)r); destroyExtConv(); return false; }
    VkDescriptorSetLayout ls[8];
    for (auto& l : ls) l = ext_.dsl;
    VkDescriptorSetAllocateInfo da{};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = dpool_; da.descriptorSetCount = 8; da.pSetLayouts = ls;
    r = f.AllocateDescriptorSets(dev, &da, ext_.dsets);
    if (r != VK_SUCCESS) { LOGW("ext: allocate descriptor sets failed %d", (int)r); destroyExtConv(); return false; }
    LOGD("ext: blit pipeline ready for externalFormat=0x%llx", (unsigned long long)fp.externalFormat);
    return true;
}

bool VkCtx::formatHasStorage(VkFormat fmt) {
    if (fmt != VK_FORMAT_R8G8B8A8_UNORM && !ext_formats) return false;     // rgb10_a2 в шейдере требует StorageImageExtendedFormats
    if (!f.GetPhysicalDeviceFormatProperties) return fmt == VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties fp{};
    f.GetPhysicalDeviceFormatProperties(phys, fmt, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

bool VkCtx::createPlainImage(uint32_t w, uint32_t h, PlainImage* out, VkFormat fmt) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    PlainImage p;
    if (f.CreateImage(dev, &ci, nullptr, &p.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    f.GetImageMemoryRequirements(dev, p.image, &mr);
    uint32_t mt;
    if (!pickLocalMemType(mr.memoryTypeBits, &mt)) { f.DestroyImage(dev, p.image, nullptr); return false; }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
    if (f.AllocateMemory(dev, &ai, nullptr, &p.mem) != VK_SUCCESS) { f.DestroyImage(dev, p.image, nullptr); return false; }
    if (f.BindImageMemory(dev, p.image, p.mem, 0) != VK_SUCCESS) { f.FreeMemory(dev, p.mem, nullptr); f.DestroyImage(dev, p.image, nullptr); return false; }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = p.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (f.CreateImageView(dev, &vi, nullptr, &p.view) != VK_SUCCESS) { f.FreeMemory(dev, p.mem, nullptr); f.DestroyImage(dev, p.image, nullptr); return false; }
    p.w = w; p.h = h;
    *out = p;
    return true;
}

void VkCtx::destroyPlainImage(PlainImage* im) {
    if (im->view) f.DestroyImageView(dev, im->view, nullptr);
    if (im->image) f.DestroyImage(dev, im->image, nullptr);
    if (im->mem) f.FreeMemory(dev, im->mem, nullptr);
    *im = PlainImage();
}

void VkCtx::recordExtCopy(VkCommandBuffer cb, const AhbImage& src, const PlainImage& dst, const int32_t crop[4], int slot) {
    VkDescriptorSet ds = ext_.dsets[slot & 7];
    VkDescriptorImageInfo si{VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   // сэмплер — immutable
    VkDescriptorImageInfo di{VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2]{};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = ds; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &si;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[1].dstSet = ds; wr[1].dstBinding = 1; wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &di;
    f.UpdateDescriptorSets(dev, 2, wr, 0, nullptr);
    f.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ext_.pipe);
    f.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ext_.pl, 0, 1, &ds, 0, nullptr);
    int32_t pc[8] = {crop[0], crop[1], crop[2], crop[3], (int32_t)src.w, (int32_t)src.h, (int32_t)dst.w, (int32_t)dst.h};
    f.CmdPushConstants(cb, ext_.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    f.CmdDispatch(cb, (dst.w + 15) / 16, (dst.h + 15) / 16, 1);
}


bool VkCtx::importAhb(AHardwareBuffer* ahb, VkImageUsageFlags usage, AhbImage* out) {
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(ahb, &d);

    // Диагностика: первые несколько отказов подробно (на каком шаге и почему).
    static int fails = 0;
#define FAIL(...) do { if (fails++ < 4) LOGW(__VA_ARGS__); return false; } while (0)

    VkAndroidHardwareBufferFormatPropertiesANDROID fp{};
    fp.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID props{};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    props.pNext = &fp;
    VkResult pr = f.GetAndroidHardwareBufferPropertiesANDROID(dev, ahb, &props);
    if (pr != VK_SUCCESS) FAIL("import: vkGetAndroidHardwareBufferProperties failed %d (ahb fmt=0x%x usage=0x%llx %ux%u)", (int)pr, d.format, (unsigned long long)d.usage, d.width, d.height);
    static int logged = 0;
    if (logged++ < 4)
        LOGD("import: ahb fmt=0x%x usage=0x%llx %ux%u -> vkFormat=%d externalFormat=0x%llx features=0x%x allocSize=%llu memBits=0x%x",
             d.format, (unsigned long long)d.usage, d.width, d.height, (int)fp.format,
             (unsigned long long)fp.externalFormat, (unsigned)fp.formatFeatures, (unsigned long long)props.allocationSize, props.memoryTypeBits);
    const bool external = fp.format == VK_FORMAT_UNDEFINED;
    if (external) {
        if (!(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || fp.externalFormat == 0) FAIL("import: external format not usable for this role (externalFormat=0x%llx)", (unsigned long long)fp.externalFormat);
        if (!(fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) FAIL("import: external format is not sampleable (features=0x%x)", (unsigned)fp.formatFeatures);
        if (!ensureExtConv(fp)) FAIL("import: ycbcr conversion for externalFormat=0x%llx unavailable", (unsigned long long)fp.externalFormat);
    }

    VkFormatFeatureFlags need = 0;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) need |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) need |= VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (!external && (fp.formatFeatures & need) != need) FAIL("import: format features 0x%x lack required 0x%x", (unsigned)fp.formatFeatures, (unsigned)need);

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkExternalFormatANDROID efmt{};
    efmt.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    efmt.externalFormat = fp.externalFormat;
    if (external) ext.pNext = &efmt;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fp.format;
    ci.extent = {d.width, d.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = external ? (VkImageUsageFlags)VK_IMAGE_USAGE_SAMPLED_BIT : usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    AhbImage im;
    VkResult cr = f.CreateImage(dev, &ci, nullptr, &im.image);
    if (cr != VK_SUCCESS) FAIL("import: vkCreateImage failed %d (vkFormat=%d usage=0x%x)", (int)cr, (int)fp.format, (unsigned)usage);

    uint32_t mt;
    if (!pickMemType(props.memoryTypeBits, &mt)) { f.DestroyImage(dev, im.image, nullptr); FAIL("import: no memory type for bits 0x%x", props.memoryTypeBits); }
    VkImportAndroidHardwareBufferInfoANDROID imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    imp.buffer = ahb;
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.pNext = &imp;
    ded.image = im.image;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &ded;
    ai.allocationSize = props.allocationSize;
    ai.memoryTypeIndex = mt;
    VkResult ar = f.AllocateMemory(dev, &ai, nullptr, &im.mem);
    if (ar != VK_SUCCESS) { f.DestroyImage(dev, im.image, nullptr); FAIL("import: vkAllocateMemory failed %d", (int)ar); }
    VkResult br = f.BindImageMemory(dev, im.image, im.mem, 0);
    if (br != VK_SUCCESS) { f.FreeMemory(dev, im.mem, nullptr); f.DestroyImage(dev, im.image, nullptr); FAIL("import: vkBindImageMemory failed %d", (int)br); }
    if (external) {
        VkSamplerYcbcrConversionInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        sci.conversion = ext_.conv;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.pNext = &sci;
        vi.image = im.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_UNDEFINED;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkResult vr = f.CreateImageView(dev, &vi, nullptr, &im.view);
        if (vr != VK_SUCCESS) { f.FreeMemory(dev, im.mem, nullptr); f.DestroyImage(dev, im.image, nullptr); FAIL("import: vkCreateImageView(ycbcr) failed %d", (int)vr); }
        im.external = true;
    }
#undef FAIL
    im.ahb = ahb; im.format = fp.format; im.w = d.width; im.h = d.height;
    *out = im;
    return true;
}

bool VkCtx::createRingImage(uint32_t w, uint32_t h, VkImageUsageFlags usage, AhbImage* out, bool ten_bit) {
    AHardwareBuffer_Desc d = {};
    d.width = w; d.height = h; d.layers = 1;
    d.format = ten_bit ? AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
              AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0) {
        d.usage &= ~(uint64_t)AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;     // не все гралоки дают OVERLAY
        if (AHardwareBuffer_allocate(&d, &ahb) != 0) return false;
    }
    if (!importAhb(ahb, usage, out)) { AHardwareBuffer_release(ahb); return false; }
    out->owns_ahb = true;
    return true;
}

void VkCtx::destroyImage(AhbImage* im) {
    if (im->view) f.DestroyImageView(dev, im->view, nullptr);
    if (im->image) f.DestroyImage(dev, im->image, nullptr);
    if (im->mem) f.FreeMemory(dev, im->mem, nullptr);
    if (im->ahb) AHardwareBuffer_release(im->ahb);    // и наш буфер, и взятая ссылка acquire
    *im = AhbImage();
}

VkSemaphore VkCtx::createSemaphore(bool exportable) {
    VkExportSemaphoreCreateInfo ex{};
    ex.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    ex.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (exportable && can_export_syncfd) si.pNext = &ex;
    VkSemaphore s = VK_NULL_HANDLE;
    if (f.CreateSemaphore(dev, &si, nullptr, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

int VkCtx::exportSyncFd(VkSemaphore sem) {
    if (!can_export_syncfd) return -1;
    VkSemaphoreGetFdInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gi.semaphore = sem;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    if (f.GetSemaphoreFdKHR(dev, &gi, &fd) != VK_SUCCESS) return -1;
    return fd;       // -1 допустимо: сигнал уже выполнен
}

bool VkCtx::importSyncFd(VkSemaphore sem, int fd) {
    if (!can_export_syncfd) return false;
    VkImportSemaphoreFdInfoKHR ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    ii.semaphore = sem;
    ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    ii.fd = fd;
    return f.ImportSemaphoreFdKHR(dev, &ii) == VK_SUCCESS;     // при успехе fd принадлежит драйверу
}
