#pragma once
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <stdint.h>

struct ASurfaceControl;

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
// true — выбранный источник приходит через Binder (видео, GPU-процесс браузера): его окно известно только из BLAST,
// эвристика «последнее окно приложения» к нему неприменима (иначе оверлей лёг бы поверх всего UI).
bool electedRemote();
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

// ---- слои SurfaceControl (sc_capture.cpp) -------------------------------------------------------------
// Так показывает страницу Chrome: GPU-процесс (`пакет:privileged_process*`) собирает кадр и отдаёт его слою
// через ASurfaceTransaction_setBuffer, мимо BufferQueue процесса браузера. Модуль ловит эти вызовы, берёт буфер
// слоя-источника и ставит свой слой-оверлей СОСЕДОМ источника (тот же родитель, z сразу над источником,
// геометрия/прозрачность/видимость как у источника).
struct ScLive {
    bool visible = true;          // источник показан (последний вызов приложения не HIDE)
    int32_t z = 0;                // z оверлея: строго над источником и строго под следующим слоем приложения
    bool geom = false;            // приложение задавало геометрию
    int dl = 0, dt = 0, dr = 0, db = 0;   // dst источника (в системе координат родителя)
    int32_t transform = 0;        // transform источника (ANATIVEWINDOW_TRANSFORM_*)
    float alpha = 1.0f;
    int transparency = -1;        // ASURFACE_TRANSACTION_TRANSPARENCY_*; -1 — приложение не задавало
};
// Ставит хуки libandroid (ASurfaceControl_* / ASurfaceTransaction_*). true — повторять не нужно (стоят, не нужны
// в этом процессе или окончательно отказаны); false — повторить позже (libandroid ещё не загружена).
bool installSc();
// Хуки стоят и приложение ведёт себя так, что захват слоёв корректен (см. sc_capture.cpp). Без этого слои не берём.
bool scArmed();
// Выбранный источник — слой SurfaceControl (его окно/родитель знает sc_capture, а не BufferQueue-хуки).
bool electedSc();
// Текущее состояние выбранного слоя. false — выбранного слоя нет (освобождён, источник другой).
bool scLive(ScLive* out);
// Создаёт слой-оверлей рядом с источником (ASurfaceControl_create у родителя источника либо createFromWindow).
// *parentKey — идентификатор родителя: сменился -> оверлей надо создать заново; *rootWindow — окно корня дерева слоёв
// (со ссылкой, для HUD и «якоря»), может быть nullptr. nullptr — родитель недоступен.
ASurfaceControl* scCreateOverlay(const char* name, uint64_t* parentKey, ANativeWindow** rootWindow);
// Родитель выбранного слоя сейчас (для сравнения с parentKey); 0 — неизвестен.
uint64_t scParentKey();
// true один раз после события, которое делает показанный оверлей неактуальным (источник скрыт, его геометрия
// изменилась, слой освобождён): оверлей надо спрятать сразу, не дожидаясь hide_ms.
bool scTakeInvalidated();

// Прекращает захват (конвейер не поднялся): игра идёт как без модуля.
void disable();
// Статистика.
struct Stats { uint64_t queue_calls, requested, attached, queued, submitted, dropped_job, rejected_fmt, slot_miss, surf_calls, rejected_prot, sc_buffers; };
Stats stats();
uint64_t scBuffers();      // сколько раз приложение отдавало буфер слою SurfaceControl (sc_capture.cpp)
}
