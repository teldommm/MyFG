// Запасной путь захвата для GLES-игр: хук eglSwapBuffers / eglSwapBuffersWithDamageKHR.
//
//  Когда нужен: BufferQueue-хуки libgui не встали (другая версия Android) или игра не отдаёт кадры через них.
//  Как работает: перед оригинальным swap в контексте игры копируем default framebuffer (FBO 0) в текстуру,
//  созданную из EGLImage поверх нашего AHardwareBuffer (кольцо из kN буферов), берём native fence
//  (EGL_SYNC_NATIVE_FENCE_ANDROID) и отдаём конвейеру обычное задание capture::Job. Дальше всё как у BufferQueue.
//
//  Почему на потоке игры: default framebuffer доступен только в контексте, к которому привязана поверхность.
//  Состояние GL, которое мы трогаем (FBO READ/DRAW, scissor, read buffer, привязка текстуры), сохраняется и
//  восстанавливается. Тяжёлой работы нет: один blit и dup fence.
//
//  Кольцо привязано к одному контексту (первому, кто стал источником). Другой контекст — кадр пропускается.
//  Правило: никакого thread_local (Zygisk-загрузчик не обрабатывает TLSDESC) — защита от рекурсии на pthread_key.
#include "fallback.h"
#include "capture.h"
#include "common.h"
#include "config.h"
#include "hooks.h"
#include "util.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <atomic>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr int kN = 4;                    // слотов в кольце
constexpr int kMaxSurf = 16;

// ---- таблица функций (через dlsym: libEGL/libGLESv3 уже загружены игрой) ------------------------------
struct Fns {
    EGLContext (*GetCurrentContext)() = nullptr;
    EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = nullptr;
    EGLBoolean (*QueryContext)(EGLDisplay, EGLContext, EGLint, EGLint*) = nullptr;
    __eglMustCastToProperFunctionPointerType (*GetProcAddress)(const char*) = nullptr;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC GetNativeClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC CreateImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC DestroyImage = nullptr;
    PFNEGLCREATESYNCKHRPROC CreateSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC DestroySync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC DupFence = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ImageTargetTex = nullptr;
    // GLES
    void (*GetIntegerv)(GLenum, GLint*) = nullptr;
    void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
    void (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
    void (*GenTextures)(GLsizei, GLuint*) = nullptr;
    void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void (*BindTexture)(GLenum, GLuint) = nullptr;
    GLboolean (*IsEnabled)(GLenum) = nullptr;
    void (*Enable)(GLenum) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    void (*Flush)() = nullptr;
    void (*Finish)() = nullptr;
    void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
    void (*ReadBuffer)(GLenum) = nullptr;
    void (*CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) = nullptr;
    bool ok = false;
} F;

bool load_fns() {
    if (F.ok) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    void* gl3 = dlopen("libGLESv3.so", RTLD_NOW);
    void* gl2 = dlopen("libGLESv2.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl || (!gl3 && !gl2)) return false;
#define E(n) F.n = (decltype(F.n))dlsym(egl, "egl" #n);
    E(GetCurrentContext) E(QuerySurface) E(QueryContext) E(GetProcAddress)
#undef E
    auto gl = [&](const char* n) -> void* {
        void* p = gl3 ? dlsym(gl3, n) : nullptr;
        if (!p && gl2) p = dlsym(gl2, n);
        return p;
    };
#define G(n) F.n = (decltype(F.n))gl("gl" #n);
    G(GetIntegerv) G(BindFramebuffer) G(GenFramebuffers) G(DeleteFramebuffers) G(FramebufferTexture2D)
    G(CheckFramebufferStatus) G(GenTextures) G(DeleteTextures) G(BindTexture) G(IsEnabled) G(Enable) G(Disable)
    G(Flush) G(Finish) G(BlitFramebuffer) G(ReadBuffer) G(CopyTexSubImage2D)
#undef G
    if (!F.GetProcAddress) return false;
#define P(n, s) F.n = (decltype(F.n))F.GetProcAddress(s);
    P(GetNativeClientBuffer, "eglGetNativeClientBufferANDROID") P(CreateImage, "eglCreateImageKHR")
    P(DestroyImage, "eglDestroyImageKHR") P(CreateSync, "eglCreateSyncKHR") P(DestroySync, "eglDestroySyncKHR")
    P(DupFence, "eglDupNativeFenceFDANDROID") P(ImageTargetTex, "glEGLImageTargetTexture2DOES")
#undef P
    if (!F.GetCurrentContext || !F.QuerySurface || !F.QueryContext || !F.GetNativeClientBuffer || !F.CreateImage ||
        !F.DestroyImage || !F.CreateSync || !F.DestroySync || !F.DupFence || !F.ImageTargetTex || !F.GetIntegerv ||
        !F.BindFramebuffer || !F.GenFramebuffers || !F.FramebufferTexture2D || !F.GenTextures || !F.BindTexture ||
        !F.IsEnabled || !F.Enable || !F.Disable || !F.Flush || !F.Finish)
        return false;
    F.ok = true;
    return true;
}

// ---- состояние ------------------------------------------------------------------------------------------
struct Slot {
    AHardwareBuffer* ahb = nullptr;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
    std::atomic<int> busy{0};
};
struct Ring {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    int w = 0, h = 0;
    bool es3 = false;
    Slot s[kN];
    bool valid = false;
    std::atomic<uint32_t> gen{0};          // поколение кольца: запоздалый done() от старого кольца не трогает новое
};
Ring g_ring;
std::mutex g_m;                                       // кольцо создаётся/удаляется под ним; busy — атомарные

struct SurfWin { EGLSurface s; ANativeWindow* w; };
SurfWin g_sw[kMaxSurf];
int g_sw_n = 0;
std::mutex g_swm;

std::atomic<bool> g_active{false};
std::atomic<bool> g_installed{false};
pthread_key_t g_guard_key;
bool g_guard_ok = false;

EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface) = nullptr;
EGLBoolean (*orig_swap_dmg)(EGLDisplay, EGLSurface, const EGLint*, EGLint) = nullptr;
EGLBoolean (*orig_swap_dmg_ext)(EGLDisplay, EGLSurface, const EGLint*, EGLint) = nullptr;

std::atomic<uint64_t> g_frames{0}, g_dropped{0}, g_fail{0};

void slot_done(void* ctx, int tag) {
    (void)ctx;
    if (tag >= 0 && ((uint32_t)tag >> 3) == (g_ring.gen.load() & 0xFFFFF)) g_ring.s[tag & 7].busy.store(0);
}

void destroy_ring_locked() {
    if (!g_ring.valid && !g_ring.s[0].ahb) return;
    // GL-объекты удаляем только в том контексте, где создали
    const bool same_ctx = F.GetCurrentContext && F.GetCurrentContext() == g_ring.ctx && g_ring.ctx != EGL_NO_CONTEXT;
    for (auto& s : g_ring.s) {
        if (same_ctx && s.fbo && F.DeleteFramebuffers) F.DeleteFramebuffers(1, &s.fbo);
        if (same_ctx && s.tex && F.DeleteTextures) F.DeleteTextures(1, &s.tex);
        if (s.img != EGL_NO_IMAGE_KHR && F.DestroyImage) F.DestroyImage(g_ring.dpy, s.img);
        if (s.ahb) AHardwareBuffer_release(s.ahb);      // задание может ещё держать свою ссылку — буфер живёт до её release
        s.fbo = 0; s.tex = 0; s.img = EGL_NO_IMAGE_KHR; s.ahb = nullptr;
        s.busy.store(0);
    }
    g_ring.valid = false;
}

bool build_ring_locked(EGLDisplay d, EGLContext ctx, int w, int h) {
    destroy_ring_locked();
    g_ring.gen.fetch_add(1);
    EGLint ver = 2;
    F.QueryContext(d, ctx, EGL_CONTEXT_CLIENT_VERSION, &ver);
    g_ring.es3 = ver >= 3 && F.BlitFramebuffer && F.ReadBuffer;
    if (!g_ring.es3 && !F.CopyTexSubImage2D) return false;

    // ES3: READ и DRAW framebuffer — независимые точки привязки. Биндим/восстанавливаем обе
    // по отдельности: единый GL_FRAMEBUFFER в этой версии затирал бы READ-биндинг игры
    // (glBindFramebuffer(GL_FRAMEBUFFER, ...) меняет оба, а сохранялся только DRAW).
    GLint prevTex = 0, prevRead = 0, prevDraw = 0;
    F.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    if (g_ring.es3) {
        F.GetIntegerv(0x8CAA /*GL_READ_FRAMEBUFFER_BINDING*/, &prevRead);
        F.GetIntegerv(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &prevDraw);
    } else {
        F.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevDraw);
        prevRead = prevDraw;
    }
    const GLenum fboT = g_ring.es3 ? 0x8CA9 /*GL_DRAW_FRAMEBUFFER*/ : GL_FRAMEBUFFER;
    bool ok = true;
    for (int i = 0; i < kN && ok; i++) {
        Slot& s = g_ring.s[i];
        AHardwareBuffer_Desc desc{};
        desc.width = (uint32_t)w; desc.height = (uint32_t)h; desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&desc, &s.ahb) != 0 || !s.ahb) { ok = false; break; }
        EGLClientBuffer cb = F.GetNativeClientBuffer(s.ahb);
        const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        s.img = cb ? F.CreateImage(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs) : EGL_NO_IMAGE_KHR;
        if (s.img == EGL_NO_IMAGE_KHR) { ok = false; break; }
        F.GenTextures(1, &s.tex);
        F.BindTexture(GL_TEXTURE_2D, s.tex);
        F.ImageTargetTex(GL_TEXTURE_2D, (GLeglImageOES)s.img);
        F.GenFramebuffers(1, &s.fbo);
        F.BindFramebuffer(fboT, s.fbo);
        F.FramebufferTexture2D(fboT, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
        if (F.CheckFramebufferStatus && F.CheckFramebufferStatus(fboT) != GL_FRAMEBUFFER_COMPLETE) ok = false;
    }
    F.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    if (g_ring.es3) {
        F.BindFramebuffer(0x8CA8 /*GL_READ_FRAMEBUFFER*/, (GLuint)prevRead);
        F.BindFramebuffer(0x8CA9 /*GL_DRAW_FRAMEBUFFER*/, (GLuint)prevDraw);
    } else {
        F.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevDraw);
    }
    g_ring.dpy = d; g_ring.ctx = ctx; g_ring.w = w; g_ring.h = h;
    g_ring.valid = ok;
    if (!ok) { LOGW("egl fallback: ring %dx%d could not be built", w, h); destroy_ring_locked(); }
    else LOGD("egl fallback: ring %dx%d x%d (%s)", w, h, kN, g_ring.es3 ? "glBlitFramebuffer" : "glCopyTexSubImage2D");
    return ok;
}

ANativeWindow* window_for(EGLSurface s, int w, int h) {
    ANativeWindow* known = nullptr;
    {
        std::lock_guard<std::mutex> g(g_swm);
        for (int i = 0; i < g_sw_n; i++) if (g_sw[i].s == s) { known = g_sw[i].w; break; }
    }
    if (known) {
        ANativeWindow* r = hooks::acquireIfKnown(known);    // окно могли вытеснить из реестра
        if (r) return r;
    }
    return hooks::pickWindow(w, h);                           // эвристика по размеру (с взятой ссылкой) или nullptr
}

// Копирование кадра. Возвращает управление игре в прежнем состоянии GL.
void capture_frame(EGLDisplay d, EGLSurface s) {
    if (!g_active.load(std::memory_order_relaxed) || !g_cfg.enable) return;
    if (capture::bqActive()) return;
    if (!load_fns()) return;
    EGLContext ctx = F.GetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || s == EGL_NO_SURFACE) return;
    EGLint w = 0, h = 0;
    if (!F.QuerySurface(d, s, EGL_WIDTH, &w) || !F.QuerySurface(d, s, EGL_HEIGHT, &h)) return;
    if (w < 256 || h < 128 || w > 8192 || h > 8192) return;      // pbuffer-ы загрузчика, мелкие служебные поверхности

    ANativeWindow* win = window_for(s, w, h);
    const bool elected = capture::extOffer(s, w, h, win);
    if (win) ANativeWindow_release(win);
    if (!elected) return;

    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ring.valid || g_ring.ctx != ctx || g_ring.w != w || g_ring.h != h) {
        if (g_ring.valid && g_ring.ctx != ctx) {                  // другой контекст: не трогаем чужое кольцо
            static uint64_t n = 0;
            if (n++ % 600 == 0) LOGW("egl fallback: frame from another context skipped");
            return;
        }
        // задания, держащие слоты старого размера, дойдут до release сами (ссылки на AHB у них свои)
        if (!build_ring_locked(d, ctx, w, h)) { g_fail++; return; }
    }
    int idx = -1;
    for (int i = 0; i < kN; i++) if (g_ring.s[i].busy.load() == 0) { idx = i; break; }
    if (idx < 0) { g_dropped++; return; }
    Slot& sl = g_ring.s[idx];

    // ---- GL: сохранить -> blit -> восстановить ----
    GLint prevRead = 0, prevDraw = 0, prevReadBuf = GL_BACK;
    if (g_ring.es3) {
        F.GetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &prevRead);
        F.GetIntegerv(0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/, &prevDraw);
        F.GetIntegerv(0x0C02 /*READ_BUFFER*/, &prevReadBuf);
    } else {
        F.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevDraw);
        prevRead = prevDraw;
    }
    const GLboolean scissor = F.IsEnabled(GL_SCISSOR_TEST);
    if (scissor) F.Disable(GL_SCISSOR_TEST);
    uint32_t transform = 0;
    if (g_ring.es3) {
        F.BindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, 0);
        F.ReadBuffer(GL_BACK);
        F.BindFramebuffer(0x8CA9 /*DRAW_FRAMEBUFFER*/, sl.fbo);
        // origin FBO 0 снизу-слева, буфер AHB читается сверху-вниз: переворачиваем при копировании
        F.BlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        F.BindFramebuffer(0x8CA8, (GLuint)prevRead);
        F.BindFramebuffer(0x8CA9, (GLuint)prevDraw);
        F.ReadBuffer((GLenum)prevReadBuf);
    } else {
        // ES2: копируем из FBO 0 в текстуру слота; переворот отдаём слою (transform = FLIP_V)
        GLint prevTex = 0;
        F.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        F.BindFramebuffer(GL_FRAMEBUFFER, 0);
        F.BindTexture(GL_TEXTURE_2D, sl.tex);
        F.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        F.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        F.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevDraw);
        transform = 2;                                             // ANATIVEWINDOW_TRANSFORM_FLIP_V
    }
    if (scissor) F.Enable(GL_SCISSOR_TEST);

    // ---- fence ----
    int fd = -1;
    EGLSyncKHR sync = F.CreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    F.Flush();
    if (sync != EGL_NO_SYNC_KHR) {
        fd = F.DupFence(d, sync);
        F.DestroySync(d, sync);
        if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) fd = -1;
    }
    if (fd < 0) F.Finish();                                         // без fence — ждём на CPU, зато кадр целый

    sl.busy.store(1);
    capture::Job j;
    AHardwareBuffer_acquire(sl.ahb);
    j.src = sl.ahb;
    j.fence_fd = fd;
    j.cl = 0; j.ct = 0; j.cr = w; j.cb = h;
    j.bw = w; j.bh = h;
    j.ts_ns = now_ns();
    j.transform = transform;
    j.dataspace = 0;
    j.done = slot_done; j.done_ctx = nullptr; j.done_tag = (int)(((g_ring.gen.load() & 0xFFFFF) << 3) | (uint32_t)idx);
    uint64_t n = g_frames++;
    if (n < 4) LOGD("egl fallback frame #%llu %dx%d slot=%d fence=%d", (unsigned long long)n, w, h, idx, fd);
    capture::extSubmit(j);
}

// ---- хуки --------------------------------------------------------------------------------------------
bool guard_enter() {
    if (!g_guard_ok) return false;
    if (pthread_getspecific(g_guard_key)) return false;      // уже внутри нашего хука (eglSwapBuffers -> ...WithDamage)
    pthread_setspecific(g_guard_key, (void*)1);
    return true;
}
void guard_leave() { if (g_guard_ok) pthread_setspecific(g_guard_key, nullptr); }

EGLBoolean hook_swap(EGLDisplay d, EGLSurface s) {
    const bool top = guard_enter();
    if (top) capture_frame(d, s);
    EGLBoolean r = orig_swap(d, s);
    if (top) guard_leave();
    return r;
}
EGLBoolean hook_swap_dmg(EGLDisplay d, EGLSurface s, const EGLint* rects, EGLint n) {
    const bool top = guard_enter();
    if (top) capture_frame(d, s);
    EGLBoolean r = orig_swap_dmg(d, s, rects, n);
    if (top) guard_leave();
    return r;
}
EGLBoolean hook_swap_dmg_ext(EGLDisplay d, EGLSurface s, const EGLint* rects, EGLint n) {
    const bool top = guard_enter();
    if (top) capture_frame(d, s);
    EGLBoolean r = orig_swap_dmg_ext(d, s, rects, n);
    if (top) guard_leave();
    return r;
}

}  // namespace

namespace fallback {

void noteEglSurface(void* surface, ANativeWindow* win) {
    if (!surface || !win) return;
    std::lock_guard<std::mutex> g(g_swm);
    for (int i = 0; i < g_sw_n; i++) if (g_sw[i].s == surface) { g_sw[i].w = win; return; }
    if (g_sw_n == kMaxSurf) { for (int i = 1; i < kMaxSurf; i++) g_sw[i - 1] = g_sw[i]; g_sw_n--; }
    g_sw[g_sw_n++] = {surface, win};
}

void setEglActive(bool on) {
    if (g_active.exchange(on) != on) LOGI("egl fallback capture %s", on ? "armed" : "disarmed");
}

bool installEgl() {
    if (g_installed.load()) return true;
    if (!g_guard_ok) g_guard_ok = pthread_key_create(&g_guard_key, nullptr) == 0;
    if (!g_guard_ok) return false;
    bool any = false;
    struct { const char* name; void* repl; void** orig; } t[] = {
        {"eglSwapBuffersWithDamageKHR", (void*)hook_swap_dmg, (void**)&orig_swap_dmg},
        {"eglSwapBuffersWithDamageEXT", (void*)hook_swap_dmg_ext, (void**)&orig_swap_dmg_ext},
        {"eglSwapBuffers", (void*)hook_swap, (void**)&orig_swap},
    };
    // Если eglSwapBuffers — короткая заглушка с tail-call в ...WithDamageKHR, inline_hook() её откажет
    // (функция < 16 байт), и кадры всё равно придут через хук WithDamage.
    for (auto& e : t) {
        size_t sz = 0;
        void* p = resolve_symbol_ex("libEGL.so", e.name, false, &sz);
        if (!p) continue;
        if (inline_hook(e.name, p, sz, false, e.repl, e.orig)) any = true;
    }
    if (any) { g_installed.store(true); LOGI("egl fallback hooks installed"); }
    return any;
}

}  // namespace fallback
