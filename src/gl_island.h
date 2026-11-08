#pragma once
// Общий GL-остров модуля: собственный EGL-контекст, не зависящий от наличия конкретного
// QCOM-расширения. Остров = собственный EGL-контекст (ES3, 1x1
// pbuffer, не пересекается с контекстами игры), создаётся лениво на потоке конвейера и
// используется qcom_ex (он живёт на этом же потоке, поэтому eglMakeCurrent выполняется один раз).
//
// Здесь же — общий синтаксис fence'ей:
//   waitFence(fd)   — серверное ожидание acquire-fence игры (eglWaitSync: GPU ждёт, CPU нет);
//   exportFence()   — glFlush + eglDupNativeFenceFD: sync_fd завершения всех уже отданных
//                     команд контекста (для OutItem). -1 = готово сразу / ошибки нет.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>     // GLeglImageOES (тип EGLImage-текстур)
#include <android/hardware_buffer.h>
#include <stdint.h>

namespace glIsland {

// Адреса, резолвятся в ensure() (все — eglGetProcAddress: их нет в заголовках NDK для API 29).
struct Fns {
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*) = nullptr;
    EGLBoolean  (*DestroyImageKHR)(EGLDisplay, EGLImageKHR) = nullptr;
    void        (*ImageTargetTex)(GLenum, GLeglImageOES) = nullptr;   // glEGLImageTargetTexture2DOES
    EGLSyncKHR  (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint*) = nullptr;
    EGLBoolean  (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR) = nullptr;
    EGLBoolean  (*WaitSyncKHR)(EGLDisplay, EGLSync, EGLint) = nullptr;
    EGLint      (*DupFenceFD)(EGLDisplay, EGLSyncKHR) = nullptr;      // eglDupNativeFenceFDANDROID
};

// Ленивая инициализация (один раз, на потоке конвейера). false — остров недоступен на этом
// устройстве (причина пишется в лог единожды): нет EGL, не поднялся ES3-контекст, нет
// EGLImage/native-fence пути. Sticky до конца процесса.
bool ensure();
bool available();
const Fns& fns();
EGLDisplay display();          // EGL_NO_DISPLAY до ensure()

// Есть ли расширение у драйвера (полный список glGetStringi; только после ensure()).
bool hasExt(const char* name);

// Серверное ожидание native fence (acquire-fence игры): dup'ается внутри, fd остаётся во
// владении вызывающего. false — контент так и не готов (CPU-fallback тоже не дождался).
bool waitFence(int fd);

// Экспорт завершения уже отданных GL-команд этого контекста: sync создаётся в текущей точке
// потока команд, glFlush отправляет всё до него, dup отдаёт fd наружу. -1 легально (всё уже
// завершено): OutItem с fence_fd=-1 показывает буфер сразу.
int exportFence();

}  // namespace glIsland
