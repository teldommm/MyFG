#include "hooks.h"
#include "capture.h"
#include "fallback.h"
#include "pipeline.h"
#include "common.h"
#include "config.h"
#include "util.h"

#include <EGL/egl.h>
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- реестр окон ------------------------------------------------------------------------
constexpr int kWin = 4;
ANativeWindow* g_win[kWin] = {};     // [0] — самое новое
std::mutex g_wm;

// ---- EGL (запасной путь) ----------------------------------------------------------------
EGLSurface (*orig_cws)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;
EGLSurface hook_cws(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint* a) {
    EGLSurface s = orig_cws(d, c, w, a);
    if (s != EGL_NO_SURFACE && w) {
        hooks::noteWindow((ANativeWindow*)w);
        fallback::noteEglSurface(s, (ANativeWindow*)w);
    }
    return s;
}

// ---- ANativeWindow_fromSurface (запасной путь) --------------------------------------------
ANativeWindow* (*orig_fromSurface)(void*, void*) = nullptr;
ANativeWindow* hook_fromSurface(void* env, void* surface) {
    ANativeWindow* w = orig_fromSurface(env, surface);
    if (w) hooks::noteWindow(w);
    return w;
}

// ---- Vulkan: патч imageUsage свопчейна (ОПЦИЯ hook_vk=1) -----------------------------------
// Без TRANSFER_SRC|SAMPLED gralloc-буферы свопчейна могут не импортироваться нашим Vulkan'ом.
// vkGetInstanceProcAddr / vkGetDeviceProcAddr в libvulkan — очень короткие функции; inline_hook()
// откажется их патчить, если ELF говорит, что они < 16 байт. Окно игры отсюда не берётся
// (его отдаёт хук Surface), поэтому vkCreateAndroidSurfaceKHR не трогаем вовсе.
typedef int (*PFN_createSwapchain)(void*, const void*, const void*, void*);
typedef void* (*PFN_gpa)(void*, const char*);
PFN_createSwapchain csw_from_gipa = nullptr, csw_from_gdpa = nullptr;
PFN_gpa orig_gipa = nullptr, orig_gdpa = nullptr;

int patched_swapchain(PFN_createSwapchain orig, void* dev, const void* ci, const void* al, void* out) {
    static const VkImageUsageFlags kTries[] = {
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    if (ci && g_cfg.hook_vk) {
        for (VkImageUsageFlags add : kTries) {
            VkSwapchainCreateInfoKHR copy = *(const VkSwapchainCreateInfoKHR*)ci;   // pNext-цепочка общая
            if ((copy.imageUsage & add) == add) break;                              // уже есть
            copy.imageUsage |= add;
            int r = orig(dev, &copy, al, out);
            if (r == 0) { LOGI("swapchain imageUsage patched (+0x%x)", (unsigned)add); return 0; }
        }
    }
    return orig(dev, ci, al, out);
}
int my_csw_gipa(void* d, const void* ci, const void* al, void* out) { return patched_swapchain(csw_from_gipa, d, ci, al, out); }
int my_csw_gdpa(void* d, const void* ci, const void* al, void* out) { return patched_swapchain(csw_from_gdpa, d, ci, al, out); }

void* hook_gipa(void* inst, const char* name) {
    void* f = orig_gipa(inst, name);
    if (f && name && strcmp(name, "vkCreateSwapchainKHR") == 0 && f != (void*)my_csw_gipa) {
        csw_from_gipa = (PFN_createSwapchain)f;
        return (void*)my_csw_gipa;
    }
    return f;
}
void* hook_gdpa(void* dev, const char* name) {
    void* f = orig_gdpa(dev, name);
    if (f && name && strcmp(name, "vkCreateSwapchainKHR") == 0 && f != (void*)my_csw_gdpa) {
        csw_from_gdpa = (PFN_createSwapchain)f;
        return (void*)my_csw_gdpa;
    }
    return f;
}

bool g_egl = false, g_vk = false, g_vk_gave_up = false, g_anw = false, g_bq = false;
bool g_swap_inst = false, g_present_inst = false, g_monitor = false, g_sc = false;
int g_attempts = 0;

// Монитор запасных путей: решает, когда они «взведены». Запасной путь работает только пока BufferQueue
// молчит: либо его хуки не встали, либо кадров от него нет fallback_ms с запуска.
void monitor_loop() {
    for (;;) {
        usleep(500 * 1000);
        if (!g_cfg.enable) continue;
        // GPU-процесс браузера: кадры идут слоями SurfaceControl (sc_capture.cpp) или через окно (хук Surface).
        // eglSwapBuffers/vkQueuePresentKHR запасного пути перехватили бы внутренние поверхности самого Chromium.
        if (myfg::auxProcess() || capture::scArmed()) {
            fallback::setEglActive(false);
            fallback::setVkActive(false);
            continue;
        }
        const bool bq_failed = !g_bq && g_attempts >= 5;
        const int64_t since = (now_ns() - capture::startNs()) / 1000000;
        const bool silent = !capture::bqActive() && (bq_failed || since >= g_cfg.fallback_ms);

        if (g_cfg.hook_swap != 0) {
            if (!g_swap_inst && (g_cfg.hook_swap >= 2 || silent)) g_swap_inst = fallback::installEgl();
            fallback::setEglActive(g_swap_inst && silent);
        } else {
            fallback::setEglActive(false);
        }
        if (g_cfg.hook_present != 0 && g_present_inst) fallback::setVkActive(silent);
        else fallback::setVkActive(false);
    }
}

// 0 — выкл, 1 — только если хук Surface не встал, 2 — всегда
bool wanted(int v, bool surface_hooked) { return v >= 2 || (v == 1 && !surface_hooked); }

}  // namespace

namespace hooks {

void noteWindow(ANativeWindow* w) {
    if (!w) return;
    std::lock_guard<std::mutex> g(g_wm);
    for (int i = 0; i < kWin; i++) {
        if (g_win[i] == w) {                      // уже есть — поднять наверх
            for (int k = i; k > 0; k--) g_win[k] = g_win[k - 1];
            g_win[0] = w;
            return;
        }
    }
    if (g_win[kWin - 1]) ANativeWindow_release(g_win[kWin - 1]);
    for (int k = kWin - 1; k > 0; k--) g_win[k] = g_win[k - 1];
    ANativeWindow_acquire(w);
    g_win[0] = w;
    LOGD("window noted %p (%dx%d)", w, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
}

ANativeWindow* acquireIfKnown(ANativeWindow* w) {
    if (!w) return nullptr;
    std::lock_guard<std::mutex> g(g_wm);
    for (int i = 0; i < kWin; i++)
        if (g_win[i] == w) { ANativeWindow_acquire(w); return w; }
    return nullptr;
}

ANativeWindow* pickWindow(int w, int h) {
    std::lock_guard<std::mutex> g(g_wm);
    auto near = [](int a, int b) { return abs(a - b) <= (b >> 4) + 8; };
    ANativeWindow* best = nullptr;
    for (int i = 0; i < kWin && !best; i++) {
        if (!g_win[i]) continue;
        int ww = ANativeWindow_getWidth(g_win[i]), wh = ANativeWindow_getHeight(g_win[i]);
        if (ww > 0 && wh > 0 && w > 0 && h > 0 &&
            ((near(ww, w) && near(wh, h)) || (near(ww, h) && near(wh, w))))
            best = g_win[i];
    }
    if (!best) best = g_win[0];
    if (best) ANativeWindow_acquire(best);
    return best;
}

bool tryInstall() {
    g_attempts++;
    if (!g_bq) g_bq = capture::install();
    static bool started = false;
    // Конвейер нужен и в режиме «только запасные пути»: BufferQueue-хуки не встали, а EGL/Vulkan-запас включён.
    const bool fb_only = !g_bq && g_attempts >= 5 && (g_cfg.hook_swap != 0 || g_cfg.hook_present != 0);
    // Захват слоёв SurfaceControl (GPU-процесс браузера): true — повторять не нужно.
    if (!g_sc) g_sc = capture::installSc();
    if ((g_bq || fb_only || capture::scArmed()) && !started) { started = true; pipeline::start(); }
    if (!g_monitor) { g_monitor = true; std::thread(monitor_loop).detach(); }
    const bool surf = capture::surfaceHooked();     // решение окончательное, как только g_bq == true
    // BufferQueue-путь «определился»: либо встал, либо за несколько попыток так и не встал.
    const bool settled = g_bq || g_attempts >= 5;
    const bool bq_failed = !g_bq && g_attempts >= 5;

    if (settled && (wanted(g_cfg.hook_egl, surf) || g_cfg.hook_swap != 0) && !g_egl) {
        size_t sz = 0;
        if (void* p = resolve_symbol_ex("libEGL.so", "eglCreateWindowSurface", false, &sz))
            g_egl = inline_hook("eglCreateWindowSurface", p, sz, false, (void*)hook_cws, (void**)&orig_cws);
    }
    if (settled && (wanted(g_cfg.hook_anw, surf) || g_cfg.hook_present != 0) && !g_anw) {
        size_t sz = 0;
        if (void* p = resolve_symbol_ex("libandroid.so", "ANativeWindow_fromSurface", false, &sz))
            g_anw = inline_hook("ANativeWindow_fromSurface", p, sz, false, (void*)hook_fromSurface, (void**)&orig_fromSurface);
    }
    // Vulkan-запас: хук на vkCreateDevice обязан стоять ДО создания устройства игры.
    const bool want_present = g_cfg.hook_present >= 2 || (g_cfg.hook_present == 1 && bq_failed);
    if (want_present && !g_present_inst && !fallback::vkFailed()) g_present_inst = fallback::installVk();
    if (g_cfg.hook_vk && !g_vk && !g_vk_gave_up) {
        // Только при явном hook_vk=1, и только если ELF подтверждает, что функция >= 16 байт.
        size_t gs = 0, ds = 0;
        void* gipa = resolve_symbol_ex("libvulkan.so", "vkGetInstanceProcAddr", false, &gs);
        void* gdpa = resolve_symbol_ex("libvulkan.so", "vkGetDeviceProcAddr", false, &ds);
        if (gipa) {
            bool a = inline_hook("vkGetInstanceProcAddr", gipa, gs, true, (void*)hook_gipa, (void**)&orig_gipa);
            bool c = gdpa && inline_hook("vkGetDeviceProcAddr", gdpa, ds, true, (void*)hook_gdpa, (void**)&orig_gdpa);
            g_vk = a || c;
            g_vk_gave_up = !g_vk;           // отказ — окончательный, не пытаемся 2 минуты подряд
            LOGI("vulkan swapchain hooks: gipa=%d gdpa=%d%s", a, c, g_vk ? "" : " (disabled: functions too short or hook failed)");
        }
    }
    const bool need_vk = g_cfg.hook_vk && !g_vk && !g_vk_gave_up;
    const bool need_present = want_present && !g_present_inst && !fallback::vkFailed();
    return g_bq && g_sc && !need_vk && !need_present;      // EGL/ANW — "лучшее из возможного", не блокируют завершение
}

}  // namespace hooks
