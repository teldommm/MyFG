#include "gl_island.h"
#include "common.h"

// Механика перенесена из qcom_me.cpp (там она была приватной), без изменений поведения:
// тот же порядок инициализации, те же запасные dlopen'ы и та же серверная схема ожидания.
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#endif
#ifndef EGL_PBUFFER_BIT
#define EGL_PBUFFER_BIT 0x0001
#endif

namespace glIsland {

namespace {
struct Island {
    bool inited = false;             // пытались
    bool ready = false;              // работает
    bool reasonLogged = false;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    Fns f;
};
Island g;

void die(const char* why) {
    if (!g.reasonLogged) LOGW("gl-island: %s", why);
    g.reasonLogged = true;
    g.ready = false;
}
}  // namespace

const Fns& fns() { return g.f; }
EGLDisplay display() { return g.dpy; }

bool available() { return g.ready; }

bool ensure() {
    if (g.inited) return g.ready;
    g.inited = true;

    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!egl) egl = dlopen("libEGL_android.so", RTLD_NOW | RTLD_LOCAL);
    if (!egl) return (die("libEGL not loadable"), false);

    g.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!g.dpy) return (die("no default EGL display"), false);
    if (!eglInitialize(g.dpy, nullptr, nullptr)) return (die("eglInitialize failed"), false);

    // (адреса берём через eglGetProcAddress — в заголовках NDK для API 29 их прототипов нет)
    g.f.CreateImageKHR = reinterpret_cast<decltype(g.f.CreateImageKHR)>(eglGetProcAddress("eglCreateImageKHR"));
    g.f.DestroyImageKHR = reinterpret_cast<decltype(g.f.DestroyImageKHR)>(eglGetProcAddress("eglDestroyImageKHR"));
    g.f.ImageTargetTex = reinterpret_cast<decltype(g.f.ImageTargetTex)>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    g.f.CreateSyncKHR = reinterpret_cast<decltype(g.f.CreateSyncKHR)>(eglGetProcAddress("eglCreateSyncKHR"));
    g.f.DestroySyncKHR = reinterpret_cast<decltype(g.f.DestroySyncKHR)>(eglGetProcAddress("eglDestroySyncKHR"));
    g.f.WaitSyncKHR = reinterpret_cast<decltype(g.f.WaitSyncKHR)>(eglGetProcAddress("eglWaitSyncKHR"));
    g.f.DupFenceFD = reinterpret_cast<decltype(g.f.DupFenceFD)>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (!g.f.CreateImageKHR || !g.f.DestroyImageKHR || !g.f.ImageTargetTex)
        return (die("EGLImage import path missing"), false);

    const EGLint cfgAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(g.dpy, cfgAttrs, &cfg, 1, &n) || n < 1) return (die("no EGL config"), false);
    const EGLint pbAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    g.surf = eglCreatePbufferSurface(g.dpy, cfg, pbAttrs);
    if (g.surf == EGL_NO_SURFACE) return (die("pbuffer surface failed"), false);
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.ctx = eglCreateContext(g.dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (g.ctx == EGL_NO_CONTEXT) return (die("ES3 context failed"), false);
    if (!eglMakeCurrent(g.dpy, g.surf, g.surf, g.ctx)) return (die("eglMakeCurrent failed (pipeline thread busy?)"), false);

    if (glGetError() != GL_NO_ERROR) return (die("GL error during init"), false);
    g.ready = true;
    LOGD("gl-island: up (%s)", (const char*)glGetString(GL_VERSION));
    return true;
}

bool hasExt(const char* name) {
    if (!g.ready || !name) return false;
    typedef const GLubyte* (*PFN_gsi)(GLenum, GLuint);
    PFN_gsi getStringi = (PFN_gsi)dlsym(RTLD_DEFAULT, "glGetStringi");
    if (getStringi) {
        GLint cnt = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &cnt);
        for (GLint i = 0; i < cnt; i++) {
            const char* e = (const char*)getStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && strstr(e, name)) return true;
        }
        return false;
    }
    const char* all = (const char*)glGetString(GL_EXTENSIONS);
    return all && strstr(all, name);
}

bool waitFence(int fd) {
    if (fd < 0) return true;
    if (g.f.CreateSyncKHR && g.f.WaitSyncKHR && g.f.DestroySyncKHR) {
        int d = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (d >= 0) {
            EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, d, EGL_NONE};
            EGLSyncKHR s = g.f.CreateSyncKHR(g.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
            if (s) {                     // EGL забрал dup: ждёт GPU, CPU не стоит
                const bool ok = g.f.WaitSyncKHR(g.dpy, s, 0) != EGL_FALSE;
                g.f.DestroySyncKHR(g.dpy, s);
                if (ok) return true;     // серверное ожидание: дальнейшие GL-команды после fence
            } else close(d);
        }
    }
    // запасной путь: CPU-ожидание (fence обычно уже подан — захват был миллисекунды назад)
    for (int i = 0; i < 25; i++) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 1) > 0) return true;
    }
    return false;
}

int exportFence() {
    if (!g.ready) return -1;
    if (!g.f.CreateSyncKHR || !g.f.DupFenceFD || !g.f.DestroySyncKHR) {
        glFinish();                       // без native-fence пути остаётся только блокирующее ожидание
        return -1;
    }
    // Канонический порядок (EGL_ANDROID_native_fence_sync): sync вставляет fence в текущую
    // точку потока команд, glFlush отправляет всё до него, dup отдаёт fd. CPU не блокируется.
    EGLSyncKHR s = g.f.CreateSyncKHR(g.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (!s) { glFinish(); return -1; }
    glFlush();
    const int fd = (int)g.f.DupFenceFD(g.dpy, s);
    g.f.DestroySyncKHR(g.dpy, s);
    if (fd < 0) glFinish();               // dup не удался (не подтянутся?) — гарантируем завершение
    return fd;
}

}  // namespace glIsland
