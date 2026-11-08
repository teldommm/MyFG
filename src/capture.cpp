// Захват кадров игры через libgui.
//
//  Почему BufferQueueProducer: при BLAST очередь буферов окна живёт в процессе приложения, а
//  Surface::queueBuffer идёт в BufferQueueProducer::queueBuffer напрямую (без Binder). Значит один
//  хук покрывает и GLES-, и Vulkan-игры, и Unity/свои движки — независимо от того, чем рисуют.
//
//  Хуки (только сбор данных, без тяжёлой работы на потоке игры):
//   Surface::hook_queueBuffer  : (window, buffer, fenceFd) — ровно то, что игра отдаёт системе.
//                                Кладём в thread-local: окно, буфер, dup(acquire-fence). Это даёт
//                                окно для ASurfaceControl без хуков EGL/Vulkan и буфер без таблицы слотов.
//   requestBuffer/attachBuffer : запасной путь — таблица (producer, slot) -> GraphicBuffer*
//   queueBuffer                : до оригинала снимаем crop/transform, берём буфер/fence из thread-local
//                                (или из таблицы / QueueBufferInput), после успешного оригинала
//                                отдаём задание воркеру.
//  Дальше задание (буфер с нашей ссылкой, dup fence, crop, transform) уходит в очередь, откуда его
//  забирает Vulkan-конвейер (pipeline.cpp). Тяжёлой работы на потоке игры нет.
#include "capture.h"
#include "common.h"
#include "config.h"
#include "util.h"
#include "hooks.h"

#include <android/hardware_buffer.h>
#include <chrono>
#include <android/sync.h>
#include <atomic>
#include <condition_variable>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <string.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr uint32_t kAnbMagic = 0x5f626672;   // ANDROID_NATIVE_BUFFER_MAGIC ('_bfr')
constexpr uint32_t kAnwMagic = 0x5f776e64;   // ANDROID_NATIVE_WINDOW_MAGIC ('_wnd')
constexpr int kElectFrames = 6;              // кандидат должен дать столько кадров за ~1 с (UI/сплеш редкие)
constexpr int kSlots = 64;                   // BufferQueueDefs::NUM_BUFFER_SLOTS

// ---- таблица producer -> слоты ----------------------------------------------------------
struct ProdEntry {
    void* self = nullptr;
    void* buf[kSlots] = {};
    int64_t last_ns = 0;
};
ProdEntry g_prod[8];
std::mutex g_pm;

// выбранная ("игровая") очередь
std::atomic<void*>   g_elected{nullptr};
std::atomic<int64_t> g_elected_last{0};
std::atomic<int>     g_elected_w{0}, g_elected_h{0};

ANativeWindow* g_elected_win = nullptr;     // с взятой ссылкой
std::mutex g_ewm;
std::atomic<ANativeWindow*> g_elected_win_raw{nullptr};
std::atomic<bool> g_surf_ok{false};
std::atomic<int64_t> g_bq_last{0};          // когда BufferQueue-путь последний раз отдал кадр
std::atomic<bool> g_bq_ever{false};
int64_t g_start_ns = now_ns();
char g_ext_token;                           // «ключ» выбранной очереди, если источник — запасной путь
// Размеры окон из BLASTBufferQueue::update: подсказка для выбора игровой очереди.
struct BlastHint { std::atomic<int> w{0}, h{0}; std::atomic<int64_t> ns{0}; };
BlastHint g_blast[4];
std::atomic<uint32_t> g_blast_i{0};
std::atomic<uint64_t> g_blast_calls{0};

// ---- классы источников -------------------------------------------------------------------
// Кадры в процессе приложения идут из разных очередей: игра/видео (то, что нужно) и окно самого приложения
// (HWUI, поток "RenderThread": тулбары, меню и прозрачная «дыра» под SurfaceView). Раньше побеждала очередь
// с большей площадью, и окно HWUI перебивало видео при каждой анимации интерфейса. Теперь у очереди есть
// приоритет: видео (кадры из MediaCodec, явный timestamp) > игра/удалённый рендер > окно приложения.
constexpr int kScoreUi = 1, kScoreGame = 2, kScoreVideo = 3;
std::atomic<int>  g_elected_score{0};
// Откуда приходят кадры выбранного источника: 0 — очередь BufferQueue в этом процессе (игра, окно известно),
// 1 — через Binder (видео, GPU-процесс браузера: окно только из BLAST), 2 — слой SurfaceControl (sc_capture.cpp).
constexpr int kModeBq = 0, kModeRemote = 1, kModeSc = 2;
std::atomic<int> g_elected_mode{kModeBq};
std::atomic<int64_t> g_remote_seen_ns{0};   // когда впервые увидели такой источник (для предупреждения в логе)
std::atomic<bool> g_remote_warned{false};

struct Cls { void* self = nullptr; int score = 0; bool ui = false; bool remote = false; const char* kind = ""; int64_t last = 0; };
// ui_source меняется на ходу, поэтому оценка окна приложения считается в момент использования
int score_of(const Cls& c) { return c.ui ? (g_cfg.ui_source ? kScoreUi : 0) : c.score; }
Cls g_cls[8];
std::mutex g_clsm;

std::atomic<int> g_fence_bad{0};
std::atomic<uint64_t> g_requested{0}, g_attached{0}, g_queue_calls{0}, g_surf_calls{0};
std::atomic<uint64_t> g_queued{0}, g_submitted{0}, g_dropped_job{0}, g_rejected_fmt{0}, g_miss{0}, g_rejected_prot{0};

int (*orig_queue)(void*, int, const void*, void*) = nullptr;
int (*orig_request)(void*, int, void*) = nullptr;
int (*orig_attach)(void*, int*, void*) = nullptr;
int (*orig_surf_queue)(void*, void*, int) = nullptr;

// Что игра отдаёт в Surface::queueBuffer на ЭТОМ потоке (producer->queueBuffer вызывается изнутри).
struct SurfTls { ANativeWindow* win; void* buf; int fence; };
// НЕ thread_local: модуль грузит Zygisk, а его загрузчик не обрабатывает R_AARCH64_TLSDESC, и первое же
// обращение к TLS прыгало на мусорный адрес (SIGSEGV в RenderThread). Используем pthread_key.
pthread_key_t g_tls_key;
bool g_tls_ok = false;
SurfTls* tls_get() { return g_tls_ok ? (SurfTls*)pthread_getspecific(g_tls_key) : nullptr; }

// ANativeWindowBuffer* и AHardwareBuffer* (= GraphicBuffer*) — РАЗНЫЕ указатели: на этом устройстве
// ANativeWindowBuffer лежит внутри GraphicBuffer со сдвигом +0x10 (видно по логу: requestBuffer отдаёт
// ...6a0, Surface::queueBuffer — ...6b0 для того же слота). Сдвиг подбираем при первом вызове сверкой
// width/height из ANativeWindowBuffer (+56/+60) с AHardwareBuffer_describe; результат кэшируем.
std::atomic<int> g_anb_delta{-1};
bool ahb_sane(AHardwareBuffer* a, AHardwareBuffer_Desc* d) {
    AHardwareBuffer_describe(a, d);
    return d->width >= 16 && d->width <= 16384 && d->height >= 16 && d->height <= 16384;
}
AHardwareBuffer* ahb_from_anb(void* anb) {
    int dl = g_anb_delta.load();
    if (dl >= 0) return (AHardwareBuffer*)((char*)anb - dl);
    int32_t wh[2];
    if (!safe_read((char*)anb + 56, wh, sizeof wh)) return nullptr;
    static const int kTry[] = {0x10, 0, 0x8, 0x18, 0x20, 0x28, 0x30, 0x40};
    for (int d : kTry) {
        AHardwareBuffer* c = (AHardwareBuffer*)((char*)anb - d);
        AHardwareBuffer_Desc desc;
        if (ahb_sane(c, &desc) && (int32_t)desc.width == wh[0] && (int32_t)desc.height == wh[1]) {
            g_anb_delta = d;
            LOGD("ANativeWindowBuffer -> AHardwareBuffer offset = -0x%x", d);
            return c;
        }
    }
    return nullptr;
}

// ---- окно удалённого источника (видео, GPU-процесс браузера) --------------------------------
// queueBuffer приходит на binder-потоке, Surface-хук там не срабатывает и окна нет. Но очередь SurfaceView —
// это BLASTBufferQueue приложения: он вызывает onFrameAvailable синхронно изнутри queueBuffer, на том же
// потоке. В этом хуке BBQ заведомо жив; BLASTBufferQueue::getSurface(true) отдаёт Surface со ссылкой на
// слой SurfaceView — ровно то, что нужно ASurfaceControl_createFromWindow (родитель = getSurfaceControlHandle()).
struct SpOut { void* p; SpOut() : p(nullptr) {} ~SpOut() {} };   // как android::sp<T>: нетривиальный dtor -> возврат через x8
typedef SpOut (*PFN_bbqGetSurface)(void* bbq, bool includeSurfaceControlHandle);
PFN_bbqGetSurface bbq_get_surface = nullptr;
void (*orig_bbq_frame)(void*, const void*) = nullptr;
std::atomic<bool> g_bbq_hooked{false}, g_bbq_tried{false};
std::atomic<int> g_anw_off{-1}, g_bbq_fail{0};
pthread_key_t g_bbq_key;
bool g_bbq_key_ok = false;
struct BbqCtx { void* self; };

struct WinEnt { void* self = nullptr; void* bbq = nullptr; ANativeWindow* win = nullptr; int64_t last = 0; };
WinEnt g_we[8];
std::mutex g_wem;

// Окно, найденное для producer'а, с взятой ссылкой (вызывающий делает ANativeWindow_release). nullptr — не найдено.
ANativeWindow* win_cached_acquire(void* self) {
    std::lock_guard<std::mutex> g(g_wem);
    for (auto& e : g_we)
        if (e.self == self && e.win) { e.last = now_ns(); ANativeWindow_acquire(e.win); return e.win; }
    return nullptr;
}

void resolve_bbq_window(void* self, void* bbq) {
    {
        std::lock_guard<std::mutex> g(g_wem);
        for (auto& e : g_we)
            if (e.self == self && e.bbq == bbq && e.win) { e.last = now_ns(); return; }     // уже известно
    }
    if (!bbq_get_surface || !bbq) return;
    if (g_bbq_fail.load() >= 3) return;               // формат ответа не распознан: не плодим объекты на каждый кадр
    SpOut r = bbq_get_surface(bbq, true);
    void* sp = r.p;                                   // +1 ссылка из sp<Surface> переходит нам (dtor пустой)
    if (!sp) return;
    // sp<Surface> указывает на Surface, а не на ANativeWindow: у Surface первым идёт RefBase (vptr + счётчики, 16 байт),
    // ANativeWindow лежит следом (+0x10) — тот же сдвиг, что у GraphicBuffer/ANativeWindowBuffer. Смещение подбираем
    // по магии окна и запоминаем; ANativeWindow_release по смещённому указателю снимает ссылку с Surface.
    int off = g_anw_off.load();
    if (off < 0) {
        static const int kTry[] = {0x10, 0, 0x8, 0x18, 0x20, 0x28};
        for (int d : kTry) {
            uint32_t m = 0;
            if (safe_read((char*)sp + d, &m, 4) && m == kAnwMagic) { off = d; g_anw_off = d; LOGD("Surface -> ANativeWindow offset = +0x%x", d); break; }
        }
    }
    uint32_t magic = 0;
    if (off < 0 || !safe_read((char*)sp + off, &magic, 4) || magic != kAnwMagic) {
        uint8_t dump[48] = {};
        safe_read(sp, dump, sizeof dump);
        char hx[48 * 3 + 1]; size_t o = 0;
        for (size_t i = 0; i < sizeof dump; i++) o += (size_t)snprintf(hx + o, sizeof hx - o, "%02x ", dump[i]);
        LOGW("BLASTBufferQueue::getSurface returned an object without a window magic (%p): %s", sp, hx);
        if (++g_bbq_fail >= 3) LOGW("remote sources stay unusable (BLAST window resolve disabled)");
        return;                                       // ссылку не трогаем: что это за объект — неизвестно
    }
    ANativeWindow* nw = (ANativeWindow*)((char*)sp + off);
    ANativeWindow* old = nullptr;
    {
        std::lock_guard<std::mutex> g(g_wem);
        WinEnt* e = nullptr;
        WinEnt* oldest = &g_we[0];
        for (auto& x : g_we) {
            if (x.self == self) { e = &x; break; }
            if (x.last < oldest->last) oldest = &x;
        }
        if (!e) e = oldest;
        old = e->win;                                 // пересоздан BBQ или вытеснена старая запись
        e->self = self; e->bbq = bbq; e->win = nw; e->last = now_ns();
    }
    if (old) ANativeWindow_release(old);
    LOGD("remote source %p: window %p from BLASTBufferQueue %p (%dx%d)", self, (void*)nw, bbq,
         ANativeWindow_getWidth(nw), ANativeWindow_getHeight(nw));
}

void hook_bbq_frame(void* bbq, const void* item) {
    if (g_bbq_key_ok) {
        BbqCtx* c = (BbqCtx*)pthread_getspecific(g_bbq_key);
        if (c && c->self) { void* s = c->self; c->self = nullptr; resolve_bbq_window(s, bbq); }   // один раз за queueBuffer
    }
    orig_bbq_frame(bbq, item);
}

// Хук ставится лениво — только когда замечен удалённый источник: играм (кадры идут через Surface-хук) он не нужен.
void bbq_install_async() {
    if (g_bbq_tried.exchange(true)) return;
    std::thread([] {
        size_t fs = 0, gs = 0;
        void* f = resolve_symbol_ex("libgui.so", "_ZN7android16BLASTBufferQueue16onFrameAvailableERKNS_10BufferItemE", false, &fs);
        if (!f) f = resolve_symbol_ex("libgui.so", "_ZN7android16BLASTBufferQueue16onFrameAvailable", true, &fs);
        void* g = resolve_symbol_ex("libgui.so", "_ZN7android16BLASTBufferQueue10getSurfaceEb", false, &gs);
        if (!g) g = resolve_symbol_ex("libgui.so", "_ZN7android16BLASTBufferQueue10getSurfaceEb", true, &gs);
        LOGD("libgui BLASTBufferQueue::onFrameAvailable=%p(%zu) getSurface=%p(%zu)", f, fs, g, gs);
        if (!f || !g) {
            LOGW("BLAST window hook unavailable (onFrameAvailable=%p getSurface=%p): video / remote sources will not be captured", f, g);
            return;
        }
        bbq_get_surface = (PFN_bbqGetSurface)g;
        if (inline_hook("BLASTBufferQueue::onFrameAvailable", f, fs, false, (void*)hook_bbq_frame, (void**)&orig_bbq_frame)) {
            g_bbq_hooked = true;
            LOGI("BLAST window hook installed (video / remote sources get their own layer)");
        } else {
            LOGW("BLAST window hook not installed: video / remote sources will not be captured");
        }
    }).detach();
}

// Класс очереди определяется один раз при первом кадре (кэш на 8 producer'ов; запись старше 5 с пересчитывается).
Cls class_of(void* self, bool via_surf, const uint8_t* in) {
    const int64_t now = now_ns();
    std::lock_guard<std::mutex> g(g_clsm);
    Cls* e = nullptr;
    Cls* oldest = &g_cls[0];
    for (auto& c : g_cls) {
        if (c.self == self && now - c.last < 5000000000LL) { e = &c; break; }
        if (c.last < oldest->last) oldest = &c;
    }
    if (e) { e->last = now; return *e; }
    Cls c;
    c.self = self; c.last = now;
    const bool remote = !via_surf && g_surf_ok.load();    // Surface-хук стоит, а кадр пришёл мимо него: Binder-поток
    c.remote = remote;
    if (remote) {
        int32_t is_auto = 1;
        memcpy(&is_auto, in + 8, sizeof is_auto);       // QueueBufferInput.isAutoTimestamp: у MediaCodec — 0 (явный PTS)
        if (is_auto == 0) { c.score = kScoreVideo; c.kind = "video"; }
        else              { c.score = kScoreGame;  c.kind = "remote"; }
    } else {
        char nm[20] = {};
        prctl(PR_GET_NAME, (unsigned long)nm, 0, 0, 0);
        if (strcmp(nm, "RenderThread") == 0) { c.ui = true; c.kind = "app window"; }
        else                                  { c.score = kScoreGame; c.kind = "game"; }
        if (!via_surf) c.kind = "game (no Surface hook)";
    }
    *oldest = c;
    LOGD("queue %p classified: %s (score %d)", self, c.kind, score_of(c));
    return c;
}

void record(void* self, int slot, void* buf) {
    if (slot < 0 || slot >= kSlots || !buf) return;
    std::lock_guard<std::mutex> g(g_pm);
    ProdEntry* e = nullptr;
    ProdEntry* oldest = &g_prod[0];
    for (auto& p : g_prod) {
        if (p.self == self) { e = &p; break; }
        if (p.last_ns < oldest->last_ns) oldest = &p;
    }
    if (!e) {
        e = oldest;
        *e = ProdEntry();
        e->self = self;
    }
    e->buf[slot] = buf;
    e->last_ns = now_ns();     // LRU: и для существующего producer'а (lookup не зовётся, когда кадры идут через Surface-хук)
}

void* lookup(void* self, int slot) {
    if (slot < 0 || slot >= kSlots) return nullptr;
    std::lock_guard<std::mutex> g(g_pm);
    for (auto& p : g_prod) {
        if (p.self == self) { p.last_ns = now_ns(); return p.buf[slot]; }
    }
    return nullptr;
}

bool size_matches(int w, int h, int tw, int th) {
    auto near = [](int a, int b) { return abs(a - b) <= (b >> 4) + 8; };
    return (near(w, tw) && near(h, th)) || (near(w, th) && near(h, tw));
}

bool blast_hint(int w, int h) {
    if (!g_cfg.elect_blast) return false;
    const int64_t now = now_ns();
    for (auto& b : g_blast) {
        int bw = b.w.load(), bh = b.h.load();
        if (bw > 0 && now - b.ns.load() < 30000000000LL && size_matches(w, h, bw, bh)) return true;
    }
    return false;
}

// Частота кадров по producer'ам (скользящее окно ~1 с).
struct Rate { void* self = nullptr; int64_t t0 = 0, last = 0; int n = 0; };
Rate g_rate[8];
std::mutex g_rmx;

int rate_touch(void* self, int64_t now) {
    std::lock_guard<std::mutex> g(g_rmx);
    Rate* r = nullptr;
    Rate* oldest = &g_rate[0];
    for (auto& x : g_rate) {
        if (x.self == self) { r = &x; break; }
        if (x.last < oldest->last) oldest = &x;
    }
    if (!r) { r = oldest; *r = Rate(); r->self = self; r->t0 = now; }
    if (now - r->t0 > 1000000000LL) { r->t0 = now; r->n = 0; }
    r->last = now;
    return ++r->n;
}

void set_elected_window(ANativeWindow* w) {
    if (!w || w == g_elected_win_raw.load()) return;
    ANativeWindow_acquire(w);
    ANativeWindow* old;
    {
        std::lock_guard<std::mutex> g(g_ewm);
        old = g_elected_win;
        g_elected_win = w;
        g_elected_win_raw.store(w);
    }
    if (old) ANativeWindow_release(old);
}

void clear_elected_window() {
    ANativeWindow* old;
    {
        std::lock_guard<std::mutex> g(g_ewm);
        old = g_elected_win;
        g_elected_win = nullptr;
        g_elected_win_raw.store(nullptr);
    }
    if (old) ANativeWindow_release(old);
}

// Решаем, наш ли это producer. Правила: явный target_w/h из конфига; иначе "побеждает больший".
// Кандидат обязан давать >= kElectFrames кадров в секунду: редкие кадры HWUI-окна активити
// и сплеша не должны перебивать игровой producer.
bool elect(void* self, int w, int h, ANativeWindow* win, int rate_n, int score, int mode, const char* kind) {
    int64_t now = now_ns();
    void* cur = g_elected.load();
    if (cur == self) {
        g_elected_last = now; g_elected_w = w; g_elected_h = h; g_elected_score = score;
        if (self != (void*)&g_ext_token) { g_bq_last = now; g_bq_ever = true; }
        if (win) set_elected_window(win);
        return true;
    }
    if (rate_n < kElectFrames) return false;
    const bool explicit_size = g_cfg.target_w > 0 && g_cfg.target_h > 0;
    if (score <= 0 && !explicit_size) return false;        // окно самого приложения (ui_source=0): не источник
    const int cur_score = g_elected_score.load();
    // Видео не отдаём окну приложения: пауза/перемотка дают тишину в очереди видео, но выбирать на её место
    // что-то другое можно лишь после заметной паузы.
    const int64_t stale_ns = (cur && cur_score >= kScoreVideo) ? 1500000000LL : 500000000LL;
    bool stale = !cur || now - g_elected_last.load() > stale_ns;
    const bool ext_cur = cur == (void*)&g_ext_token;       // сейчас кадры идут запасным путём — BufferQueue приоритетнее
    bool take = false;
    if (explicit_size) {
        take = size_matches(w, h, g_cfg.target_w, g_cfg.target_h);
    } else {
        int64_t ca = (int64_t)w * h, ea = (int64_t)g_elected_w.load() * g_elected_h.load();
        take = stale || ext_cur || score > cur_score ||
               (score == cur_score && (ca * 2 > ea * 3 ||                        // сильно больше текущего
                                       (blast_hint(w, h) && !blast_hint(g_elected_w.load(), g_elected_h.load()))));
    }
    if (!take) return false;
    g_elected = self; g_elected_last = now; g_elected_w = w; g_elected_h = h;
    g_elected_score = score; g_elected_mode = mode;
    if (self != (void*)&g_ext_token) { g_bq_last = now; g_bq_ever = true; }
    if (win) set_elected_window(win);
    else clear_elected_window();                           // окно прежнего источника к новому не относится
    LOGI("capture: %s %dx%d (%d frames/s) [%s]", self == (void*)&g_ext_token ? "fallback source elected" : "game queue elected", w, h, rate_n, kind);
    LOGD("elected %p window=%p mode=%d score=%d", self, (void*)win, mode, score);
    return true;
}

// ---- acquire fence игры -----------------------------------------------------------------
// QueueBufferInput: [0]timestamp [8]isAutoTimestamp [12]dataSpace [16]crop(4xint) [32]scalingMode
// [36]transform [40]stickyTransform [48]sp<Fence>. Fence = LightRefBase<Fence> { atomic mCount; unique_fd }.
// Раскладка проверяется на лету (mCount в разумных пределах + fd обязан быть sync_file);
// после 3 подряд неудач отключаем ожидание (кадр копируем без wait — как лучшее приближение).
int dup_acquire_fence(const uint8_t* input) {
    if (!g_cfg.acq_fence || g_fence_bad.load() >= 3) return -1;
    void* fp = nullptr;
    if (!safe_read(input + 48, &fp, sizeof fp) || !fp) return -1;
    struct { int32_t cnt, fd; } f;
    if (!safe_read(fp, &f, sizeof f) || f.cnt < 1 || f.cnt > 1024) { g_fence_bad++; return -1; }
    if (f.fd < 0) { g_fence_bad = 0; return -1; }            // NO_FENCE: буфер уже готов
    int d = fcntl(f.fd, F_DUPFD_CLOEXEC, 0);
    if (d < 0) { g_fence_bad++; return -1; }
    struct sync_file_info* info = sync_file_info(d);
    if (!info) { close(d); g_fence_bad++; return -1; }
    sync_file_info_free(info);
    g_fence_bad = 0;
    return d;
}

// ---- очередь заданий конвейеру ----------------------------------------------------------
constexpr int kQ = 2;
std::mutex g_qm;
std::condition_variable g_qcv;
capture::Job g_q[kQ];
int g_qn = 0;

void submit(capture::Job& j) {
    {
        std::lock_guard<std::mutex> g(g_qm);
        if (g_qn == kQ) {                    // конвейер не успевает: выбрасываем самое старое
            g_q[0].release();
            for (int i = 1; i < g_qn; i++) g_q[i - 1] = g_q[i];
            g_qn--;
            g_dropped_job++;
        }
        g_q[g_qn++] = j;
    }
    g_qcv.notify_one();
}

// Белого списка форматов нет: у вендорских гралоков бывают нестандартные коды (на этом устройстве
// буферы игры приходят с fmt=277), а решает всё равно Vulkan при импорте (importAhb). Отсекаем
// только заведомо не цветовые форматы.
bool format_ok(uint32_t f) {
    switch (f) {
        case 0x21:                                // BLOB
        case 0x23: case 0x36:                     // YCbCr 420 / P010
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:   // глубина/трафарет
            return false;
    }
    return true;
}

// ---- хуки -------------------------------------------------------------------------------
int hook_request(void* self, int slot, void* outSp) {
    int r = orig_request(self, slot, outSp);
    if (g_requested++ < 6) LOGD("requestBuffer self=%p slot=%d r=%d gb=%p", self, slot, r, outSp ? *(void**)outSp : nullptr);
    if (r == 0 && outSp) record(self, slot, *(void**)outSp);      // sp<GraphicBuffer> = один указатель
    return r;
}

int hook_attach(void* self, int* outSlot, void* spBuf) {
    int r = orig_attach(self, outSlot, spBuf);
    if (g_attached++ < 6) LOGD("attachBuffer self=%p r=%d", self, r);
    if (r == 0 && outSlot && spBuf) record(self, *outSlot, *(void**)spBuf);   // const sp<GraphicBuffer>&
    return r;
}

std::atomic<bool> g_off{false};

// Surface::hook_queueBuffer(ANativeWindow*, ANativeWindowBuffer*, int fenceFd): fenceFd отдаётся
// Surface'у (он его закроет), поэтому берём dup. Проверяем сигнатуру на первых вызовах.
std::atomic<int> g_surf_valid{0};
int hook_surf_queue(void* win, void* buf, int fenceFd) {
    g_surf_calls++;
    if (g_surf_valid.load(std::memory_order_relaxed) < 32) {
        uint32_t mw = 0, mb = 0;
        if (!(safe_read(win, &mw, 4) && mw == kAnwMagic && safe_read(buf, &mb, 4) && mb == kAnbMagic))
            return orig_surf_queue(win, buf, fenceFd);
        g_surf_valid++;
    }
    if (!g_tls_ok) return orig_surf_queue(win, buf, fenceFd);
    SurfTls cur;
    cur.win = (ANativeWindow*)win;
    cur.buf = buf;
    cur.fence = (g_cfg.acq_fence && fenceFd >= 0) ? fcntl(fenceFd, F_DUPFD_CLOEXEC, 0) : -1;
    hooks::noteWindow(cur.win);
    SurfTls* saved = (SurfTls*)pthread_getspecific(g_tls_key);
    pthread_setspecific(g_tls_key, &cur);
    int r = orig_surf_queue(win, buf, fenceFd);
    pthread_setspecific(g_tls_key, saved);
    if (cur.fence >= 0) close(cur.fence);              // конвейер не забрал
    return r;
}

// BLASTBufferQueue::update(const sp<SurfaceControl>&, uint32_t width, uint32_t height, int32_t format).
// Только наблюдаем: сигнатуру на этой версии Android мы не проверяли, поэтому пробрасываем все 8 регистровых
// аргументов, а width/height принимаем, лишь если они похожи на размер окна.
void* (*orig_blast_update)(void*, void*, void*, void*, void*, void*, void*, void*) = nullptr;
void* hook_blast_update(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    uint64_t n = g_blast_calls++;
    const uint32_t w = (uint32_t)(uintptr_t)a2, h = (uint32_t)(uintptr_t)a3;
    if (w >= 64 && w <= 16384 && h >= 64 && h <= 16384) {
        BlastHint& b = g_blast[g_blast_i++ & 3u];
        b.w = (int)w; b.h = (int)h; b.ns = now_ns();
        if (n < 6) LOGD("BLASTBufferQueue::update #%llu %ux%u", (unsigned long long)n, w, h);
    }
    return orig_blast_update(a0, a1, a2, a3, a4, a5, a6, a7);
}

int hook_queue(void* self, int slot, const void* input, void* output) {
    capture::Job job;
    bool want = false;
    SurfTls* ts = tls_get();
    const bool via_surf = ts && ts->buf != nullptr;
    {
        uint64_t n = g_queue_calls++;
        if (g_cfg.debug && (n < 8 || (n % 600) == 0)) {
            void* gbd = via_surf ? ts->buf : (input ? lookup(self, slot) : nullptr);
            AHardwareBuffer_Desc dd{};
            AHardwareBuffer* ab = !gbd ? nullptr : (via_surf ? ahb_from_anb(gbd) : (AHardwareBuffer*)gbd);
            if (ab) AHardwareBuffer_describe(ab, &dd);
            int32_t lc[4] = {-1, -1, -1, -1}, lds = -1;
            uint32_t ltr = 0xffffffffu;
            if (input) {   // input — ссылка на QueueBufferInput вызывающего: память валидна, читаем напрямую
                memcpy(lc, (const uint8_t*)input + 16, sizeof lc);
                memcpy(&lds, (const uint8_t*)input + 12, sizeof lds);
                memcpy(&ltr, (const uint8_t*)input + 36, sizeof ltr);
            }
            LOGD("queueBuffer #%llu self=%p slot=%d gb=%p%s %ux%u fmt=%u usage=0x%llx crop=[%d,%d,%d,%d] transform=%u dataspace=0x%x",
                 (unsigned long long)n, self, slot, gbd, via_surf ? "(surface)" : "(slot table)", dd.width, dd.height, dd.format,
                 (unsigned long long)dd.usage, lc[0], lc[1], lc[2], lc[3], ltr, (unsigned)lds);
        }
    }
    if (g_cfg.enable && !g_off.load(std::memory_order_relaxed) && input) {
        g_queued++;
        void* gb = via_surf ? ts->buf : lookup(self, slot);
        if (!gb) g_miss++;           // слот не в таблице: requestBuffer/attachBuffer не видели этот буфер
        AHardwareBuffer* ahb = !gb ? nullptr : (via_surf ? ahb_from_anb(gb) : (AHardwareBuffer*)gb);
        AHardwareBuffer_Desc d{};
        if (ahb && !ahb_sane(ahb, &d)) { ahb = nullptr; g_miss++; }
        if (ahb) {
            const uint8_t* in = (const uint8_t*)input;
            if (!format_ok(d.format)) {
                g_rejected_fmt++;
            } else if (d.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) {
                // Защищённый (DRM) буфер: небезопасный контекст его не прочитает (чёрный кадр или отказ импорта),
                // а непрозрачный оверлей закрыл бы им живое видео. Такие кадры не берём вовсе.
                g_rejected_prot++;
            } else {
                int32_t crop[4];
                int cl = 0, ct = 0, cr = (int)d.width, cb = (int)d.height;
                memcpy(crop, in + 16, sizeof crop);          // без safe_read: input — валидная ссылка, это 2 syscall'а на кадр
                if (crop[0] >= 0 && crop[1] >= 0 &&
                    crop[2] > crop[0] && crop[3] > crop[1] && crop[2] <= (int)d.width && crop[3] <= (int)d.height) {
                    cl = crop[0]; ct = crop[1]; cr = crop[2]; cb = crop[3];
                }
                const int64_t now = now_ns();
                const int rate_n = rate_touch(self, now);
                const Cls kc = class_of(self, via_surf, in);
                // Удалённый источник (Binder): окно — только из BLAST (resolve_bbq_window), своё чужое окно не подсовываем.
                ANativeWindow* rw = nullptr;
                bool can = true;
                if (kc.remote) {
                    if (g_cfg.hook_bbq) rw = win_cached_acquire(self);
                    can = rw != nullptr;
                    if (!can && !g_remote_warned.load() && !capture::scArmed()) {
                        const int64_t seen = g_remote_seen_ns.load();
                        if (!seen) g_remote_seen_ns = now;
                        else if (now - seen > 4000000000LL && !g_remote_warned.exchange(true))
                            LOGW("%s source %p has no window: %s", kc.kind, self,
                                 g_cfg.hook_bbq ? "BLASTBufferQueue hook gave none (not a SurfaceView queue?), source ignored"
                                                : "hook_bbq=0, source ignored");
                    }
                }
                const bool elected = can && elect(self, cr - cl, cb - ct, via_surf ? ts->win : rw, rate_n,
                                                  score_of(kc), kc.remote ? kModeRemote : kModeBq, kc.kind);
                if (rw) ANativeWindow_release(rw);
                if (elected) {
                    uint32_t tr = 0;
                    memcpy(&tr, in + 36, sizeof tr);
                    int32_t ds = 0;
                    memcpy(&ds, in + 12, sizeof ds);
                    // Значение берём только если оно похоже на android_dataspace (старшие биты 30/31 пусты);
                    // смещение +12 на устройстве не проверялось, поэтому мусор отсекаем.
                    job.dataspace = (ds >= 0 && ds < 0x40000000) ? ds : 0;
                    // QueueBufferInput[0] — timestamp: у видео это PTS кадра (медиа-часы), у игр —
                    // systemTime на момент queueBuffer (CLOCK_MONOTONIC, как now_ns()). Абсолютная база
                    // может быть любой (медиа-часы) — интервалы и привязку фазы делает конвейер
                    // (λ-фильтр в pipeline.cpp, сброс при скачке на 60 мс), поэтому здесь
                    // отсекаем только ноль/отрицательное. MediaCodec при releaseOutputBuffer(render)
                    // ставит mediaTimeUs*1000 — отсчёт от нуля, окно ±30 с от now его бы отсекло.
                    int64_t qbi_ts = 0;
                    memcpy(&qbi_ts, in, 8);
                    if (qbi_ts > 0)
                        job.pts_ns = qbi_ts;
                    if (via_surf) { job.fence_fd = ts->fence; ts->fence = -1; }   // fence из Surface (без разбора Fence)
                    else job.fence_fd = dup_acquire_fence(in);       // ДО оригинала: после него Fence может уйти
                    AHardwareBuffer_acquire(ahb);
                    job.src = ahb;
                    job.cl = cl; job.ct = ct; job.cr = cr; job.cb = cb;
                    job.bw = (int)d.width; job.bh = (int)d.height;
                    job.ts_ns = now; job.transform = tr;
                    want = true;
                }
            }
        }
    }
    // Удалённый источник (видео/GPU-процесс браузера): пока окна нет, просим BBQ-хук назвать себя изнутри queueBuffer.
    BbqCtx bctx{self};
    BbqCtx* bsaved = nullptr;
    bool barmed = false;
    if (input && !via_surf && g_cfg.hook_bbq && g_surf_ok.load() && g_bbq_key_ok) {
        if (g_bbq_hooked.load()) { bsaved = (BbqCtx*)pthread_getspecific(g_bbq_key); pthread_setspecific(g_bbq_key, &bctx); barmed = true; }
        else bbq_install_async();
    }
    int r = orig_queue(self, slot, input, output);   // игра не должна заметить вмешательства
    if (barmed) pthread_setspecific(g_bbq_key, bsaved);
    if (want) {
        if (r == 0) { g_submitted++; submit(job); }
        else job.release();
    }
    return r;
}

}  // namespace

namespace capture {

// Каждый хук отслеживается отдельно: install() вызывается повторно, пока всё не встало, и второй
// DobbyHook на тот же адрес вернул бы ошибку "already hooked".
bool install() {
    static bool r_ok = false, a_ok = false, q_ok = false, s_tried = false, logged = false, blast_tried = false;
    if (r_ok && q_ok) return true;
    if (!g_tls_ok) g_tls_ok = pthread_key_create(&g_tls_key, nullptr) == 0;
    if (!g_bbq_key_ok) g_bbq_key_ok = pthread_key_create(&g_bbq_key, nullptr) == 0;
    size_t qs = 0, rs = 0, as = 0;
    void* q = resolve_symbol_ex("libgui.so", "_ZN7android19BufferQueueProducer11queueBufferEi", true, &qs);
    void* r = resolve_symbol_ex("libgui.so", "_ZN7android19BufferQueueProducer13requestBufferEi", true, &rs);
    void* a = resolve_symbol_ex("libgui.so", "_ZN7android19BufferQueueProducer12attachBufferE", true, &as);
    if (!logged) {
        LOGD("libgui symbols: queue=%p(%zu) request=%p(%zu) attach=%p(%zu)", q, qs, r, rs, a, as);
        logged = true;
    }
    if (!q || !r) return false;

    if (!r_ok) r_ok = inline_hook("BufferQueueProducer::requestBuffer", r, rs, false, (void*)hook_request, (void**)&orig_request);
    if (!r_ok) return false;
    if (a && !a_ok) a_ok = inline_hook("BufferQueueProducer::attachBuffer", a, as, false, (void*)hook_attach, (void**)&orig_attach);

    if (g_cfg.hook_surface && !s_tried) {
        s_tried = true;
        size_t ss = 0;
        // статический вход, через который идут ANativeWindow::queueBuffer (EGL, Vulkan, NDK)
        void* sf = resolve_symbol_ex("libgui.so", "_ZN7android7Surface16hook_queueBufferEP13ANativeWindowP19ANativeWindowBufferi", false, &ss);
        const char* nm = "Surface::hook_queueBuffer";
        if (!sf) {      // запасной вариант: метод (ABI тот же: this, buffer, fenceFd)
            sf = resolve_symbol_ex("libgui.so", "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi", false, &ss);
            nm = "Surface::queueBuffer";
        }
        LOGD("libgui Surface symbol: %s=%p(%zu)", nm, sf, ss);
        if (sf && inline_hook(nm, sf, ss, false, (void*)hook_surf_queue, (void**)&orig_surf_queue)) g_surf_ok = true;
        else LOGW("Surface hook not installed: window will come from EGL/ANativeWindow hooks");
    }
    if (!q_ok) q_ok = inline_hook("BufferQueueProducer::queueBuffer", q, qs, false, (void*)hook_queue, (void**)&orig_queue);
    if (q_ok && g_cfg.elect_blast && !blast_tried) {
        blast_tried = true;
        size_t bs = 0;
        void* bu = resolve_symbol_ex("libgui.so", "_ZN7android16BLASTBufferQueue6updateE", true, &bs);
        LOGD("libgui BLASTBufferQueue::update=%p(%zu)", bu, bs);
        if (bu && !inline_hook("BLASTBufferQueue::update", bu, bs, false, (void*)hook_blast_update, (void**)&orig_blast_update))
            LOGW("BLAST update hook not installed (election falls back to rate+size)");
    }
    if (q_ok) LOGI("capture hooks installed: BufferQueue%s%s", a_ok ? "+attach" : "", g_surf_ok.load() ? "+Surface" : "");
    return r_ok && q_ok;
}

bool surfaceHooked() { return g_surf_ok.load(); }
bool electedRemote() { return g_elected_mode.load() == kModeRemote; }
bool electedSc() { return g_elected_mode.load() == kModeSc; }

ANativeWindow* electedWindow() {
    std::lock_guard<std::mutex> g(g_ewm);
    if (g_elected_win) ANativeWindow_acquire(g_elected_win);
    return g_elected_win;
}

void Job::release() {
    if (fence_fd >= 0) close(fence_fd);
    if (src) AHardwareBuffer_release(src);
    fence_fd = -1; src = nullptr;
    if (done) { auto d = done; done = nullptr; d(done_ctx, done_tag); }
}

void disable() { g_off.store(true); }

bool pop(Job* out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_qm);
    if (!g_qcv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return g_qn > 0; })) return false;
    *out = g_q[0];
    for (int i = 1; i < g_qn; i++) g_q[i - 1] = g_q[i];
    g_qn--;
    return true;
}

bool electedSize(int* w, int* h) {
    if (!g_elected.load()) return false;
    *w = g_elected_w.load(); *h = g_elected_h.load();
    return *w > 0 && *h > 0;
}

Stats stats() {
    return {g_queue_calls.load(), g_requested.load(), g_attached.load(), g_queued.load(), g_submitted.load(), g_dropped_job.load(), g_rejected_fmt.load(), g_miss.load(), g_surf_calls.load(), g_rejected_prot.load(), scBuffers()};
}

bool bqActive() { return now_ns() - g_bq_last.load() < 1500000000LL; }
bool bqEverElected() { return g_bq_ever.load(); }
int64_t startNs() { return g_start_ns; }

bool extOffer(const void* key, int w, int h, ANativeWindow* win) {
    if (g_off.load() || !g_cfg.enable) return false;
    if (bqActive()) return false;                                  // BufferQueue работает — запасной путь не нужен
    (void)key;
    const int64_t now = now_ns();
    const int rate_n = rate_touch(const_cast<void*>(key), now);
    if (!elect((void*)&g_ext_token, w, h, win, rate_n, kScoreGame, kModeBq, "fallback")) return false;
    return true;
}

void extSubmit(Job& j) {
    g_queued++; g_submitted++;
    submit(j);
}

}  // namespace capture

// ---- для sc_capture.cpp ------------------------------------------------------------------
namespace capture {
namespace detail {
int rateTouch(const void* key) { return rate_touch(const_cast<void*>(key), now_ns()); }
bool formatOk(uint32_t f) { return format_ok(f); }
void countRejectedFmt() { g_rejected_fmt++; }
void countRejectedProt() { g_rejected_prot++; }
void countQueued() { g_queued++; }
// Слой SurfaceControl предлагает себя в источники. true — выбран именно он.
bool scElect(const void* key, int w, int h, int rate_n) {
    if (g_off.load() || !g_cfg.enable) return false;
    return elect(const_cast<void*>(key), w, h, nullptr, rate_n, kScoreGame, kModeSc, "surface-control layer");
}
void scSubmit(Job& j) { g_submitted++; submit(j); }
const void* electedKey() { return g_elected.load(); }
// Слой освобождён приложением: если он был источником, выбор сбрасывается (следующий кандидат берёт его без ожидания).
bool dropElected(const void* key) {
    void* exp = const_cast<void*>(key);
    if (!g_elected.compare_exchange_strong(exp, nullptr)) return false;
    g_elected_last = 0;
    g_elected_mode = kModeBq;
    return true;
}
}  // namespace detail
}  // namespace capture
