#pragma once
// Запасные пути захвата кадров: используются, только если BufferQueue-путь (capture.cpp) недоступен
// или не отдаёт кадры. Как только BufferQueue начинает отдавать кадры (capture::bqActive()), эти пути молчат.
#include <android/native_window.h>
#include <stdint.h>

namespace fallback {

// ---- EGL: eglSwapBuffers / eglSwapBuffersWithDamageKHR (fallback_egl.cpp) ------------------------------
// Ставит хуки (идемпотентно). true — хотя бы один хук swap стоит.
bool installEgl();
// Включает/выключает копирование кадров (хуки остаются на месте, в выключенном состоянии это проброс).
void setEglActive(bool on);
// eglCreateWindowSurface вернул поверхность для окна: запоминаем соответствие.
void noteEglSurface(void* surface, ANativeWindow* win);

// ---- Vulkan: vkCreateDevice / vkCreateSwapchainKHR / vkQueuePresentKHR (fallback_vk.cpp) -------------------
// Ставит хуки на libvulkan. Важно: чтобы копировать кадры устройством игры, vkCreateDevice игры должен пройти
// через наш хук (там добавляются расширения AHB), поэтому ставить надо ДО создания устройства (hook_present=2),
// либо сразу, если BufferQueue-хуки недоступны.
bool installVk();
void setVkActive(bool on);
// true — установка окончательно отказана (хук vkCreateDevice не принят): повторять не нужно.
bool vkFailed();
// Помечает вызывающий поток как «поток модуля»: vkCreateDevice на нём (собственное устройство модуля)
// хук пропускает без изменений. Вызывать из потока конвейера до VkCtx::init().
void markOwnThread();

}  // namespace fallback
