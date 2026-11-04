#pragma once
// Счётчик FPS поверх игры: собственный ASurfaceControl-слой "myfg_hud" (ребёнок окна игры, выше слоя
// оверлея). Текст рисуется на CPU встроенным растровым шрифтом 5x7 в AHardwareBuffer и обновляется
// дважды в секунду; сам слой не зависит от Vulkan и конвейера генерации, кроме счётчиков кадров.
//
// Показывает (hud_mode): 0 — итоговый FPS; 1 — "игра>вывод"; 2 — построчно: FPS, GAME, GEN (сколько кадров
// в секунду добавлено модулем), GPU (время нашего прохода).
#include <android/native_window.h>
#include <stdint.h>

namespace hud {

struct Source {
    // Окно игры с увеличенным счётчиком ссылок (вызывающий отпускает) либо nullptr.
    ANativeWindow* (*acquire_window)(void* ctx);
    // Накопленные счётчики: кадры игры, сгенерированные кадры, показанные кадры; время GPU, мс (0 — нет данных).
    void (*counters)(void* ctx, uint64_t* src, uint64_t* gen, uint64_t* shown, double* gpu_ms);
    void* ctx;
};

// Запускает поток счётчика, если он ещё не работает (можно вызывать часто). Настройки читает из g_cfg:
// hud_mode, hud_pos, hud_scale, hud_tf, zorder.
void ensure(const Source& s);
// Просит поток остановиться: слой прячется и освобождается. Безопасно вызывать часто.
void stop();

}  // namespace hud
