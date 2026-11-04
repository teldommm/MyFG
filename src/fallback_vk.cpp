// Запасной путь захвата для Vulkan-игр: vkCreateDevice / vkCreateSwapchainKHR / vkQueuePresentKHR.
//
//  Когда нужен: BufferQueue-хуки libgui не встали (другая версия Android) или игра не отдаёт кадры через них.
//
//  Как работает:
//   1. vkCreateDevice игры (хук на api::CreateDevice) — к её расширениям добавляем
//      VK_ANDROID_external_memory_android_hardware_buffer и его зависимости (если устройство их умеет).
//      Если создание с добавками не удалось — повторяем без них (игра не пострадает, запасной путь отключается).
//   2. После создания устройства берём из vkGetDeviceProcAddr адреса vkCreateSwapchainKHR / vkDestroySwapchainKHR /
//      vkQueuePresentKHR (это настоящие функции загрузчика, не 4-байтовые заглушки) и ставим на них хуки.
//   3. vkCreateSwapchainKHR: добавляем imageUsage |= TRANSFER_SRC (если драйвер не принял — создаём как просили).
//   4. vkQueuePresentKHR: пока запасной путь включён, на очереди презента ставим короткий submit:
//        ждёт те же wait-семафоры, что и презент → копирует картинку свопчейна в картинку нашего AHB
//        (в устройстве игры) → снова сигналит те же семафоры. Дальше презент идёт как обычно.
//      Отдельный поток ждёт fence копирования и отдаёт конвейеру обычное capture::Job.
//
//  Ограничения (честно):
//   * устройство игры должно быть создано ПОСЛЕ установки хука (hook_present=2 ставит его сразу при старте
//     процесса; hook_present=1 — только если BufferQueue-хуки не встали);
//   * команды пишутся в пул семейства очередей с флагом GRAPHICS (из queueCreateInfo устройства);
//     презент на очереди другого семейства не поддерживается;
//   * форматы свопчейна: R8G8B8A8_UNORM/SRGB (копия), B8G8R8A8_UNORM (blit с перестановкой каналов);
//     B8G8R8A8_SRGB, 10-битные и FP16 — не поддерживаются (кадры не берём);
//   * pre-rotation: transform слоя считается как обратный к preTransform свопчейна.
#include "fallback.h"
#include "capture.h"
#include "common.h"
#include "config.h"
#include "hooks.h"
#include "util.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kN = 4;                         // слотов в кольце одного свопчейна
constexpr int kMaxDev = 4;

#define DEV_FNS(X)                                                                                     \
    X(CreateImage) X(DestroyImage) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)                  \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)         \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBlitImage)    \
    X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetSwapchainImagesKHR)

struct DevState {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t fam = 0;
    bool ok = false;              // расширение AHB включено на этом устройстве
    bool foreign = false;         // доступен VK_QUEUE_FAMILY_FOREIGN_EXT
    VkPhysicalDeviceMemoryProperties mp{};
#define X(n) PFN_vk##n n = nullptr;
    DEV_FNS(X)
#undef X
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAHBProps = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
};

struct Slot {
    AHardwareBuffer* ahb = nullptr;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::atomic<int> busy{0};
};

struct Swap {
    std::shared_ptr<DevState> d;
    VkSwapchainKHR h = VK_NULL_HANDLE;
    uint32_t w = 0, hgt = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    int mode = 0;                 // 1 — copy, 2 — blit, 0 — не поддерживается
    uint32_t transform = 0;       // HAL transform слоя
    std::vector<VkImage> images;
    bool usage_ok = false;        // TRANSFER_SRC добавлен
    std::mutex m;                 // выбор слота / создание кольца
    std::mutex wait_m;            // ждущий поток держит его на время vkWaitForFences
    bool ring_ok = false, dead = false;
    int fails = 0;
    Slot s[kN];
};

// ---- глобальное состояние -----------------------------------------------------------------------------
std::mutex g_m;
std::shared_ptr<DevState> g_devs[kMaxDev];
std::unordered_map<VkSwapchainKHR, std::shared_ptr<Swap>> g_sw;
std::atomic<bool> g_active{false};
std::atomic<bool> g_failed{false};
std::atomic<bool> g_cd_hooked{false}, g_dev_hooks_done{false}, g_dev_hooks_ok{false};
pthread_key_t g_own_key;
bool g_own_key_ok = false;
PFN_vkGetDeviceProcAddr g_gdpa = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties g_gqfp = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties g_gmp = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_edep = nullptr;

PFN_vkCreateDevice orig_create_device = nullptr;
PFN_vkCreateSwapchainKHR orig_create_sc = nullptr;
PFN_vkDestroySwapchainKHR orig_destroy_sc = nullptr;
PFN_vkQueuePresentKHR orig_present = nullptr;

std::atomic<uint64_t> g_frames{0}, g_dropped{0};

void ensure_key() {
    static std::once_flag once;
    std::call_once(once, [] { g_own_key_ok = pthread_key_create(&g_own_key, nullptr) == 0; });
}

// ---- помощники ----------------------------------------------------------------------------------------
// Если p — короткая заглушка вида "B target", идём по переходу: настоящая функция загрузчика лежит дальше
// (по ней идут и экспорт vkCreateDevice, и vkGetInstanceProcAddr("vkCreateDevice")).
void* follow_branch(void* p, size_t size, size_t* out_size) {
    *out_size = size;
    if (size >= 16) return p;
    for (int hop = 0; hop < 4; hop++) {
        uint32_t ins = 0;
        if (!safe_read(p, &ins, 4)) return p;
        if ((ins & 0xFC000000u) != 0x14000000u) return p;              // не B imm26
        int32_t imm = (int32_t)(ins & 0x03FFFFFFu);
        if (imm & 0x02000000) imm |= (int32_t)0xFC000000u;              // знаковое расширение
        p = (void*)((uintptr_t)p + (intptr_t)imm * 4);
        *out_size = 0;                                                   // размер приёмника неизвестен
        // приёмник мог быть ещё одной заглушкой — проверим по следующему кругу, пока не упрёмся в не-B
        uint32_t nxt = 0;
        if (!safe_read(p, &nxt, 4) || (nxt & 0xFC000000u) != 0x14000000u) break;
    }
    return p;
}

bool has_ext(const std::vector<VkExtensionProperties>& v, const char* n) {
    for (auto& e : v) if (strcmp(e.extensionName, n) == 0) return true;
    return false;
}
bool has_name(const std::vector<const char*>& v, const char* n) {
    for (auto s : v) if (strcmp(s, n) == 0) return true;
    return false;
}

std::shared_ptr<DevState> dev_for(VkDevice d) {
    std::lock_guard<std::mutex> g(g_m);
    for (auto& s : g_devs) if (s && s->dev == d) return s;
    return nullptr;
}

bool load_dev_fns(DevState& d) {
    if (!g_gdpa) return false;
#define X(n) d.n = (PFN_vk##n)g_gdpa(d.dev, "vk" #n); if (!d.n) return false;
    DEV_FNS(X)
#undef X
    d.GetAHBProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)g_gdpa(d.dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    return d.GetAHBProps != nullptr;
}

uint32_t inverse_transform(VkSurfaceTransformFlagBitsKHR t) {
    // HAL: ROT_90 = 4, ROT_180 = 3, ROT_270 = 7. Загрузчик ставит буферу обратное к preTransform.
    switch (t) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  return 7;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return 3;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return 4;
        default: return 0;
    }
}

// ---- кольцо ---------------------------------------------------------------------------------------------
void free_slot(DevState& d, Slot& s) {
    if (s.fence) d.DestroyFence(d.dev, s.fence, nullptr);
    if (s.img) d.DestroyImage(d.dev, s.img, nullptr);
    if (s.mem) d.FreeMemory(d.dev, s.mem, nullptr);
    if (s.ahb) AHardwareBuffer_release(s.ahb);
    s.fence = VK_NULL_HANDLE; s.img = VK_NULL_HANDLE; s.mem = VK_NULL_HANDLE; s.ahb = nullptr; s.cb = VK_NULL_HANDLE;
    s.busy.store(0);
}

bool build_slot(DevState& d, Swap& sc, Slot& s) {
    AHardwareBuffer_Desc desc{};
    desc.width = sc.w; desc.height = sc.hgt; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &s.ahb) != 0 || !s.ahb) return false;

    VkAndroidHardwareBufferFormatPropertiesANDROID fp{}; fp.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID pr{}; pr.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID; pr.pNext = &fp;
    if (d.GetAHBProps(d.dev, s.ahb, &pr) != VK_SUCCESS) return false;
    if (fp.format != VK_FORMAT_R8G8B8A8_UNORM) { LOGW("vk fallback: AHB format 0x%x unexpected", (unsigned)fp.format); return false; }

    VkExternalMemoryImageCreateInfo emi{}; emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {sc.w, sc.hgt, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.CreateImage(d.dev, &ici, nullptr, &s.img) != VK_SUCCESS) return false;

    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < d.mp.memoryTypeCount; i++)
        if ((pr.memoryTypeBits & (1u << i)) && (d.mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
    if (mt == UINT32_MAX)
        for (uint32_t i = 0; i < d.mp.memoryTypeCount; i++) if (pr.memoryTypeBits & (1u << i)) { mt = i; break; }
    if (mt == UINT32_MAX) return false;

    VkImportAndroidHardwareBufferInfoANDROID imp{}; imp.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    imp.buffer = s.ahb;
    VkMemoryDedicatedAllocateInfo ded{}; ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; ded.pNext = &imp;
    ded.image = s.img;
    VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.pNext = &ded;
    mai.allocationSize = pr.allocationSize; mai.memoryTypeIndex = mt;
    if (d.AllocateMemory(d.dev, &mai, nullptr, &s.mem) != VK_SUCCESS) return false;
    if (d.BindImageMemory(d.dev, s.img, s.mem, 0) != VK_SUCCESS) return false;

    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (d.CreateFence(d.dev, &fci, nullptr, &s.fence) != VK_SUCCESS) return false;
    return true;
}

bool ensure_ring_locked(Swap& sc) {
    if (sc.ring_ok) return true;
    DevState& d = *sc.d;
    if (!d.pool) {
        VkCommandPoolCreateInfo pci{}; pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = d.fam;
        if (d.CreateCommandPool(d.dev, &pci, nullptr, &d.pool) != VK_SUCCESS) return false;
    }
    VkCommandBuffer cbs[kN];
    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = d.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = kN;
    if (d.AllocateCommandBuffers(d.dev, &ai, cbs) != VK_SUCCESS) return false;
    for (int i = 0; i < kN; i++) {
        sc.s[i].cb = cbs[i];
        if (!build_slot(d, sc, sc.s[i])) {
            for (int k = 0; k <= i; k++) { VkCommandBuffer c = sc.s[k].cb; free_slot(d, sc.s[k]); (void)c; }
            LOGW("vk fallback: ring %ux%u could not be built", sc.w, sc.hgt);
            return false;
        }
    }
    sc.ring_ok = true;
    LOGD("vk fallback: ring %ux%u x%d fmt=%d (%s)", sc.w, sc.hgt, kN, (int)sc.fmt, sc.mode == 1 ? "copy" : "blit");
    return true;
}

void destroy_swap(const std::shared_ptr<Swap>& sc) {
    std::lock_guard<std::mutex> w(sc->wait_m);                // дождаться ждущего потока (<= ~1 c)
    std::lock_guard<std::mutex> l(sc->m);
    if (sc->dead) return;
    sc->dead = true;
    if (sc->ring_ok) {
        DevState& d = *sc->d;
        for (auto& s : sc->s) if (s.fence && s.busy.load()) d.WaitForFences(d.dev, 1, &s.fence, VK_TRUE, 500000000ull);
        for (auto& s : sc->s) free_slot(d, s);
        sc->ring_ok = false;
    }
}

// ---- поток ожидания fence ---------------------------------------------------------------------------
struct Item { std::shared_ptr<Swap> sc; int idx; };
std::mutex g_qm;
std::condition_variable g_qcv;
std::deque<Item> g_q;
bool g_thread_started = false;

void done_cb(void* ctx, int tag) {
    auto* p = (std::shared_ptr<Swap>*)ctx;
    if (tag >= 0 && tag < kN) (*p)->s[tag].busy.store(0);
    delete p;
}

void waiter() {
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(g_qm);
            g_qcv.wait(lk, [] { return !g_q.empty(); });
            it = std::move(g_q.front());
            g_q.pop_front();
        }
        Swap& sc = *it.sc;
        std::lock_guard<std::mutex> w(sc.wait_m);
        if (sc.dead || !sc.ring_ok) continue;
        Slot& s = sc.s[it.idx];
        DevState& d = *sc.d;
        VkResult r = d.WaitForFences(d.dev, 1, &s.fence, VK_TRUE, 1000000000ull);
        if (r != VK_SUCCESS) { s.busy.store(0); sc.fails++; continue; }
        capture::Job j;
        AHardwareBuffer_acquire(s.ahb);
        j.src = s.ahb;
        j.fence_fd = -1;
        j.cl = 0; j.ct = 0; j.cr = (int)sc.w; j.cb = (int)sc.hgt;
        j.bw = (int)sc.w; j.bh = (int)sc.hgt;
        j.ts_ns = now_ns();
        j.transform = sc.transform;
        j.dataspace = 0;
        j.done = done_cb; j.done_ctx = new std::shared_ptr<Swap>(it.sc); j.done_tag = it.idx;
        capture::extSubmit(j);
    }
}

void enqueue(const std::shared_ptr<Swap>& sc, int idx) {
    {
        std::lock_guard<std::mutex> lk(g_qm);
        if (!g_thread_started) { g_thread_started = true; std::thread(waiter).detach(); }
        g_q.push_back({sc, idx});
    }
    g_qcv.notify_one();
}

// ---- копирование при презенте -------------------------------------------------------------------------
void try_copy(VkQueue q, const VkPresentInfoKHR* pi) {
    std::shared_ptr<Swap> sc;
    {
        std::lock_guard<std::mutex> g(g_m);
        auto it = g_sw.find(pi->pSwapchains[0]);
        if (it == g_sw.end()) return;
        sc = it->second;
    }
    if (sc->mode == 0 || !sc->usage_ok || sc->fails >= 8 || pi->waitSemaphoreCount > 16) return;
    if (capture::bqActive()) return;
    const uint32_t ii = pi->pImageIndices[0];
    if (ii >= sc->images.size()) return;

    ANativeWindow* win = hooks::pickWindow((int)sc->w, (int)sc->hgt);
    const bool elected = capture::extOffer(sc.get(), (int)sc->w, (int)sc->hgt, win);
    if (win) ANativeWindow_release(win);
    if (!elected) return;

    DevState& d = *sc->d;
    int idx = -1;
    {
        std::lock_guard<std::mutex> lk(sc->m);
        if (sc->dead) return;
        if (!ensure_ring_locked(*sc)) { sc->fails = 8; return; }
        for (int i = 0; i < kN; i++) if (sc->s[i].busy.load() == 0) { idx = i; break; }
        if (idx < 0) { g_dropped++; return; }
        sc->s[idx].busy.store(1);
    }
    Slot& s = sc->s[idx];
    VkImage src = sc->images[ii];

    d.ResetCommandBuffer(s.cb, 0);
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(s.cb, &bi);

    VkImageMemoryBarrier b[2]{};
    for (auto& x : b) {
        x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        x.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    b[0].image = src;
    b[0].srcAccessMask = 0; b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[1].image = s.img;
    b[1].srcAccessMask = 0; b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    d.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);

    if (sc->mode == 1) {
        VkImageCopy rg{};
        rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.dstSubresource = rg.srcSubresource;
        rg.extent = {sc->w, sc->hgt, 1};
        d.CmdCopyImage(s.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    } else {
        VkImageBlit rg{};
        rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.dstSubresource = rg.srcSubresource;
        rg.srcOffsets[1] = {(int32_t)sc->w, (int32_t)sc->hgt, 1}; rg.dstOffsets[1] = rg.srcOffsets[1];
        d.CmdBlitImage(s.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg, VK_FILTER_NEAREST);
    }

    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b[0].dstAccessMask = 0;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b[1].dstAccessMask = 0;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (d.foreign) { b[1].srcQueueFamilyIndex = d.fam; b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT; }
    d.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    if (d.EndCommandBuffer(s.cb) != VK_SUCCESS) { s.busy.store(0); sc->fails++; return; }

    VkPipelineStageFlags stages[16];
    for (auto& st : stages) st = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = pi->waitSemaphoreCount; si.pWaitSemaphores = pi->pWaitSemaphores; si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1; si.pCommandBuffers = &s.cb;
    si.signalSemaphoreCount = pi->waitSemaphoreCount; si.pSignalSemaphores = pi->pWaitSemaphores;   // те же семафоры: презент ждёт после нас
    d.ResetFences(d.dev, 1, &s.fence);
    if (d.QueueSubmit(q, 1, &si, s.fence) != VK_SUCCESS) { s.busy.store(0); sc->fails++; LOGW("vk fallback: submit failed"); return; }
    uint64_t n = g_frames++;
    if (n < 4) LOGD("vk fallback frame #%llu %ux%u slot=%d", (unsigned long long)n, sc->w, sc->hgt, idx);
    enqueue(sc, idx);
}

// ---- хуки ------------------------------------------------------------------------------------------------
VkResult hook_present(VkQueue q, const VkPresentInfoKHR* pi) {
    if (g_active.load(std::memory_order_relaxed) && g_cfg.enable && pi && pi->swapchainCount >= 1 && pi->pImageIndices)
        try_copy(q, pi);
    return orig_present(q, pi);
}

VkResult hook_create_sc(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* al, VkSwapchainKHR* out) {
    auto d = dev_for(dev);
    if (!d || !d->ok || !ci) return orig_create_sc(dev, ci, al, out);
    VkSwapchainCreateInfoKHR copy = *ci;
    bool usage = false;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!(copy.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        r = orig_create_sc(dev, &copy, al, out);
        usage = r == VK_SUCCESS;
    }
    if (!usage) {
        if (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage = true;     // игра сама просила
        r = orig_create_sc(dev, ci, al, out);
    }
    if (r != VK_SUCCESS || !out || !*out) return r;

    auto sc = std::make_shared<Swap>();
    sc->d = d; sc->h = *out;
    sc->w = ci->imageExtent.width; sc->hgt = ci->imageExtent.height;
    sc->fmt = ci->imageFormat;
    sc->usage_ok = usage;
    sc->transform = inverse_transform(ci->preTransform);
    switch (ci->imageFormat) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: sc->mode = 1; break;
        case VK_FORMAT_B8G8R8A8_UNORM: sc->mode = 2; break;
        default: sc->mode = 0; break;
    }
    uint32_t n = 0;
    d->GetSwapchainImagesKHR(dev, *out, &n, nullptr);
    sc->images.resize(n);
    if (n) d->GetSwapchainImagesKHR(dev, *out, &n, sc->images.data());
    sc->images.resize(n);
    LOGD("vk fallback: swapchain %ux%u fmt=%d usage_ok=%d mode=%d images=%u", sc->w, sc->hgt, (int)sc->fmt, usage, sc->mode, n);
    if (sc->mode == 0) LOGW("vk fallback: swapchain format %d is not supported (frames will not be captured)", (int)sc->fmt);
    std::shared_ptr<Swap> old;
    {
        std::lock_guard<std::mutex> g(g_m);
        auto it = g_sw.find(*out);
        if (it != g_sw.end()) old = it->second;
        g_sw[*out] = sc;
    }
    if (old) destroy_swap(old);
    return r;
}

void hook_destroy_sc(VkDevice dev, VkSwapchainKHR h, const VkAllocationCallbacks* al) {
    std::shared_ptr<Swap> sc;
    {
        std::lock_guard<std::mutex> g(g_m);
        auto it = g_sw.find(h);
        if (it != g_sw.end()) { sc = it->second; g_sw.erase(it); }
    }
    if (sc) destroy_swap(sc);
    orig_destroy_sc(dev, h, al);
}

bool install_dev_hooks(DevState& d) {
    if (g_dev_hooks_done.exchange(true)) return g_dev_hooks_ok.load();
    struct { const char* name; void* repl; void** orig; } t[] = {
        {"vkCreateSwapchainKHR", (void*)hook_create_sc, (void**)&orig_create_sc},
        {"vkDestroySwapchainKHR", (void*)hook_destroy_sc, (void**)&orig_destroy_sc},
        {"vkQueuePresentKHR", (void*)hook_present, (void**)&orig_present},
    };
    bool all = true;
    for (auto& e : t) {
        void* p = (void*)g_gdpa(d.dev, e.name);
        if (!p) { all = false; LOGW("vk fallback: %s not resolved", e.name); continue; }
        // размер неизвестен (внутренняя функция загрузчика), но это настоящие реализации, не заглушки
        if (!inline_hook(e.name, p, 0, false, e.repl, e.orig)) { all = false; LOGW("vk fallback: hook %s failed", e.name); }
    }
    if (!all) LOGW("vk fallback: swapchain/present hooks incomplete, path disabled");
    g_dev_hooks_ok.store(all);
    return all;
}

VkResult hook_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* al, VkDevice* out) {
    // Устройство самого модуля (поток конвейера помечен) и выключенный режим — без вмешательства.
    if (!ci || (g_own_key_ok && pthread_getspecific(g_own_key)) || !g_cfg.enable)
        return orig_create_device(phys, ci, al, out);

    const std::vector<const char*> orig_names(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    std::vector<const char*> names = orig_names;
    bool injected = false, supported = false;
    VkDeviceCreateInfo copy = *ci;
    if (g_edep) {
        uint32_t n = 0;
        g_edep(phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> av(n);
        if (n) g_edep(phys, nullptr, &n, av.data());
        av.resize(n);
        supported = has_ext(av, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        if (supported) {
            static const char* kWant[] = {
                VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                VK_KHR_MAINTENANCE_1_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
                VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            };
            for (const char* w : kWant)
                if (has_ext(av, w) && !has_name(names, w)) { names.push_back(w); injected = true; }
        }
    }
    VkResult r;
    if (injected) {
        copy.enabledExtensionCount = (uint32_t)names.size();
        copy.ppEnabledExtensionNames = names.data();
        r = orig_create_device(phys, &copy, al, out);
        if (r != VK_SUCCESS) {
            LOGW("vk fallback: vkCreateDevice with AHB extensions failed (%d), retrying as the game asked", (int)r);
            injected = false;
            r = orig_create_device(phys, ci, al, out);
        }
    } else {
        r = orig_create_device(phys, ci, al, out);
    }
    if (r != VK_SUCCESS || !out || !*out) return r;

    auto d = std::make_shared<DevState>();
    d->dev = *out; d->phys = phys;
    const std::vector<const char*>& enabled = injected ? names : orig_names;
    const bool ext_on = supported && has_name(enabled, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    d->foreign = ext_on && has_name(enabled, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    // семейство очередей: первое с GRAPHICS среди запрошенных
    d->fam = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    if (g_gqfp) {
        uint32_t n = 0;
        g_gqfp(phys, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qp(n);
        if (n) g_gqfp(phys, &n, qp.data());
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
            uint32_t f = ci->pQueueCreateInfos[i].queueFamilyIndex;
            if (f < n && (qp[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { d->fam = f; break; }
        }
    }
    if (g_gmp) g_gmp(phys, &d->mp);
    d->ok = ext_on && load_dev_fns(*d) && g_gmp != nullptr;
    LOGD("vk fallback: device %p created, AHB ext %s, queue family %u%s", (void*)*out, d->ok ? "on" : "off", d->fam,
         d->foreign ? ", foreign" : "");
    {
        std::lock_guard<std::mutex> g(g_m);
        for (auto& s : g_devs) if (!s) { s = d; break; }
    }
    if (d->ok) install_dev_hooks(*d);
    return r;
}

}  // namespace

namespace fallback {

void markOwnThread() {
    ensure_key();
    if (g_own_key_ok) pthread_setspecific(g_own_key, (void*)1);
}

void setVkActive(bool on) {
    if (g_active.exchange(on) != on) LOGI("vk fallback capture %s", on ? "armed" : "disarmed");
}

bool vkFailed() { return g_failed.load(); }

bool installVk() {
    if (g_cd_hooked.load()) return true;
    ensure_key();
    if (g_failed.load()) return false;
    void* vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
    if (!vk) return false;                                    // ещё не загружена — повторим
    g_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(vk, "vkGetDeviceProcAddr");
    g_gqfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)dlsym(vk, "vkGetPhysicalDeviceQueueFamilyProperties");
    g_gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)dlsym(vk, "vkGetPhysicalDeviceMemoryProperties");
    g_edep = (PFN_vkEnumerateDeviceExtensionProperties)dlsym(vk, "vkEnumerateDeviceExtensionProperties");
    if (!g_gdpa || !g_gqfp || !g_gmp || !g_edep) return false;

    size_t sz = 0;
    void* p = resolve_symbol_ex("libvulkan.so", "vkCreateDevice", false, &sz);
    if (!p) return false;
    size_t tsz = 0;
    void* t = follow_branch(p, sz, &tsz);
    LOGD("vk fallback: vkCreateDevice export=%p size=%zu target=%p size=%zu", p, sz, t, tsz);
    if (inline_hook("vkCreateDevice", t, tsz, false, (void*)hook_create_device, (void**)&orig_create_device)) {
        g_cd_hooked.store(true);
        LOGI("vk fallback hooks installed (vkCreateDevice)");
        return true;
    }
    LOGW("vk fallback: vkCreateDevice hook refused, Vulkan fallback unavailable");
    g_failed.store(true);
    return false;
}

}  // namespace fallback
