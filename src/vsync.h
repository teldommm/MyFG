#pragma once
// Поток с ALooper + AChoreographer. На каждый vsync вызывает callback с таймлайнами кадров
// (id, ожидаемое время показа, дедлайн) — API 33+ (postVsyncCallback). На более старых версиях —
// postFrameCallback64 без таймлайнов (present оценивается как frameTime + 2*period).
//
// Фазовая привязка: ведём собственную сетку vsync (next_ns),
// экстраполируемую по оценённому периоду. На каждом реальном тике считаем фазовую ошибку
// phase = frame_ns - next_ns:
//   |phase| > kSnapNs (5 мс) — жёсткий ре-лок: next_ns = frame_ns + period (дрейф не копится,
//                            сетка отбрасывается и заводится заново от реального тика);
//   иначе — плавное следование: next_ns += period + phase/8 (поглощаем 1/8 ошибки за тик).
// Это стабилизирует интерполяцию vsync-времён между колбэками и переживает пропуски тиков
// Choreographer (пауза, джиттер шедулера, смена частоты экрана) без накопления фазового сдвига.
#include <stdint.h>

struct VsyncInfo {
    static constexpr int kMax = 8;
    int      n = 0;                    // число таймлайнов (0 = нет данных, fallback)
    int64_t  id[kMax] = {};
    int64_t  present[kMax] = {};       // ожидаемое время показа, CLOCK_MONOTONIC
    int64_t  deadline[kMax] = {};
    int64_t  frame_ns = 0;             // время начала кадра
    int64_t  period_ns = 16666667;     // оценка периода vsync
    bool     has_timeline = false;

    // Фазовая сетка (см. шапку файла)
    int64_t  next_ns = 0;              // прогноз времени следующего vsync
    int64_t  phase_ns = 0;             // фазовая ошибка этого тика (real - прогноз), signed
    bool     snapped = false;          // true = на этом тике был жёсткий ре-лок (> 5 мс)
};

class Vsync {
public:
    typedef void (*Callback)(const VsyncInfo&, void* user);
    bool start(Callback cb, void* user);
    void stop();
    // Пауза: колбэки Choreographer не перепоставляются (поток не просыпается на каждый vsync). Возобновление — до ~100 мс.
    void pause(bool on) { paused_ = on; }
    bool running() const { return running_; }

    // Экстраполяция сетки на произвольный момент: ближайший будущий vsync >= now_ns.
    // Считается от ведущейся сетки (без её сдвига); если данные ещё не набраны —
    // возвращает now_ns + period/2. Потокобезопасно (atomics).
    int64_t nextVsyncAfter(int64_t now_ns) const;

    static constexpr int64_t kSnapNs = 5'000'000;   // порог жёсткого ре-лока: ±5 мс

private:
    void run();
    Callback cb_ = nullptr;
    void* user_ = nullptr;
    volatile bool stop_ = false;
    volatile bool paused_ = false;
    volatile bool running_ = false;
    struct Impl;
    Impl* impl_ = nullptr;
    friend struct Impl;
};
