#pragma once
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <stdint.h>

namespace capture {
// Ставит inline-хуки на libgui (BufferQueueProducer::queueBuffer / requestBuffer / attachBuffer).
// true — queueBuffer и requestBuffer захвачены (минимум для работы).
bool install();
// Задание конвейеру: буфер игры (с нашей ссылкой AHardwareBuffer_acquire), его acquire-fence
// (dup, -1 = готов), область crop и transform из QueueBufferInput.
struct Job {
    AHardwareBuffer* src = nullptr;
    int fence_fd = -1;
    int cl = 0, ct = 0, cr = 0, cb = 0;
    int bw = 0, bh = 0;
    int64_t ts_ns = 0;
    // Метка источника: timestamp из QueueBufferInput (+0). Для видео (MediaCodec → Surface) это
    // PTS кадра, для игр — systemTime на момент queueBuffer (CLOCK_MONOTONIC, как now_ns()).
    // 0 — нет (запасные пути захвата): конвейер использует ts_ns как до этого.
    int64_t pts_ns = 0;
    uint32_t transform = 0;
    int32_t dataspace = 0;          // android_dataspace из QueueBufferInput (0 = UNKNOWN)
    // Запасные пути (EGL/Vulkan) отдают кадры из своих колец AHB: когда конвейер закончил с заданием,
    // слот кольца надо освободить. release() вызывает done(done_ctx, done_tag) один раз.
    void (*done)(void* ctx, int tag) = nullptr;
    void* done_ctx = nullptr;
    int done_tag = 0;
    void release();
};
// Ждёт задание до timeout_ms. true — задание получено (владение переходит вызывающему).
bool pop(Job* out, int timeout_ms);
// Размер буфера выбранной игровой очереди (для подбора окна). false — ещё нет кадров.
bool electedSize(int* w, int* h);
// Окно выбранной очереди (точно то, в которое игра отдаёт кадры), с взятой ссылкой — вызывающий
// делает ANativeWindow_release. nullptr — хук Surface не стоит или кадров ещё не было.
ANativeWindow* electedWindow();
// true — хук android::Surface::*queueBuffer установлен (окно/буфер/fence приходят оттуда).
bool surfaceHooked();
// ---- запасные пути захвата (fallback_egl.cpp / fallback_vk.cpp) ---------------------------------
// true — BufferQueue-путь отдавал кадры в последние ~1.5 с: запасные пути в это время молчат.
bool bqActive();
// Были ли когда-либо выбраны кадры BufferQueue-путём, и время запуска модуля (для fallback_ms).
bool bqEverElected();
int64_t startNs();
// Запасной источник предлагает поверхность (ключ, размер, окно). true — этот ключ сейчас выбран
// игровым источником и кадры можно отдавать через extSubmit (BufferQueue молчит).
bool extOffer(const void* key, int w, int h, ANativeWindow* win);
// Отдать конвейеру задание запасного источника (владение переходит).
void extSubmit(Job& j);

// Прекращает захват (конвейер не поднялся): игра идёт как без модуля.
void disable();
// Статистика.
struct Stats { uint64_t queue_calls, requested, attached, queued, submitted, dropped_job, rejected_fmt, slot_miss, surf_calls; };
Stats stats();
}
