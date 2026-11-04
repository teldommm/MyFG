// Vulkan-конвейер: кадр игры -> копия в кольцо -> WFG -> очередь вывода -> ASurfaceControl.
//
//  Потоки:
//   * pipeline (run()) — единственный, кто трогает Vulkan: на каждое задание из capture пишет командный
//     буфер (blit игрового буфера в кольцо, vkr_dis_process, N × vkr_dis_generate_into), делает submit
//     и экспортирует его завершение как sync_fd. Благодаря одной очереди и одному потоку между
//     копированием и WFG не нужны семафоры. С включённым qcom_me буферов и submit'ов два:
//     blit уходит на GPU до блокирующего ME (glFinish+readback) — поле считается, пока GPU копирует
//     кадр; зависимость закрывает барьер в начале второго буфера (submit'ы одной очереди упорядочены).
//   * vsync (Vsync) — на каждый vsync берёт готовый OutItem, выбирает frame-timeline и делает
//     ASurfaceTransaction_apply. Vulkan там не используется.
//   * callbacks SurfaceFlinger (onComplete) — замер лэтча и контур пейсинга:
//     см. paceOnComplete.
//
//  Вывод по кадру игры N (интервал до предыдущего dt, planned = vkr_dis_plan):
//     generated[g]  g = 0..planned-1  -> target = T + g * dt/(planned+1)
//     real N                          -> target = T + planned * dt/(planned+1)
//  То есть тот же порядок, что в рендерере WFG: сначала сгенерированные, потом реальный кадр,
//  с задержкой в один кадр игры.
#include "pipeline.h"
#include "capture.h"
#include "fallback.h"
#include "common.h"
#include "config.h"
#include "hooks.h"
#include "hud.h"
#include "vk_ctx.h"
#include "hdr.h"
#include "qcom_me.h"
#include "display.h"
#include "vsync.h"
#include "vkr_dis.h"

#include <android/native_window.h>
#include <android/surface_control.h>
#include <algorithm>
#include <climits>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <atomic>
#include <dlfcn.h>
#include <math.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

// ---- троттлинг по температуре батареи ----------------------------------------------------------
// /sys/class/power_supply/battery/temp: «442\n» = 44.2°C. Отсутствие файла — обычное дело
// на части ядер: тогда троттлинг тихо неактивен (одна строка в лог).
static int batteryTenthsC() {
    const int fd = open("/sys/class/power_supply/battery/temp", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return INT_MIN;
    char b[16] = {};
    const ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return INT_MIN;
    return atoi(b);          // число в 0.1°C; мусор в файле даст 0 — не похоже ни на один порог
}
constexpr int kFrames = 3;          // командные буферы в полёте
constexpr int kReal = 6;            // кольцо реальных кадров
constexpr int kOutMax = 24;         // потолок кольца сгенерированных
constexpr int kHdrMax = 26;        // кольцо кадров после HDR-прохода
constexpr int kPlain = 3;          // промежуточные storage-образы HDR (запасной путь без STORAGE у AHB)
constexpr int kDsBt2020Pq = 0x09c60000;   // STANDARD_BT2020 | TRANSFER_ST2084 | RANGE_FULL
constexpr int kQ = 12;              // очередь вывода
constexpr uint64_t kPending = ~0ULL;
constexpr uint64_t kShown = ~0ULL - 1;     // на экране, ждём onComplete следующей транзакции
constexpr int64_t kShownTimeoutNs = 250000000LL;

typedef void (*PFN_setFrameTimeline)(ASurfaceTransaction*, int64_t);
typedef void (*PFN_setFrameRate)(ASurfaceTransaction*, ASurfaceControl*, float, int8_t, int8_t);
typedef int (*PFN_getId)(const AHardwareBuffer*, uint64_t*);
typedef void (*PFN_winRate)(ANativeWindow*, float, int8_t, int8_t);   // ANativeWindow_setFrameRate[WithChangeStrategy]

struct Slot {
    AhbImage im;
    // 0 — свободен; kPending — в работе/в очереди вывода; иначе — занят до этого output-номера
    std::atomic<uint64_t> hold{0};
    // onComplete: release-fence предыдущего показа (пока не сработал — слот занят) и момент показа (для страховки)
    std::atomic<int> rel_fd{-1};
    std::atomic<int64_t> shown_ns{0};
};

struct OutItem {
    AHardwareBuffer* ahb = nullptr;   // с нашей ссылкой
    int fence_fd = -1;                // уйдёт в setBuffer
    int64_t target_ns = 0;
    Slot* slot = nullptr;
    uint32_t transform = 0;
    int32_t dataspace = 0;
    bool real = false;                // реальный кадр игры (его при догоне очереди не выбрасываем)
    bool hdr = false;                 // кадр прошёл SDR->HDR проход: слою ставится dataspace BT2020_PQ
    int w = 0, h = 0;
    uint64_t rgen = 0;                // поколение кольца, которому принадлежит slot (см. apply)
    void drop() {                     // так и не показан
        if (fence_fd >= 0) close(fence_fd);
        if (ahb) AHardwareBuffer_release(ahb);
        if (slot) slot->hold.store(0);
        fence_fd = -1; ahb = nullptr; slot = nullptr;
    }
};

struct FrameRes {
    VkCommandBuffer cb = VK_NULL_HANDLE;    // буфер A: blit игрового кадра (submit до ME)
    VkCommandBuffer cb2 = VK_NULL_HANDLE;   // буфер B: WFG + HDR (второй submit, только при qcom_me)
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore wait_sem = VK_NULL_HANDLE;   // сюда импортируется acquire-fence игры
    VkSemaphore sig_sem = VK_NULL_HANDLE;    // экспортируется в sync_fd
    bool submitted = false;
    VkQueryPool qp = VK_NULL_HANDLE;         // три timestamp: начало / конец генерации / конец HDR-проходов
    bool timed = false;
    uint32_t planned = 0;                    // сколько кадров генерировал этот submit
    std::vector<Slot*> fence_rel;            // слоты gen[] HDR-варианта: нужны только до конца submit, отпускаем по fence
    uint64_t fence_rel_gen = 0;              // поколение кольца (см. ringGen_): после пересоздания указатели недействительны
    std::vector<AhbImage> garbage;           // некэшируемые импорты: освобождаем, когда кадр отработал
};

class Pipeline {
public:
    void start() { std::thread([this] { run(); }).detach(); }

private:
    // =============================================================== vsync-сторона
    static void onVsyncStatic(const VsyncInfo& vi, void* u) { ((Pipeline*)u)->onVsync(vi); }

    void onVsync(const VsyncInfo& vi) {
        period_ns_.store(vi.period_ns);
        OutItem it;
        int64_t tl_id = 0;
        int64_t tl_desired = 0;          // ожидание выбранного таймлайна (для контура пейсинга)
        bool have_tl = false;
        bool empty = false;
        {
            std::lock_guard<std::mutex> g(oq_m_);
            // Догон. За vsync уходит один кадр, а WFG допускает выход до ~110% частоты экрана, поэтому
            // при источнике чуть выше кратного очередь росла бы до kQ (десятки мс задержки), а потом
            // терялся самый старый кадр, в том числе реальный. Сгенерированный кадр, опоздавший больше
            // чем на 1.5 периода, показывать уже поздно: выбрасываем его, реальные кадры не трогаем.
            {
                // now_ns() берём на каждой итерации: за время выбрасывания нескольких
                // устаревших кадров время уходит, и «сейчас» не должно застревать на входе в цикл
                while (on_ > 1 && !oq_[0].real && now_ns() - oq_[0].target_ns > vi.period_ns * 3 / 2) {
                    oq_[0].drop();
                    for (int i = 1; i < on_; i++) oq_[i - 1] = oq_[i];
                    on_--;
                    dropped_late_++;
                }
            }
            if (on_ == 0) empty = true;
        }
        if (empty) { idle(); return; }
        {
            std::lock_guard<std::mutex> g(oq_m_);
            if (on_ == 0) return;
            const OutItem& head = oq_[0];
            if (vi.has_timeline && vi.n > 0) {
                int pick = -1;
                int64_t now = now_ns();
                // Базовый запас 0.5 мс + guard пейсинга. Guard поднимается, когда
                // показ стабильно опаздывает (paceOnComplete): таймлайн с present раньше цели
                // становится допустим — SurfaceFlinger лэтчит нас раньше, опоздание съедается.
                const int64_t slack = 500000 + (g_cfg.pacing ? pace_guard_ns_.load(std::memory_order_relaxed) : 0);
                for (int i = 0; i < vi.n; i++) {
                    if (vi.present[i] + slack >= head.target_ns && vi.deadline[i] > now + 300000) { pick = i; break; }
                }
                if (pick < 0) {
                    if (head.target_ns > vi.present[vi.n - 1]) return;   // цель далеко: ждём следующих vsync
                    pick = 0;                                            // цель в прошлом: ближайший
                }
                tl_id = vi.id[pick]; tl_desired = vi.present[pick]; have_tl = true;
            } else if (head.target_ns > vi.frame_ns + vi.period_ns * 3 / 2) {
                return;
            }
            it = oq_[0];
            for (int i = 1; i < on_; i++) oq_[i - 1] = oq_[i];
            on_--;
        }
        apply(it, have_tl ? &tl_id : nullptr, have_tl ? tl_desired : 0);
    }

    // Контекст onComplete: transaction N сообщает, что буфер от transaction N-1 свободен.
    // gen — поколение слоя, rgen — поколение кольца: между apply и колбэком могло произойти
    // пересоздание (смена слоя/разрешения), и слот prev уже принадлежит новому поколению —
    // тогда его не трогаем вовсе. desired_ns — ожидание показа выбранного таймлайна (0 = без
    // таймлайнов): уходит в контур пейсинга (paceOnComplete) вместе с фактическим лэтчем.
    struct CbCtx { Pipeline* p; Slot* prev; uint64_t gen; uint64_t rgen; ASurfaceControl* sc; int64_t desired; };
    static void onCompleteStatic(void* u, ASurfaceTransactionStats* st) {
        CbCtx* c = (CbCtx*)u;
        if (c->prev) {
            // Фенс не запрашиваем, пока не убедились в актуальности слота: полученный fd
            // пришлось бы закрывать самому, а записать его уже некуда.
            if (c->p->sc_gen_.load() == c->gen && c->p->ringGen_.load() == c->rgen) {
                int fd = ASurfaceTransactionStats_getPreviousReleaseFenceFd(st, c->sc);
                c->prev->rel_fd.store(fd);
                uint64_t exp = kShown;
                if (!c->prev->hold.compare_exchange_strong(exp, 0)) {           // уже забран по таймауту
                    int f2 = c->prev->rel_fd.exchange(-1);
                    if (f2 >= 0) close(f2);
                }
            }
        }
        if (c->desired > 0 && c->p->sc_gen_.load() == c->gen)
            c->p->paceOnComplete(st, c->sc, c->desired);
        delete c;
    }

    // ---- пейсинг с обратной связью -------------------------------------------------------------
    // Измерение (поток callbacks SF): latch = ASurfaceTransactionStats_getLatchTime (API 29,
    // как и getPreviousReleaseFenceFd) -> present = следующий vsync после лэтча по нашей фазовой
    // сетке (Vsync::nextVsyncAfter) -> err = present - desired (desired кладётся в CbCtx при
    // apply: ожидание именно выбранного таймлайна, а не target кадра — иначе в err попал бы
    // намеренный фазовый сдвиг). Present-fence не читаем: он для владения, а не для err.
    //
    // Два контура:
    //  1) медленный bias (α=0.02) забирает постоянную задержку композита (лэтч в пределах кадра,
    //     буферная логика SF) — реакция контура остаётся только на переменную часть;
    //  2) ema(err/period) (α=0.1): стабильно >= +0.6 слота -> guard +250 мкс (до 2.5 мс) — выбор
    //     таймлайна разрешает более ранний present (slack в onVsync); запас (< 0.2 слота, 8
    //     замеров подряд, кулдаун 1 с) -> guard тихо снимается теми же ступенями. Выбросы
    //     (|err| > 3 периодов: слитые транзакции, ре-лок сетки) в контур не пускаются.
    // Работает только при use_oncomplete и таймлайнах (API 33+); без них контур спит, не
    // имитируясь через target кадра. Всё живёт на callback-потоке, наружу — только атомик guard.
    void paceOnComplete(ASurfaceTransactionStats* st, ASurfaceControl* sc, int64_t desired) {
        (void)sc;                                            // latch — свойство транзакции, не слоя
        if (!g_cfg.pacing) return;
        const int64_t latch = ASurfaceTransactionStats_getLatchTime(st);
        if (latch <= 0) return;                          // транзакция не лэтчнулась (слита/пропущена)
        const int64_t present = vsync_.nextVsyncAfter(latch);
        const int64_t period = period_ns_.load();
        if (period < 1000000) return;
        const int64_t errRaw = present - desired;
        if (llabs(errRaw) > period * 3) return;          // выброс: не портим EMA

        pace_bias_ += (double)(errRaw - (int64_t)pace_bias_) * 0.02;
        const double slots = (double)(errRaw - (int64_t)pace_bias_) / (double)period;
        pace_ema_ += (slots - pace_ema_) * 0.1;
        pace_n_++;
        const int64_t now = now_ns();
        if (pace_ema_ >= 0.6) {
            pace_calm_ = 0;
            if (pace_guard_ns_.load(std::memory_order_relaxed) < kPaceGuardMaxNs &&
                now - pace_change_ns_ > 200000000LL) {
                const int64_t g = std::min(kPaceGuardMaxNs, pace_guard_ns_.load(std::memory_order_relaxed) + kPaceGuardStepNs);
                pace_guard_ns_.store(g, std::memory_order_relaxed);
                pace_change_ns_ = now;
                LOGI("pacing: present late (ema %+.2f slots, bias %+.2f ms) -> earlier timeline allowed, guard %+.2f ms",
                     pace_ema_, pace_bias_ / 1.0e6, g / 1.0e6);
            }
        } else if (pace_ema_ < 0.2) {                    // запас: тихо возвращаем надбавку
            if (++pace_calm_ >= 8 && pace_guard_ns_.load(std::memory_order_relaxed) > 0 &&
                now - pace_change_ns_ > 1000000000LL) {
                const int64_t g = std::max((int64_t)0, pace_guard_ns_.load(std::memory_order_relaxed) - kPaceGuardStepNs);
                pace_guard_ns_.store(g, std::memory_order_relaxed);
                pace_change_ns_ = now; pace_calm_ = 0;
                LOGI("pacing: slack back to %+.2f ms (ema %+.2f slots)", g / 1.0e6, pace_ema_);
            }
        } else {
            pace_calm_ = 0;
        }
        if (g_cfg.debug && now - pace_last_log_ns_ > 5000000000LL) {
            pace_last_log_ns_ = now;
            LOGD("pacing: err %+.2f slots (raw %+.2f ms, bias %+.2f ms), guard %+.2f ms, n=%llu",
                 pace_ema_, errRaw / 1.0e6, pace_bias_ / 1.0e6,
                 pace_guard_ns_.load(std::memory_order_relaxed) / 1.0e6, (unsigned long long)pace_n_);
        }
    }

    void apply(OutItem& it, const int64_t* vsync_id, int64_t desired_ns = 0) {
        // Модуль выключили на ходу, а этот кадр уже забран из очереди заблаговременно:
        // не показываем его поверх спрятанного оверлея (vsync-поток встаёт на паузу и
        // никто не спрятал бы его повторно).
        if (!g_cfg.enable) { it.drop(); return; }
        // Кадр из прежнего кольца (кольцо пересоздано, пока он ждал своего vsync): показывать
        // нельзя, а слот уже принадлежит новому поколению — отпускаем буфер, не касаясь слота
        // (drop() здесь нельзя: он обнулил бы hold чужого слота).
        if (it.slot && it.rgen != ringGen_.load()) {
            if (it.fence_fd >= 0) close(it.fence_fd);
            if (it.ahb) AHardwareBuffer_release(it.ahb);
            it.fence_fd = -1; it.ahb = nullptr; it.slot = nullptr;
            return;
        }
        std::lock_guard<std::mutex> g(sc_m_);
        if (!sc_) { it.drop(); return; }
        ASurfaceTransaction* t = ASurfaceTransaction_create();
        ASurfaceTransaction_setBuffer(t, sc_, it.ahb, it.fence_fd);      // fd забирает транзакция
        it.fence_fd = -1;
        ASurfaceTransaction_setVisibility(t, sc_, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
        visible_.store(true);
        last_apply_ns_.store(now_ns());
        {
            // HDR-кадр всегда идёт с BT2020_PQ (независимо от ключа dataspace); обычный — dataspace игры или 0
            const int ds = it.hdr ? kDsBt2020Pq : (g_cfg.dataspace ? it.dataspace : 0);
            if (ds != applied_ds_) {
                ASurfaceTransaction_setBufferDataSpace(t, sc_, (ADataSpace)ds);
                LOGD("overlay dataspace 0x%x -> 0x%x", (unsigned)(applied_ds_ < 0 ? 0 : applied_ds_), (unsigned)ds);
                applied_ds_ = ds;
            }
        }
        ASurfaceTransaction_setZOrder(t, sc_, g_cfg.zorder);
        ASurfaceTransaction_setBufferTransparency(t, sc_, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
        // transform слоя: из QueueBufferInput или принудительный (layer_tf 0..7)
        const uint32_t tf = g_cfg.layer_tf >= 0 ? (uint32_t)g_cfg.layer_tf : it.transform;
        // Размер окна спрашиваем у окна ЗДЕСЬ (под sc_m_): кэш win_w_/win_h_ обновляется
        // только разметкой раз в секунду (housekeeping), и после поворота/ресайза оверлей
        // до секунды растягивался бы по старой геометрии.
        int ww = 0, wh = 0;
        if (win_) { ww = ANativeWindow_getWidth(win_); wh = ANativeWindow_getHeight(win_); }
        if (ww <= 0 || wh <= 0) { ww = it.w; wh = it.h; if (tf & 4) { int x = ww; ww = wh; wh = x; } }
        ARect src = {0, 0, it.w, it.h};
        ARect dst = {0, 0, ww, wh};
        ASurfaceTransaction_setGeometry(t, sc_, src, dst, (int32_t)tf);
        if (vsync_id && setFrameTimeline_) setFrameTimeline_(t, *vsync_id);
        // Частота слоя (голос SurfaceFlinger): display_fps > auto_rate > panel_hz. Повторяем раз в 0.5 с,
        // чтобы голос не терялся, и при сдвиге >= 2 fps.
        if (setFrameRate_) {
            float want = g_cfg.display_fps > 0 ? (float)g_cfg.display_fps
                            : (g_cfg.auto_rate && want_rate_.load() > 0.0f ? want_rate_.load()
                               : (g_cfg.panel_hz > 0 ? (float)g_cfg.panel_hz : 0.0f));
            // Троттлинг батареи: голос не выше деградированного темпа — иначе панель остаётся на
            // 120 Гц с полупустыми кадрами, и экономии нет. Перекрывает даже явный display_fps:
            // защита от перегрева важнее ручной подсказки.
            if (therm_on_.load() && want > (float)g_cfg.thermal_hz) want = (float)g_cfg.thermal_hz;
            const int64_t nowr = now_ns();
            const bool changed = want > 0.0f && fabsf(want - applied_rate_) >= 2.0f;
            if (want > 0.0f && (changed || nowr - rate_ns_ > 500000000LL)) {
                setFrameRate_(t, sc_, want, 0, 1);
                if (changed) LOGD("overlay frame rate hint %.0f -> %.0f", (double)applied_rate_, (double)want);
                applied_rate_ = want;
                rate_ns_ = nowr;
            }
        }
        // Освобождение буферов: onComplete (точно) либо эвристика release_lag
        const bool oncomp = g_cfg.use_oncomplete && it.slot;
        if (oncomp) {
            ASurfaceTransaction_setOnComplete(t, new CbCtx{this, shown_slot_, sc_gen_.load(), ringGen_.load(), sc_, desired_ns}, &Pipeline::onCompleteStatic);
            shown_slot_ = it.slot;
        } else {
            shown_slot_ = nullptr;
        }
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
        presented_++;
        uint64_t seq = out_seq_.fetch_add(1) + 1;
        if (it.slot) {
            if (oncomp) { it.slot->shown_ns.store(now_ns()); it.slot->hold.store(kShown); }
            else it.slot->hold.store(seq + (uint64_t)g_cfg.release_lag);
        }
        AHardwareBuffer_release(it.ahb);     // SurfaceFlinger держит свою ссылку
        it.ahb = nullptr; it.slot = nullptr;
    }

    // Очередь пуста на vsync: если кадров нет дольше hide_ms (или конвейер потерял реальный кадр),
    // прячем оверлей — виден живой слой игры под ним, последний кадр не «залипает» поверх.
    void idle() {
        const bool req = hide_req_.exchange(false);
        if (!visible_.load()) return;
        const bool timeout = g_cfg.hide_ms > 0 &&
                             now_ns() - last_apply_ns_.load() > (int64_t)g_cfg.hide_ms * 1000000LL;
        if (!timeout && !req) return;
        std::lock_guard<std::mutex> g(sc_m_);
        if (!sc_ || !visible_.load()) return;
        ASurfaceTransaction* t = ASurfaceTransaction_create();
        ASurfaceTransaction_setVisibility(t, sc_, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
        visible_.store(false);
        hidden_cnt_++;
        if (hidden_cnt_ <= 3 || g_cfg.debug) LOGI("overlay hidden (%s): the game layer is visible", req ? "real frame lost" : "no frames from the game");
    }

    // Прячет оверлей с текущего потока (idle() зовёт vsync-поток). Нужно при выключении на ходу:
    // vsync вот-вот встанет на паузу, и hide_req_ может остаться необработанным — тогда поверх
    // живой игры остался бы последний показанный кадр.
    void hideNow() {
        std::lock_guard<std::mutex> g(sc_m_);
        if (!sc_ || !visible_.load()) return;
        ASurfaceTransaction* t = ASurfaceTransaction_create();
        ASurfaceTransaction_setVisibility(t, sc_, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
        visible_.store(false);
        hidden_cnt_++;
    }

    void push(OutItem& it) {
        std::lock_guard<std::mutex> g(oq_m_);
        if (on_ == kQ) {
            // Переполнение: реальные кадры не теряем. Жертва — старейший сгенерированный кадр
            // (как в догоне onVsync); если очередь целиком из реальных — выбрасываем входящий
            // сгенерированный, и лишь реальный при полной очереди реальных заменяет старейший.
            int victim = -1;
            for (int i = 0; i < on_; i++)
                if (!oq_[i].real) { victim = i; break; }
            if (victim < 0 && !it.real) { it.drop(); dropped_out_++; return; }
            if (victim < 0) victim = 0;
            oq_[victim].drop();
            for (int i = victim + 1; i < on_; i++) oq_[i - 1] = oq_[i];
            on_--;
            dropped_out_++;
        }
        oq_[on_++] = it;
    }
    void clearQueue() {
        std::lock_guard<std::mutex> g(oq_m_);
        for (int i = 0; i < on_; i++) oq_[i].drop();
        on_ = 0;
    }

    // =============================================================== слой поверх окна игры
    bool ensureSurfaceControl() {
        int ew, eh;
        if (!capture::electedSize(&ew, &eh)) return false;
        ANativeWindow* w = capture::electedWindow();          // точное окно игры (хук Surface)
        if (!w) w = hooks::pickWindow(ew, eh);                // запасной путь: эвристика по размеру
        if (!w) return false;
        if (w == win_) { ANativeWindow_release(w); refreshWinSize(); return true; }
        ASurfaceControl* sc = ASurfaceControl_createFromWindow(w, "myfg_overlay");
        if (!sc) {
            if (sc_fail_cnt_ < 3 || g_cfg.debug)      // housekeeping зовёт раз в секунду: не заливаем лог
                LOGW("ASurfaceControl_createFromWindow failed for %p (%dx%d)", w, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
            sc_fail_cnt_++;
            ANativeWindow_release(w);
            return false;
        }
        sc_fail_cnt_ = 0;
        ASurfaceControl* old; ANativeWindow* oldw;
        {
            std::lock_guard<std::mutex> g(sc_m_);
            old = sc_; oldw = win_;
            sc_ = sc; win_ = w;
            applied_rate_ = 0.0f; applied_ds_ = -1; visible_.store(false);
            sc_gen_.fetch_add(1);              // callbacks старого слоя больше не трогают его
            shown_slot_ = nullptr;
            rate_ns_ = 0;
        }
        // слот, показанный на старом слое, свободен: старый слой уничтожается
        for (auto& sl : real_) { uint64_t e = kShown; sl.hold.compare_exchange_strong(e, 0); }
        for (auto& sl : out_) { uint64_t e = kShown; sl.hold.compare_exchange_strong(e, 0); }
        // старый ASurfaceControl не отпускаем сразу: на него могут прийти запоздалые onComplete
        if (old) {
            if (sc_grave_) ASurfaceControl_release(sc_grave_);
            sc_grave_ = old;
        }
        if (oldw) ANativeWindow_release(oldw);
        refreshWinSize();
        LOGI("overlay layer created on window %p (%dx%d)", w, (int)win_w_, (int)win_h_);
        return true;
    }
    void refreshWinSize() {
        if (!win_) return;
        win_w_ = ANativeWindow_getWidth(win_);
        win_h_ = ANativeWindow_getHeight(win_);
    }

    // =============================================================== Vulkan: ресурсы
    bool createFrameRes() {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_.qfam;
        if (vk_.f.CreateCommandPool(vk_.dev, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
        for (auto& fr : fr_) {
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = pool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 2;      // A (blit, уходит на GPU до ME) + B (WFG+HDR)
            VkCommandBuffer cbs[2] = {};
            if (vk_.f.AllocateCommandBuffers(vk_.dev, &ai, cbs) != VK_SUCCESS) return false;
            fr.cb = cbs[0];
            fr.cb2 = cbs[1];
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vk_.f.CreateFence(vk_.dev, &fi, nullptr, &fr.fence) != VK_SUCCESS) return false;
            if (vk_.can_ts) {
                VkQueryPoolCreateInfo qi{};
                qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 3;
                if (vk_.f.CreateQueryPool(vk_.dev, &qi, nullptr, &fr.qp) != VK_SUCCESS) fr.qp = VK_NULL_HANDLE;
            }
            fr.wait_sem = vk_.createSemaphore(false);
            fr.sig_sem = vk_.createSemaphore(true);
            if (!fr.wait_sem || !fr.sig_sem) return false;
        }
        return true;
    }

    // gen-слоты HDR-варианта: submit завершён, HDR-проход их уже прочитал — отпускаем сразу,
    // а не по таймеру на 3/fps
    void releaseFenceSlots(FrameRes& fr) {
        if (fr.fence_rel_gen == ringGen_) {
            for (Slot* s : fr.fence_rel) {
                uint64_t exp = kPending;
                s->hold.compare_exchange_strong(exp, 0);
            }
        }
        fr.fence_rel.clear();
    }
    // Неблокирующий опрос fence'ей ВСЕХ кадров в полёте. waitFrame() дёргается только для кадра, чей
    // ресурс переиспользуется (раз в kFrames кадров игры) — освобождение только там давало ту же
    // задержку ~3 кадра, что и таймер. Здесь слоты возвращаются, как только GPU закончил submit.
    void reapFrames() {
        for (auto& fr : fr_) {
            if (!fr.submitted || fr.fence_rel.empty()) continue;
            if (vk_.f.WaitForFences(vk_.dev, 1, &fr.fence, VK_TRUE, 0) == VK_SUCCESS) releaseFenceSlots(fr);
        }
    }

    void waitFrame(FrameRes& fr) {
        if (fr.submitted) {
            if (vk_.f.WaitForFences(vk_.dev, 1, &fr.fence, VK_TRUE, 500000000ull) == VK_TIMEOUT) {
                // Submit не завершился за 500 мс (троттлинг/зависание GPU). Сбрасывать fence и
                // переиспользовать командные буферы можно только после гарантированного конца
                // исполнения — иначе порча буферов и риск device loss. Блокировка здесь редкость,
                // поэтому приемлема.
                LOGW("frame submit not finished in 500 ms, waiting idle");
                vk_.f.DeviceWaitIdle(vk_.dev);
            }
            vk_.f.ResetFences(vk_.dev, 1, &fr.fence);
            fr.submitted = false;
            if (fr.timed && fr.qp) {
                uint64_t ts[3] = {0, 0, 0};
                if (vk_.f.GetQueryPoolResults(vk_.dev, fr.qp, 0, 3, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                    const uint64_t t0 = ts[0] & vk_.ts_mask, t1 = ts[1] & vk_.ts_mask, t2 = ts[2] & vk_.ts_mask;
                    // генерация без HDR-проходов: governor не должен наказывать за PQ-кодирование
                    const double gen_ms = (double)((t1 - t0) & vk_.ts_mask) * vk_.ts_ns_per_tick / 1.0e6;
                    const double hdr_ms = (double)((t2 - t1) & vk_.ts_mask) * vk_.ts_ns_per_tick / 1.0e6;
                    if (hdr_ms > 0.0 && hdr_ms < 1000.0)
                        hdr_ema_ = hdr_ema_ > 0.0 ? hdr_ema_ + (hdr_ms - hdr_ema_) * 0.15 : hdr_ms;
                    if (fr.planned > 0 && gen_ms > 0.0 && gen_ms < 1000.0) onGpuSample(gen_ms);
                }
            }
            fr.timed = false;
            releaseFenceSlots(fr);
        }
        for (auto& g : fr.garbage) vk_.destroyImage(&g);
        fr.garbage.clear();
    }

    void freeRings() {
        freeHdr();
        for (auto& s : real_) { vk_.destroyImage(&s.im); s.hold.store(0); int fd = s.rel_fd.exchange(-1); if (fd >= 0) close(fd); }
        for (auto& s : out_) { vk_.destroyImage(&s.im); s.hold.store(0); int fd = s.rel_fd.exchange(-1); if (fd >= 0) close(fd); }
        vk_.destroyPlainImage(&plain_);
        nOut_ = 0; ringW_ = ringH_ = 0;
    }

    bool buildRings(uint32_t w, uint32_t h) {
        clearQueue();
        vk_.f.DeviceWaitIdle(vk_.dev);
        ringGen_++;                                  // колбэки прежнего кольца отбрасываются по rgen
        {
            std::lock_guard<std::mutex> g(sc_m_);
            shown_slot_ = nullptr;                   // слот прежнего кольца уничтожен: не отдавать его как prev
        }
        for (auto& fr : fr_) {
            for (auto* s : fr.fence_rel) { uint64_t exp = kPending; s->hold.compare_exchange_strong(exp, 0); }
            fr.fence_rel.clear(); fr.fence_rel_gen = 0;
            for (auto& g : fr.garbage) vk_.destroyImage(&g); fr.garbage.clear();
        }
        freeRings();
        if (wfg_) vkr_dis_forget_targets(wfg_);
        const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ringMaxGen_ = effMaxGen();
        nOut_ = std::min(kOutMax, std::max(6, 2 * ringMaxGen_ + 2));
        for (int i = 0; i < kReal; i++)
            if (!vk_.createRingImage(w, h, u, &real_[i].im)) { LOGE("ring alloc failed (real %d, %ux%u)", i, w, h); freeRings(); return false; }
        for (int i = 0; i < nOut_; i++)
            if (!vk_.createRingImage(w, h, u, &out_[i].im)) { LOGE("ring alloc failed (out %d, %ux%u)", i, w, h); freeRings(); return false; }
        if (vk_.can_ycbcr && !vk_.createPlainImage(w, h, &plain_)) LOGW("intermediate image for external-format sources could not be created");
        ringW_ = w; ringH_ = h;
        LOGD("rings %ux%u: real x%d, generated x%d", w, h, kReal, nOut_);
        return true;
    }

    // Слот свободен? (без побочных эффектов)
    bool slotFree(Slot& sl, uint64_t seq, int64_t now) {
        uint64_t hv = sl.hold.load();
        if (hv == kPending) return false;
        if (hv == kShown) { if (now - sl.shown_ns.load() <= kShownTimeoutNs) return false; }
        else if (hv > seq) return false;
        int fd = sl.rel_fd.load();
        if (fd >= 0) { struct pollfd pf = {fd, POLLIN, 0}; if (poll(&pf, 1, 0) == 0) return false; }   // SurfaceFlinger ещё читает
        return true;
    }
    Slot* takeFree(Slot* arr, int n) {
        const uint64_t seq = out_seq_.load();
        const int64_t now = now_ns();
        for (int i = 0; i < n; i++) {
            if (!slotFree(arr[i], seq, now)) continue;
            uint64_t hv = arr[i].hold.load();
            if (!arr[i].hold.compare_exchange_strong(hv, kPending)) continue;     // опередил onComplete/таймаут
            int fd = arr[i].rel_fd.exchange(-1);
            if (fd >= 0) close(fd);
            return &arr[i];
        }
        return nullptr;
    }
    int freeCount(Slot* arr, int n) {
        const uint64_t seq = out_seq_.load();
        const int64_t now = now_ns();
        int c = 0;
        for (int i = 0; i < n; i++) if (slotFree(arr[i], seq, now)) c++;
        return c;
    }
    // Максимум сгенерированных кадров на кадр игры с учётом режима множителя.
    int effMaxGen() const {
        int g = (int)std::min<uint32_t>((uint32_t)g_cfg.max_gen, VKR_DIS_MAX_GENERATIONS);
        if (g_cfg.multiplier >= 2) g = std::max(g, std::min(g_cfg.multiplier - 1, (int)VKR_DIS_MAX_GENERATIONS));
        return std::min(g, (kOutMax - 2) / 2);
    }

    // Образы буферов, которых игра давно не показывала (пересоздан свопчейн), держат ссылки на AHB и память.
    // Выгружаем те, что не использовались 240 кадров; destroy под DeviceWaitIdle — как при вытеснении.
    void purgeStale() {
        std::vector<uint64_t> dead;
        for (auto& kv : srcCache_) if (tick_ - kv.second.last_use > 240) dead.push_back(kv.first);
        if (dead.empty()) return;
        vk_.f.DeviceWaitIdle(vk_.dev);
        for (uint64_t k : dead) {
            auto it = srcCache_.find(k);
            vk_.destroyImage(&it->second);
            srcCache_.erase(it);
        }
        LOGD("source cache: released %zu stale buffer(s)", dead.size());
    }

    // Кэш импортов игровых буферов по AHardwareBuffer_getId (API 31+).
    bool getSource(capture::Job& j, FrameRes& fr, AhbImage** out, AhbImage* tmp) {
        uint64_t id = 0;
        bool cacheable = getId_ && getId_(j.src, &id) == 0;
        // Без getId ключ — указатель: пока образ в кэше, он держит ссылку на AHB, так что указатель не переиспользуется.
        if (!cacheable) { id = (uint64_t)(uintptr_t)j.src | (1ull << 63); cacheable = true; }
        if (cacheable) {
            auto it = srcCache_.find(id);
            if (it != srcCache_.end()) { it->second.last_use = ++tick_; *out = &it->second; return true; }
        }
        AHardwareBuffer_acquire(j.src);                   // ссылка образа (отдельно от ссылки задания)
        if (!vk_.importAhb(j.src, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, tmp)) { AHardwareBuffer_release(j.src); return false; }
        if (!cacheable) { fr.garbage.push_back(*tmp); *out = &fr.garbage.back(); return true; }
        if (srcCache_.size() >= 8) {
            vk_.f.DeviceWaitIdle(vk_.dev);
            auto old = srcCache_.begin();
            for (auto i = srcCache_.begin(); i != srcCache_.end(); ++i) if (i->second.last_use < old->second.last_use) old = i;
            vk_.destroyImage(&old->second);
            srcCache_.erase(old);
        }
        tmp->last_use = ++tick_;
        *out = &(srcCache_[id] = *tmp);
        return true;
    }

    void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                 uint32_t sq = VK_QUEUE_FAMILY_IGNORED, uint32_t dq = VK_QUEUE_FAMILY_IGNORED) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = sq; b.dstQueueFamilyIndex = dq;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_.f.CmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // =============================================================== адаптивная нагрузка
    // GPU делится с игрой. Мерим время нашего submit (timestamp-запросы) и сравниваем с долей интервала
    // между кадрами игры (gpu_budget %). Перегруз 2 проверки подряд (по 0.5 с) -> уровень выше; запас
    // (< 60% бюджета) 6 с подряд -> уровень ниже; между сменами не меньше 2 с.
    // Уровни 1..3 уменьшают разрешение потока (x0.8 за уровень, не ниже 96 px), дальше убавляется число
    // сгенерированных кадров (минимум 1). Время считается только по кадрам, где что-то генерировалось.
    void onGpuSample(double ms) {
        gpu_ema_ = gpu_ema_ > 0.0 ? gpu_ema_ + (ms - gpu_ema_) * 0.15 : ms;
        if (!g_cfg.adaptive) {
            if (gov_level_ != 0) { gov_level_ = 0; LOGI("governor off: level 0"); }
            return;
        }
        const int64_t now = now_ns();
        if (now - gov_check_ns_ < 500000000LL) return;
        gov_check_ns_ = now;
        if (src_ema_ <= 0.0 || src_n_ < 30) return;
        const double budget = (1000.0 / src_ema_) * (double)g_cfg.gpu_budget / 100.0;
        const int maxLevel = 3 + std::max(0, effMaxGen() - 1);
        if (gpu_ema_ > budget) {
            gov_over_++; gov_under_ = 0;
            if (gov_over_ >= 2 && gov_level_ < maxLevel && now - gov_change_ns_ > 2000000000LL) {
                gov_level_++; gov_change_ns_ = now; gov_over_ = 0;
                LOGI("governor: level %d (gpu %.1f ms > budget %.1f ms): flow_side=%u gen cap=%d", gov_level_, gpu_ema_, budget,
                     govFlow(), govGenCap());
            }
        } else if (gpu_ema_ < budget * 0.6) {
            gov_under_++; gov_over_ = 0;
            if (gov_under_ >= 12 && gov_level_ > 0 && now - gov_change_ns_ > 2000000000LL) {
                gov_level_--; gov_change_ns_ = now; gov_under_ = 0;
                LOGI("governor: level %d (gpu %.1f ms, budget %.1f ms): flow_side=%u gen cap=%d", gov_level_, gpu_ema_, budget,
                     govFlow(), govGenCap());
            }
        } else { gov_over_ = gov_under_ = 0; }
    }
    uint32_t govFlow() const {
        double f = (double)g_cfg.flow_side;
        for (int i = 0; i < std::min(gov_level_, 3); i++) f *= 0.8;
        return (uint32_t)std::max(f, std::min(96.0, (double)g_cfg.flow_side));
    }
    int govGenCap() const {
        const int base = effMaxGen();
        const int cut = std::max(0, gov_level_ - 3);
        return base <= 1 ? base : std::max(1, base - cut);
    }

    // =============================================================== SDR -> HDR10
    void freeHdr() {
        for (int i = 0; i < kHdrMax; i++) {
            vk_.destroyImage(&hdrS_[i].im);
            hdrS_[i].hold.store(0);
            int fd = hdrS_[i].rel_fd.exchange(-1);
            if (fd >= 0) close(fd);
        }
        for (auto& v : realV_) { if (v) vk_.f.DestroyImageView(vk_.dev, v, nullptr); v = VK_NULL_HANDLE; }
        for (auto& v : outV_) { if (v) vk_.f.DestroyImageView(vk_.dev, v, nullptr); v = VK_NULL_HANDLE; }
        for (auto& v : hdrV_) { if (v) vk_.f.DestroyImageView(vk_.dev, v, nullptr); v = VK_NULL_HANDLE; }
        for (auto& ph : plainH_) vk_.destroyPlainImage(&ph);
        for (auto& b : hdrInited_) b = false;
        for (auto& b : phInited_) b = false;
        hdrDirect_ = false;
        nHdr_ = 0; hdrW_ = hdrH_ = 0;
    }

    // Пик панели для шейдера: ключ hdr_peak (вручную) либо значение панели, иначе 1000 нит.
    int hdrPeak() const {
        if (g_cfg.hdr_peak > 0) return g_cfg.hdr_peak;
        if (panelPeak_ > 0) return std::min(2000, std::max(400, panelPeak_));
        return 1000;
    }

    // Панель умеет HDR? Запрос к Display через JNI делается один раз; если он не удался (приложение ещё не готово,
    // скрытые API закрыты), пробуем ещё два раза с паузой 5 с, затем работаем вслепую (с предупреждением).
    bool hdrAllowed() {
        if (hdrState_ == 1 || hdrState_ == 4) return true;
        if (hdrState_ == 2) return false;
        const int64_t n = now_ns();
        if (hdrTryNs_ != 0 && n - hdrTryNs_ < 5000000000LL) return false;
        hdrTryNs_ = n;
        bool sup = false; int peak = 0;
        if (display::hdrInfo(&sup, &peak)) {
            panelPeak_ = peak;
            if (sup) { hdrState_ = 1; LOGI("HDR: panel reports HDR support, peak %d nit (%s)", peak, peak > 0 ? "from the panel" : "not reported, using 1000"); }
            else { hdrState_ = 2; LOGW("HDR: the panel reports no HDR support, HDR output is disabled"); }
        } else if (++hdrTries_ >= 3) {
            hdrState_ = 4;
            LOGW("HDR: could not query the panel (hidden API?), enabling anyway, peak %d nit", hdrPeak());
        }
        return hdrState_ == 1 || hdrState_ == 4;
    }

    HdrParams hdrParams() const {
        HdrParams p;
        p.paper_white = (float)g_cfg.hdr_pw;
        p.peak = (float)hdrPeak();
        p.knee = (float)g_cfg.hdr_knee / 100.0f;
        p.boost = (float)g_cfg.hdr_boost / 100.0f;
        p.gamma = g_cfg.hdr_gamma == 0 ? 0.0f : (float)g_cfg.hdr_gamma / 10.0f;
        p.contrast = (float)g_cfg.hdr_ct / 100.0f;
        p.saturation = (float)g_cfg.hdr_sat / 100.0f;
        p.seed = (float)(frames_ & 1023u);
        return p;
    }

    // Кольцо HDR-кадров w x h (того же размера, что кадр игры). Сначала пробуем прямую запись
    // compute-шейдером в кольцо (usage STORAGE): проход = один dispatch, без копии и без ожидания
    // соседних проходов. Если AHB/драйвер не даёт storage для формата — откат на старую схему:
    // TRANSFER_DST-кольцо + kPlain промежуточных storage-образов по кругу + CmdCopyImage.
    bool buildHdrRing(uint32_t w, uint32_t h, int n) {
        for (int i = 0; i < kReal; i++) if (!(realV_[i] = hdr_.makeView(vk_, real_[i].im.image, VK_FORMAT_R8G8B8A8_UNORM))) return false;
        for (int i = 0; i < nOut_; i++) if (!(outV_[i] = hdr_.makeView(vk_, out_[i].im.image, VK_FORMAT_R8G8B8A8_UNORM))) return false;
        auto clearRing = [&]() {
            for (int i = 0; i < kHdrMax; i++) {
                if (hdrV_[i]) { vk_.f.DestroyImageView(vk_.dev, hdrV_[i], nullptr); hdrV_[i] = VK_NULL_HANDLE; }
                vk_.destroyImage(&hdrS_[i].im);
                hdrInited_[i] = false;
            }
        };
        // 1) прямая запись шейдером в кольцо
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            if (!vk_.createRingImage(w, h, VK_IMAGE_USAGE_STORAGE_BIT, &hdrS_[i].im, hdr_.tenBit())) ok = false;
            else if (hdrS_[i].im.format != hdr_.storageFormat()) ok = false;
            else if (!(hdrV_[i] = hdr_.makeView(vk_, hdrS_[i].im.image, hdr_.storageFormat()))) ok = false;
        }
        if (ok) { hdrDirect_ = true; LOGI("hdr: shader writes directly into the ring (storage), no copy"); return true; }
        clearRing();
        // 2) копия через промежуточные образы (по кругу, чтобы проходы не ждали друг друга)
        for (int i = 0; i < kPlain; i++) {
            if (!vk_.createPlainImage(w, h, &plainH_[i], hdr_.storageFormat())) return false;
            phInited_[i] = false;
        }
        const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (int i = 0; i < n; i++) {
            if (!vk_.createRingImage(w, h, u, &hdrS_[i].im, hdr_.tenBit())) return false;
            if (hdrS_[i].im.format != hdr_.storageFormat()) {     // копия образ->образ требует совпадения форматов
                LOGW("hdr: ring format %d != pass format %d", (int)hdrS_[i].im.format, (int)hdr_.storageFormat());
                return false;
            }
        }
        hdrDirect_ = false;
        LOGI("hdr: AHB without STORAGE for format %d, HDR pass via intermediate copy (x%d)", (int)hdr_.storageFormat(), kPlain);
        return true;
    }

    bool ensureHdr(uint32_t w, uint32_t h) {
        if (hdrFail_) return false;
        if (!hdrInit_) {
            // 10 бит (A2B10G10R10), если драйвер даёт storage-запись; иначе RGBA8 с дизерингом
            bool ten = vk_.formatHasStorage(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
            if (ten && !hdr_.init(vk_, true)) { hdr_.destroy(vk_); ten = false; }
            if (!ten && !hdr_.init(vk_, false)) { hdrFail_ = true; LOGW("HDR: pass unavailable, output stays SDR"); return false; }
            hdrInit_ = true;
        }
        if (hdrW_ == w && hdrH_ == h && nHdr_ > 0) return true;
        clearQueue();
        vk_.f.DeviceWaitIdle(vk_.dev);
        freeHdr();
        // очередь вывода (kQ) держит свои слоты, плюс план и real: если колец меньше,
        // часть кадров уходит как SDR между PQ (мерцание яркости)
        const int n = std::min(kHdrMax, kQ + ringMaxGen_ + 2);
        bool ok = buildHdrRing(w, h, n);
        if (!ok && hdr_.tenBit()) {            // 10-битный буфер не получился (AHB/импорт): откат на 8 бит
            LOGW("HDR: 10-bit ring %ux%u could not be created, falling back to 8-bit with dithering", w, h);
            freeHdr();
            hdr_.destroy(vk_);
            if (hdr_.init(vk_, false)) ok = buildHdrRing(w, h, n);
        }
        if (!ok) { freeHdr(); hdrFail_ = true; LOGW("HDR: output ring %ux%u could not be created, output stays SDR", w, h); return false; }
        nHdr_ = n; hdrW_ = w; hdrH_ = h;
        LOGI("HDR output %ux%u: %s PQ BT.2020, paper white %d nit, peak %d nit (%s), knee %d%% boost %d%% gamma %s contrast %d%% saturation %d%%, ring x%d",
             w, h, hdr_.tenBit() ? "10-bit" : "8-bit+dither", g_cfg.hdr_pw, hdrPeak(), g_cfg.hdr_peak > 0 ? "manual" : (panelPeak_ > 0 ? "panel" : "default"),
             g_cfg.hdr_knee, g_cfg.hdr_boost,
             g_cfg.hdr_gamma == 0 ? "sRGB" : std::to_string(g_cfg.hdr_gamma / 10.0).substr(0, 3).c_str(), g_cfg.hdr_ct, g_cfg.hdr_sat, n);
        return true;
    }

    // Слот-источник апскейла: не нужен для показа, держим по времени (история WFG на следующем кадре).
    // Используется только для real: gen-слоты HDR-варианта освобождаются по fence submit'а (waitFrame).
    void holdTimed(Slot* sl) {
        const double fps = src_ema_ > 1.0 ? src_ema_ : 30.0;
        int64_t ns = (int64_t)(3.0e9 / fps);
        ns = std::min<int64_t>(200000000LL, std::max<int64_t>(60000000LL, ns));
        sl->shown_ns.store(now_ns() - kShownTimeoutNs + ns);
        sl->hold.store(kShown);
    }

    // =============================================================== детектор повторных кадров
    // Игра может постить 60 к/с при 30 уникальных. Такие кадры нельзя считать источником: WFG видел бы 60 fps
    // и ничего не генерировал. Берём уменьшенную копию кадра (64x36, blit с LINEAR) в host-буфер и сравниваем
    // со следующим; повтор пропускаем целиком (интервал dt считается между уникальными кадрами).
    // Цена: один короткий submit и ожидание его на CPU на каждый кадр, поэтому опция выключена по умолчанию.
    static constexpr uint32_t kSigW = 64, kSigH = 36;

    bool ensureSig() {
        if (sigReady_) return true;
        if (sigFail_) return false;
        sigFail_ = true;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {kSigW, kSigH, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.f.CreateImage(vk_.dev, &ici, nullptr, &sigImg_) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vk_.f.GetImageMemoryRequirements(vk_.dev, sigImg_, &mr);
        uint32_t mt;
        if (!vk_.findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mt) && !vk_.findMemType(mr.memoryTypeBits, 0, &mt)) return false;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        if (vk_.f.AllocateMemory(vk_.dev, &mai, nullptr, &sigImgMem_) != VK_SUCCESS) return false;
        if (vk_.f.BindImageMemory(vk_.dev, sigImg_, sigImgMem_, 0) != VK_SUCCESS) return false;

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = kSigW * kSigH * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vk_.f.CreateBuffer(vk_.dev, &bci, nullptr, &sigBuf_) != VK_SUCCESS) return false;
        vk_.f.GetBufferMemoryRequirements(vk_.dev, sigBuf_, &mr);
        sigCoherent_ = true;
        if (!vk_.findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) {
            sigCoherent_ = false;
            if (!vk_.findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &mt)) return false;
        }
        mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        if (vk_.f.AllocateMemory(vk_.dev, &mai, nullptr, &sigBufMem_) != VK_SUCCESS) return false;
        if (vk_.f.BindBufferMemory(vk_.dev, sigBuf_, sigBufMem_, 0) != VK_SUCCESS) return false;
        void* mp = nullptr;
        if (vk_.f.MapMemory(vk_.dev, sigBufMem_, 0, VK_WHOLE_SIZE, 0, &mp) != VK_SUCCESS || !mp) return false;
        sigMap_ = (uint8_t*)mp;

        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (vk_.f.AllocateCommandBuffers(vk_.dev, &ai, &sigCb_) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vk_.f.CreateFence(vk_.dev, &fi, nullptr, &sigFence_) != VK_SUCCESS) return false;
        sigSem_ = vk_.createSemaphore(false);
        if (!sigSem_) return false;
        prevSig_.assign((size_t)kSigW * kSigH * 4, 0);
        sigFail_ = false; sigReady_ = true;
        LOGD("duplicate-frame detector ready (%ux%u signature, threshold %d)", kSigW, kSigH, g_cfg.dup_thresh);
        return true;
    }

    // true — кадр повторяет предыдущий уникальный; его нужно пропустить.
    bool isDuplicate(capture::Job& j, AhbImage* src) {
        if (!g_cfg.dup_detect) { prevValid_ = false; return false; }
        if (src->external) {          // буфер с внешним форматом (YCbCr) сравнить нечем: детектор для него не работает
            prevValid_ = false;
            if (!dupExtWarned_) { dupExtWarned_ = true; LOGW("dup_detect unavailable: the game buffer has an external (YCbCr) format"); }
            return false;
        }
        if (!ensureSig()) return false;
        const uint32_t FOREIGN = VK_QUEUE_FAMILY_FOREIGN_EXT, Q = vk_.qfam;
        int fd2 = j.fence_fd >= 0 ? dup(j.fence_fd) : -1;
        bool gpuWait = false;
        if (fd2 >= 0) {
            if (vk_.importSyncFd(sigSem_, fd2)) gpuWait = true;
            else { struct pollfd pf = {fd2, POLLIN, 0}; poll(&pf, 1, 20); close(fd2); }
        }
        VkCommandBuffer cb = sigCb_;
        vk_.f.ResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.f.BeginCommandBuffer(cb, &bi);
        barrier(cb, src->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FOREIGN, Q);
        barrier(cb, sigImg_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[0] = {j.cl, j.ct, 0}; bl.srcOffsets[1] = {j.cr, j.cb, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[1] = {(int32_t)kSigW, (int32_t)kSigH, 1};
        vk_.f.CmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sigImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        barrier(cb, sigImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy rg{};
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {kSigW, kSigH, 1};
        vk_.f.CmdCopyImageToBuffer(cb, sigImg_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sigBuf_, 1, &rg);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk_.f.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        barrier(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
        vk_.f.EndCommandBuffer(cb);

        const VkPipelineStageFlags wst = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = gpuWait ? 1 : 0; si.pWaitSemaphores = &sigSem_; si.pWaitDstStageMask = &wst;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        if (vk_.f.QueueSubmit(vk_.queue, 1, &si, sigFence_) != VK_SUCCESS) return false;
        VkResult wr = vk_.f.WaitForFences(vk_.dev, 1, &sigFence_, VK_TRUE, 50000000ull);
        if (wr != VK_SUCCESS) wr = vk_.f.WaitForFences(vk_.dev, 1, &sigFence_, VK_TRUE, 500000000ull);
        vk_.f.ResetFences(vk_.dev, 1, &sigFence_);
        if (wr != VK_SUCCESS) return false;
        if (!sigCoherent_) {
            VkMappedMemoryRange mr{};
            mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; mr.memory = sigBufMem_; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
            vk_.f.InvalidateMappedMemoryRanges(vk_.dev, 1, &mr);
        }
        const size_t n = (size_t)kSigW * kSigH * 4;
        bool dup = false;
        if (prevValid_) {
            uint64_t sum = 0;
            for (size_t i = 0; i < n; i += 4)
                sum += (uint64_t)abs((int)sigMap_[i] - prevSig_[i]) + (uint64_t)abs((int)sigMap_[i + 1] - prevSig_[i + 1]) +
                       (uint64_t)abs((int)sigMap_[i + 2] - prevSig_[i + 2]);
            const double mean = (double)sum / ((double)kSigW * kSigH * 3.0);
            dup = mean <= (double)g_cfg.dup_thresh;
        }
        if (!dup) { memcpy(prevSig_.data(), sigMap_, n); prevValid_ = true; }
        return dup;
    }

    // =============================================================== «якорь» частоты панели
    // Отдельный прозрачный слой-ребёнок окна игры, который обновляется с частотой панели и несёт голос
    // panel_hz для SurfaceFlinger: голос держится, даже когда оверлей молчит (игра 30 к/с, конвейер спит).
    void anchorStart() {
        std::lock_guard<std::mutex> g(anchor_m_);
        anchor_run_.store(true);
        if (anchor_alive_.load()) return;
        anchor_alive_.store(true);
        std::thread([this] {
            for (;;) {
                anchorLoop();
                if (anchor_run_.load()) usleep(1000 * 1000);   // цикл вышел при включённом якоре (буферы не создались): повтор через секунду
                std::lock_guard<std::mutex> g2(anchor_m_);
                if (!anchor_run_.load()) { anchor_alive_.store(false); return; }
            }
        }).detach();
    }
    void anchorLoop() {
        pthread_setname_np(pthread_self(), "myfg-anchor");
        AHardwareBuffer* b[2] = {};
        AHardwareBuffer_Desc d{};
        d.width = 16; d.height = 16; d.layers = 1; d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        bool ok = true;
        for (auto& x : b) {
            if (AHardwareBuffer_allocate(&d, &x) != 0 || !x) { ok = false; break; }
            void* p = nullptr;
            if (AHardwareBuffer_lock(x, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr, &p) == 0 && p) {
                AHardwareBuffer_Desc dd{};
                AHardwareBuffer_describe(x, &dd);
                memset(p, 0, (size_t)dd.stride * 16 * 4);            // полностью прозрачные пиксели
                AHardwareBuffer_unlock(x, nullptr);
            } else ok = false;
        }
        ASurfaceControl* a = nullptr;
        ANativeWindow* w = nullptr;
        int i = 0;
        int64_t next = now_ns();
        int64_t chk = 0;
        LOGD("panel-rate anchor thread started (%d Hz)", g_cfg.panel_hz);
        while (ok && anchor_run_.load()) {
            int hz = g_cfg.panel_hz;
            if (therm_on_.load() && hz > g_cfg.thermal_hz) hz = g_cfg.thermal_hz;   // троттлинг и тут
            if (hz <= 0) { usleep(200 * 1000); continue; }
            const int64_t now = now_ns();
            if (!a || now - chk > 1000000000LL) {
                chk = now;
                ANativeWindow* cur = nullptr;
                { std::lock_guard<std::mutex> g(sc_m_); cur = win_; if (cur) ANativeWindow_acquire(cur); }
                if (cur != w) {                                    // окно игры сменилось: пересоздаём слой
                    if (a) {
                        ASurfaceTransaction* t = ASurfaceTransaction_create();
                        ASurfaceTransaction_setVisibility(t, a, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
                        ASurfaceTransaction_apply(t); ASurfaceTransaction_delete(t);
                        ASurfaceControl_release(a); a = nullptr;
                    }
                    if (w) ANativeWindow_release(w);
                    w = cur;
                    if (w) a = ASurfaceControl_createFromWindow(w, "myfg_anchor");
                } else if (cur) ANativeWindow_release(cur);
                if (!a) { usleep(300 * 1000); continue; }
            }
            ASurfaceTransaction* t = ASurfaceTransaction_create();
            ASurfaceTransaction_setBuffer(t, a, b[i & 1], -1);
            ASurfaceTransaction_setVisibility(t, a, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
            ASurfaceTransaction_setBufferTransparency(t, a, ASURFACE_TRANSACTION_TRANSPARENCY_TRANSPARENT);
            ASurfaceTransaction_setZOrder(t, a, 0);
            ARect src = {0, 0, 16, 16}, dst = {0, 0, 1, 1};
            ASurfaceTransaction_setGeometry(t, a, src, dst, 0);
            if (setFrameRate_) setFrameRate_(t, a, (float)hz, 0, 1);
            ASurfaceTransaction_apply(t);
            ASurfaceTransaction_delete(t);
            i++;
            next += 1000000000LL / hz;
            const int64_t n2 = now_ns();
            if (next < n2) next = n2;                              // отстали — не догоняем пачкой
            timespec ts = {(time_t)(next / 1000000000LL), (long)(next % 1000000000LL)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        }
        if (a) {
            ASurfaceTransaction* t = ASurfaceTransaction_create();
            ASurfaceTransaction_setVisibility(t, a, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
            ASurfaceTransaction_apply(t); ASurfaceTransaction_delete(t);
            ASurfaceControl_release(a);
        }
        if (w) ANativeWindow_release(w);
        for (auto& x : b) if (x) AHardwareBuffer_release(x);
        LOGD("panel-rate anchor thread stopped");
    }

    // Раз в секунду: слой/окно, голос частоты на окно игры, якорь, счётчик FPS.
    // Генерация выключена на ходу (enable=0): оверлей прячется, а счётчик FPS, якорь и голос частоты
    // панели снимаются — иначе окно игры держало бы повышенную частоту без всякой генерации.
    int effPanelHz() const {          // голос частоты с учётом троттлинга батареи
        int hz = g_cfg.panel_hz;
        if (therm_on_.load() && hz > g_cfg.thermal_hz) hz = g_cfg.thermal_hz;
        return hz;
    }
    void voteGameRate() {             // немедленный перевыбор голоса окна игры (смена состояния терм)
        // №9: голосуем только при тех же условиях, что и периодический голос в housekeeping:
        // модуль включён, panel_game_vote=1 и есть что голосовать. При выключении модуля голос
        // с окна снимается — терм не должен ни ставить новый (перегрев), ни возвращать снятый
        // (охлаждение): иначе выключенный модуль продолжал бы давить/держать частоту окна игры.
        if (!winRate_ || !g_cfg.enable || !g_cfg.panel_game_vote || effPanelHz() <= 0) return;
        ANativeWindow* w = nullptr;
        { std::lock_guard<std::mutex> g(sc_m_); w = win_; if (w) ANativeWindow_acquire(w); }
        if (w) { winRate_(w, (float)effPanelHz(), 0, 1); ANativeWindow_release(w); }
    }
    // Замер температуры раз в 2 с в потоке кадров. Включение — два замера подряд выше hi (не
    // откликаться на секундный всплеск от быстрой зарядки/нагрузки), выключение — ниже lo (гистерезис).
    void thermalTick(const int64_t now) {
        if (!g_cfg.thermal) {
            if (therm_on_.load()) { therm_on_.store(false); LOGI("thermal: full rate restored (disabled in settings)"); voteGameRate(); }
            therm_over_ = 0;
            return;
        }
        if (now - therm_chk_ns_ < 2000000000LL) return;
        therm_chk_ns_ = now;
        const int t = batteryTenthsC();
        if (t == INT_MIN) {
            if (!therm_no_sensor_) { therm_no_sensor_ = true; LOGI("thermal: no battery temp sensor, throttle idle"); }
            if (therm_on_.load()) { therm_on_.store(false); LOGI("thermal: full rate restored"); voteGameRate(); }
            return;
        }
        therm_t_ = t;
        if (!therm_on_.load()) {
            if (t >= g_cfg.thermal_hi * 10) {
                if (++therm_over_ >= 2) {
                    therm_on_.store(true);
                    LOGI("thermal: battery %.1fC >= %dC, output capped to %d Hz", t / 10.0, g_cfg.thermal_hi, g_cfg.thermal_hz);
                    voteGameRate();
                }
            } else therm_over_ = 0;
        } else if (t <= g_cfg.thermal_lo * 10) {
            therm_on_.store(false); therm_over_ = 0;
            LOGI("thermal: battery %.1fC <= %dC, full rate restored", t / 10.0, g_cfg.thermal_lo);
            voteGameRate();
        }
    }
    void housekeeping() {
        const int64_t n = now_ns();
        if (n - last_check_ < 1000000000LL) return;
        last_check_ = n;
        // B: пейсинг выключили на ходу — надбавка выбора таймлайна больше не действует
        if (!g_cfg.pacing && pace_guard_ns_.load(std::memory_order_relaxed) != 0)
            pace_guard_ns_.store(0, std::memory_order_relaxed);
        thermalTick(n);                          // замер батареи — свой темп (раз в 2 с)
        const bool en = g_cfg.enable != 0;
        if (en != enabled_) {
            enabled_ = en;
            if (!en) {
                clearQueue();
                hide_req_.store(true);      // зайдёт ещё vsync-колбэк — пусть спрячет оверлей и он
                hideNow();                  // но после паузы он может не зайти вовсе: прячем сами
                want_rate_.store(0.0f);
                src_ema_ = 0.0; src_n_ = 0; lastTs_ = 0; lastPts_ = 0; lastPtsValid_ = false;
                lamValid_ = false; plan_last_ = -1; plan_stable_ = 0;
                if (winRate_) {                        // снять голос частоты с окна игры
                    ANativeWindow* w = nullptr;
                    { std::lock_guard<std::mutex> g(sc_m_); w = win_; if (w) ANativeWindow_acquire(w); }
                    if (w) { winRate_(w, 0.0f, 0, 1); ANativeWindow_release(w); }
                }
            }
            vsync_.pause(!en);             // паузу ставим, когда оверлей уже гарантированно спрятан
            LOGI(en ? "frame generation resumed (enabled in settings)" : "frame generation paused (disabled in settings)");
        }
        if (!en) {
            anchor_run_.store(false);
            hud::stop();
            return;
        }
        ensureSurfaceControl();
        if (g_cfg.panel_game_vote && effPanelHz() > 0 && winRate_) {
            ANativeWindow* w = nullptr;
            { std::lock_guard<std::mutex> g(sc_m_); w = win_; if (w) ANativeWindow_acquire(w); }
            if (w) { winRate_(w, (float)effPanelHz(), 0, 1); ANativeWindow_release(w); }
        }
        if (g_cfg.panel_anchor && g_cfg.panel_hz > 0) anchorStart();
        else anchor_run_.store(false);
        if (g_cfg.hud) hud::ensure(hudSource());
        else hud::stop();
    }

    // Счётчик FPS (hud.cpp): окно игры и счётчики кадров берутся отсюда.
    hud::Source hudSource() {
        hud::Source s;
        s.ctx = this;
        s.acquire_window = [](void* c) -> ANativeWindow* {
            Pipeline* p = (Pipeline*)c;
            std::lock_guard<std::mutex> g(p->sc_m_);
            if (p->win_) ANativeWindow_acquire(p->win_);
            return p->win_;
        };
        s.counters = [](void* c, uint64_t* src, uint64_t* gen, uint64_t* shown, double* gpu) {
            Pipeline* p = (Pipeline*)c;
            *src = __atomic_load_n(&p->src_cnt_, __ATOMIC_RELAXED);      // пишут другие потоки; для счётчика кадров этого достаточно
            *gen = __atomic_load_n(&p->gen_cnt_, __ATOMIC_RELAXED);
            *shown = __atomic_load_n(&p->presented_, __ATOMIC_RELAXED);
            *gpu = p->gpu_ema_ > 0.0 ? p->gpu_ema_ : 0.0;
        };
        return s;
    }

    // =============================================================== WFG
    bool createWfg() {
        if (!vkr_dis_bind_dispatch(vk_.gipa, vk_.inst)) { LOGE("WFG: Vulkan entry points could not be resolved"); return false; }
        wfg_ = vkr_dis_create(vk_.dev, vk_.phys);
        if (!wfg_) { LOGE("WFG: engine creation failed"); return false; }
        return true;
    }

    // =============================================================== главный цикл
    void run() {
        pthread_setname_np(pthread_self(), "myfg-pipe");
        void* h = dlopen("libandroid.so", RTLD_NOW);
        if (h) {
            setFrameTimeline_ = (PFN_setFrameTimeline)dlsym(h, "ASurfaceTransaction_setFrameTimeline");
            // API 31: с выбором стратегии смены режима; иначе API 30. Вызываем с 5 аргументами:
            // у 4-аргументного варианта лишний параметр ни на что не влияет (arm64).
            setFrameRate_ = (PFN_setFrameRate)dlsym(h, "ASurfaceTransaction_setFrameRateWithChangeStrategy");
            if (!setFrameRate_) setFrameRate_ = (PFN_setFrameRate)dlsym(h, "ASurfaceTransaction_setFrameRate");
            winRate_ = (PFN_winRate)dlsym(h, "ANativeWindow_setFrameRateWithChangeStrategy");
            if (!winRate_) winRate_ = (PFN_winRate)dlsym(h, "ANativeWindow_setFrameRate");
        }
        // panel_hz/display_fps/auto_rate молча не работают, если точек входа нет (старый Android):
        // пользователь должен увидеть в логе, почему голос частоты не действует.
        if (!setFrameRate_ && !winRate_ && (g_cfg.panel_hz > 0 || g_cfg.display_fps > 0 || g_cfg.auto_rate))
            LOGW("frame rate hints are ignored: setFrameRate entry points not found (old Android?)");
        fallback::markOwnThread();              // vkCreateDevice модуля не должен проходить через хук запасного пути
        getId_ = (PFN_getId)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getId");

        // 1. ждём "игровую" очередь и окно
        for (int tick = 0; !(capture::electedSize(&last_w_, &last_h_) && windowReady()); tick++) {
            usleep(100 * 1000);
            if (tick == 99 || (tick > 99 && tick % 600 == 599)) {      // 10 с без кадров, дальше раз в минуту
                auto cs = capture::stats();
                int ew = 0, eh = 0;
                bool el = capture::electedSize(&ew, &eh);
                LOGW("no usable game frames yet (%d s): surfCalls=%llu queueCalls=%llu request=%llu attach=%llu queued=%llu slotMiss=%llu fmtRej=%llu elected=%d(%dx%d) surfaceHook=%d",
                     (tick + 1) / 10, (unsigned long long)cs.surf_calls, (unsigned long long)cs.queue_calls, (unsigned long long)cs.requested,
                     (unsigned long long)cs.attached, (unsigned long long)cs.queued, (unsigned long long)cs.slot_miss,
                     (unsigned long long)cs.rejected_fmt, el, ew, eh, (int)capture::surfaceHooked());
            }
        }

        // 2. Vulkan + WFG; при неудаче модуль тихо отключается (игра идёт как без него)
        if (!vk_.init() || !createFrameRes() || !createWfg()) {
            LOGE("pipeline init failed; frame generation disabled for this process");
            capture::disable();
            return;
        }
        // Слой может не создаться сразу (окно ещё без SurfaceControl / пересоздаётся): повторяем ~5 с
        bool overlay = false;
        for (int i = 0; i < 50 && !(overlay = ensureSurfaceControl()); i++) usleep(100 * 1000);
        if (!overlay) { LOGE("overlay layer could not be created"); capture::disable(); return; }
        vsync_.start(&Pipeline::onVsyncStatic, this);
        LOGI("pipeline up");

        last_check_ = now_ns();
        stat_t_ = last_check_;      // иначе первый интервал статистики считался бы от epoch (stat_t_=0)
                                    // и первая строка выходила с мусорными "0.0/s" / ложным idle
        for (;;) {
            capture::Job j;
            if (!capture::pop(&j, 100)) { housekeeping(); printStats(); continue; }
            housekeeping();
            if (!g_cfg.enable) { j.release(); printStats(); continue; }     // выключено на ходу: кадры не трогаем
            process(j);
            j.release();
            printStats();
        }
    }

    bool windowReady() {
        int w, h;
        if (!capture::electedSize(&w, &h)) return false;
        ANativeWindow* win = capture::electedWindow();
        if (!win) win = hooks::pickWindow(w, h);
        if (!win) return false;
        ANativeWindow_release(win);
        return true;
    }

    void process(capture::Job& j) {
        const uint32_t w = (uint32_t)(j.cr - j.cl), h = (uint32_t)(j.cb - j.ct);
        if (w < 16 || h < 16) return;
        if (w != ringW_ || h != ringH_ || effMaxGen() > ringMaxGen_) {
            if (!buildRings(w, h)) { capture::disable(); return; }
            lastTs_ = 0;
        }
        reapFrames();                        // вернуть gen-слоты завершённых submit'ов до планирования
        FrameRes& fr = fr_[frIdx_];
        waitFrame(fr);
        if ((++purge_tick_ & 63u) == 0) purgeStale();

        AhbImage tmp;
        AhbImage* src = nullptr;
        if (!getSource(j, fr, &src, &tmp)) { import_fail_++; if (import_fail_ % 120 == 1) LOGW("import of game buffer failed (usage/format?) x%llu", (unsigned long long)import_fail_); return; }

        if (src->external && !plain_.image) { import_fail_++; return; }
        if (isDuplicate(j, src)) { dup_skipped_++; return; }         // повтор: интервал считается между уникальными кадрами
        Slot* real = takeFree(real_, kReal);
        if (!real) {                         // нет свободного слота: этот кадр оверлею не достанется
            dropped_real_++;
            hide_req_.store(true);           // не держать поверх старый кадр: покажется живой слой игры
            return;
        }

        // --- план генерации -----------------------------------------------------------
        const int64_t ts = j.ts_ns;
        const int64_t dt = lastTs_ ? ts - lastTs_ : 0;
        // ---- фаза от меток источника -------------------------------------------------------
        // Схема привязки блока вывода:
        //   1) шаг D — интервал по меткам источника (PTS видео ровный 41.(6) мс при 24 fps,
        //      тогда как прибытия дрожат и приходят пачками);
        //   2) якорь base = pts + λ, где λ — EMA скольжения (прибытие − метка): поглощает сдвиг
        //      опорных часов (медиа-часы с другой базой) и усредняет дрожание латентности захвата;
        //   3) сброс λ при скачке > 60 мс (seek в видео, смена источника): фильтр не тянет разрыв.
        // Якорь зажат сверху моментом now: ошибка λ не может отложить кадры в будущее —
        // худший случай вырождается в якорь = момент прибытия.
        // lastPts_ имеет смысл считать интервалом, только если оно тоже получено из PTS:
        // иначе D смешивает опорные часы (медиа-время и CLOCK_MONOTONIC) и сдвигает якорь фазы.
        const bool havePts = g_cfg.pts_phase && j.pts_ns > 0 && lastPts_ > 0 && lastPtsValid_;
        if (!j.pts_ns) ptsFallback_++;
        if (lamSamples_ == 30)                      // раз в жизнь потока: чем реально anchors
            LOGI("pts-phase: anchor from source PTS (λ=%+.1f ms, resets=%d)%s", lamEma_ / 1.0e6, lamResets_,
                 ptsFallback_ ? " — some frames carry no PTS" : "");
        int64_t D = 0;
        if (havePts) {
            D = j.pts_ns - lastPts_;
            if (!(D > 1000000LL && D <= (int64_t)g_cfg.max_dt_ms * 2000000LL)) D = 0;   // не похож на интервал — откат
        }
        const int64_t dtPts = D ? D : dt;                       // шаг блока вывода
        // λ обновляется в точке вывода (после submit, см. «очередь вывода»): якорь должен
        // поглощать латентность всего конвейера, а не только захвата.
        int64_t base = now_ns();
        if (lamValid_ && j.pts_ns > 0 && g_cfg.pts_phase) {
            const int64_t anchored = j.pts_ns + (int64_t)lamEma_;
            if (anchored < base) base = anchored;                // λ-ошибка не откладывает кадры в будущее
        }
        if (j.pts_ns > 0) { lastPts_ = j.pts_ns; lastPtsValid_ = true; }
        else lastPtsValid_ = false;      // кадр без метки: следующему с PTS нельзя считать D от этого
        lastTs_ = ts;
        frames_++;
        if (frames_ == 1) LOGI("first game frame: %ux%u transform=%u dataspace=0x%x", w, h, (unsigned)j.transform, (unsigned)j.dataspace);
        const bool dt_ok = dtPts > 2000000LL && dtPts <= (int64_t)g_cfg.max_dt_ms * 1000000LL;
        if (dt_ok) {                         // сглаженный FPS источника (для min_fps и auto_rate)
            const double f = 1.0e9 / (double)dtPts;
            src_ema_ = src_ema_ > 0.0 ? src_ema_ + (f - src_ema_) * 0.1 : f;
            if (src_n_ < 1000000u) src_n_++;
        }
        const int64_t period = period_ns_.load();
        const float refresh = period > 1000000 ? 1.0e9f / (float)period : 60.0f;

        uint32_t planned = 0;
        bool wfg_ok = false;
        // --- SDR -> HDR: решаем ДО плана генерации: план ограничивается и кольцом out_, и кольцом hdr ---
        Slot* upGen[VKR_DIS_MAX_GENERATIONS] = {};
        Slot* upReal = nullptr;
        bool useUp = false;
        if (g_cfg.hdr) {
            if (hdrAllowed()) useUp = ensureHdr(w, h);
        } else if (nHdr_ > 0) {                      // HDR выключили на ходу: кольцо больше не держим
            clearQueue();
            vk_.f.DeviceWaitIdle(vk_.dev);
            freeHdr();
            LOGI("HDR off: output returns to SDR");
        }
        if (g_cfg.enable && wfg_) {
            const VkrDisContentRect content = {0, 0, w, h};
            vkr_dis_configure(wfg_, govFlow(), (uint32_t)g_cfg.target_fps, refresh);
            vkr_dis_set_fixed_generations(wfg_, g_cfg.multiplier >= 2 ? (uint32_t)(g_cfg.multiplier - 1) : 0u);
            if (vkr_dis_needs_rebuild(wfg_, w, h, VK_FORMAT_R8G8B8A8_UNORM, content)) vk_.f.DeviceWaitIdle(vk_.dev);
            wfg_ok = vkr_dis_prepare(wfg_, w, h, VK_FORMAT_R8G8B8A8_UNORM, content);
            if (wfg_ok) {
                uint32_t cap = std::min<uint32_t>({(uint32_t)govGenCap(), (uint32_t)VKR_DIS_MAX_GENERATIONS,
                                                   (uint32_t)freeCount(out_, nOut_)});
                if (useUp) {   // каждый выходной кадр занимает ещё слот hdr-кольца (план + real)
                    const int fh = freeCount(hdrS_, nHdr_) - 1;
                    cap = std::min<uint32_t>(cap, (uint32_t)std::max(0, fh));
                }
                if (!dt_ok) cap = 0;
                const bool slow = g_cfg.min_fps > 0 && src_n_ >= 8 && src_ema_ < (double)g_cfg.min_fps;
                if (slow) cap = 0;
                if (slow != slow_paused_) {
                    slow_paused_ = slow;
                    LOGI(slow ? "interpolation paused: game at %.1f fps < min_fps %d" : "interpolation resumed: game at %.1f fps", src_ema_, g_cfg.min_fps);
                }
                // Троттлинг батареи: выход не выше thermal_hz, т.е. не больше thermal_hz/fps
                // кадров на пару (включая реальный). Источник уже быстрее — генерации нет вовсе.
                if (therm_on_.load()) {
                    const double fps = src_n_ >= 8 ? src_ema_ : (dt > 0 ? 1.0e9 / (double)dt : 0.0);
                    if (fps > 1.0) {
                        const uint32_t allow = (uint32_t)((double)g_cfg.thermal_hz / fps);
                        cap = std::min<uint32_t>(cap, allow > 0 ? allow - 1u : 0u);
                    }
                }
                planned = vkr_dis_plan(wfg_, cap, frames_);
            }
        }
        Slot* gen[VKR_DIS_MAX_GENERATIONS] = {};
        uint32_t got = 0;
        for (uint32_t g = 0; g < planned; g++) { gen[g] = takeFree(out_, nOut_); if (!gen[g]) break; got++; }
        planned = got;

        if (useUp) {
            for (uint32_t g = 0; g < planned; g++) upGen[g] = takeFree(hdrS_, nHdr_);
            upReal = takeFree(hdrS_, nHdr_);
            if (!upReal) {
                // слота даже для real нет: кадр целиком уходит как SDR, чтобы не чередовать PQ и SDR
                for (uint32_t g = 0; g < planned; g++) if (upGen[g]) { upGen[g]->hold.store(0); upGen[g] = nullptr; }
                useUp = false;
            } else {
                // сгенерированный без hdr-слота не показываем: смешение PQ и SDR даёт мерцание яркости
                uint32_t m = 0;
                for (uint32_t g = 0; g < planned; g++) {
                    if (upGen[g]) { upGen[m] = upGen[g]; gen[m] = gen[g]; if (m != g) gen[g] = nullptr; m++; }
                    else gen[g]->hold.store(0);
                }
                planned = m;
            }
        }

        // ---- аппаратный ME Adreno (GL_QCOM_motion_estimation) ------------------------------------
        // Поле снаружи -> WFG (vkr_dis_supply_flow): пара получает вектора от аппаратного блока ME
        // (но не бесплатно: glFinish + readback + билинейный пересчёт поля на CPU каждый кадр),
        // DIS/Farneback остаётся фолбэком — нет расширения, ошибка GL, мелкое окно.
        // Цепочка luma-снимков идёт каждый кадр (даже без генерации), иначе пары разъезжаются.
        // Перед отдачей поле уточняется на GPU внутри ME-конвейера (me_refine: медиана
        // временного поля, gain-компенсация, защита центра), затем блочная редукция в сетку ME —
        // контракт с WFG не меняется.
        // Сам ME-блок выполняется ниже — между submit'ом A (blit) и записью буфера B: readback
        // нужен WFG-проходу, а не блиту. Здесь остаётся только решение о запуске — оно нужно
        // до записи, чтобы выбрать схему submit'ов.
        bool meActive = g_cfg.qcom_me && wfg_ok && !meFail_;
        // acquire-fence игры нужен обоим читателям буфера: blit'у (wait_sem, fd заберёт импорт в
        // submit A) и luma-снимку ME (свой EGL-wait) — дублируем заранее. Не продублировался —
        // кадр без ME: одним submit'ом, DIS.
        int meFence = meActive && j.fence_fd >= 0 ? dup(j.fence_fd) : -1;
        if (meActive && j.fence_fd >= 0 && meFence < 0) meActive = false;
        if (!meActive && !meFail_) qcomMe::reset();   // кадр мимо ME — цепочка рвётся

        // подсказка частоты слоя для auto_rate: только когда план держится ~30 кадров подряд
        if ((int)planned == plan_last_) plan_stable_++; else { plan_last_ = (int)planned; plan_stable_ = 0; }
        if (g_cfg.auto_rate && src_n_ >= 8 && plan_stable_ >= 30) {
            double r = src_ema_ * (double)(planned + 1);
            if (refresh > 1.0f && r > (double)refresh) r = (double)refresh;
            want_rate_.store((float)floor(r + 0.5));
        }

        // --- запись команд: буфер A (blit игрового кадра в кольцо) --------------------------------
        // meActive: буферов два — A (blit) уходит на GPU немедленно, B (WFG+HDR) записывается после ME.
        // Иначе это один буфер и один submit (запись и submit B продолжают использовать cb).
        VkCommandBuffer cbA = fr.cb;
        vk_.f.ResetCommandBuffer(cbA, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_.f.BeginCommandBuffer(cbA, &bi);
        if (fr.qp && !meActive) {
            vk_.f.CmdResetQueryPool(cbA, fr.qp, 0, 3);
            // TRANSFER-стадия: начало отсчёта после ожидания acquire-fence игры (иначе в «время GPU» попало бы её рендеринг)
            vk_.f.CmdWriteTimestamp(cbA, VK_PIPELINE_STAGE_TRANSFER_BIT, fr.qp, 0);
        }

        const uint32_t FOREIGN = VK_QUEUE_FAMILY_FOREIGN_EXT, Q = vk_.qfam;
        const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        const VkAccessFlags RW = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        const bool ext = src->external;
        if (ext) {
            barrier(cbA, src->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, FOREIGN, Q);
        } else {
            barrier(cbA, src->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FOREIGN, Q);
        }
        barrier(cbA, real->im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, FOREIGN, Q);

        if (ext) {
            // Внешний формат: читаем через YCbCr-сэмплер compute-шейдером в обычный RGBA8, затем копия в кольцо.
            const int32_t crop[4] = {j.cl, j.ct, j.cr, j.cb};
            barrier(cbA, plain_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            vk_.recordExtCopy(cbA, *src, plain_, crop, frIdx_);
            barrier(cbA, plain_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy cp{};
            cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {w, h, 1};
            vk_.f.CmdCopyImage(cbA, plain_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               real->im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        } else {
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.srcOffsets[0] = {j.cl, j.ct, 0};
            bl.srcOffsets[1] = {j.cr, j.cb, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.dstOffsets[0] = {0, 0, 0};
            bl.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
            vk_.f.CmdBlitImage(cbA, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               real->im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        }

        // --- submit A: blit уходит на GPU до ME ---------------------------------------------------
        // Пока CPU стоит в ME (glFinish + readback, ~1–2 мс), blit уже исполняется на GPU;
        // GL-команды ME тоже уходят сразу после A. Submit'ы одной очереди
        // упорядочены и для барьеров не граница («как один большой командный буфер», спецификация),
        // поэтому B не ждёт ничего, кроме барьера на real в его начале.
        if (meActive) {
            vk_.f.EndCommandBuffer(cbA);
            bool waitedA = false;
            if (j.fence_fd >= 0) {
                if (vk_.importSyncFd(fr.wait_sem, j.fence_fd)) { waitedA = true; j.fence_fd = -1; }
                else { struct pollfd p = {j.fence_fd, POLLIN, 0}; poll(&p, 1, 20); }   // CPU-ожидание, если импорт невозможен
            }
            const VkPipelineStageFlags wstA = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            VkSubmitInfo siA{};
            siA.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            siA.waitSemaphoreCount = waitedA ? 1 : 0;
            siA.pWaitSemaphores = &fr.wait_sem;
            siA.pWaitDstStageMask = &wstA;
            siA.commandBufferCount = 1;
            siA.pCommandBuffers = &cbA;
            if (vk_.f.QueueSubmit(vk_.queue, 1, &siA, VK_NULL_HANDLE) != VK_SUCCESS) {
                LOGE("vkQueueSubmit failed (blit)");
                if (meFence >= 0) close(meFence);
                real->hold.store(0);
                for (uint32_t g = 0; g < planned; g++) gen[g]->hold.store(0);
                for (uint32_t g = 0; g < planned; g++) if (upGen[g]) upGen[g]->hold.store(0);
                if (upReal) upReal->hold.store(0);
                return;
            }

            // ---- ME + supply: CPU стоит в glFinish/readback, GPU в это время делает blit ----
            if (!qcomMe::ensure()) meFail_ = true;
            else {
                const float* mv = nullptr;
                uint32_t mgw = 0, mgh = 0;
                if (qcomMe::frame(j.src, meFence, j.cl, j.ct, j.cr, j.cb, (int)j.bw, (int)j.bh, &mv, &mgw, &mgh))
                    // full_w/h — контент (вектора в его пикселях): масштаб и сетка считаются от него
                    vkr_dis_supply_flow(wfg_, mv, mgw, mgh, (uint32_t)(j.cr - j.cl), (uint32_t)(j.cb - j.ct));
            }
            if (meFence >= 0) close(meFence);
        }

        // --- запись команд: буфер B (WFG + HDR) — второй при meActive, иначе продолжение A ---------
        VkCommandBuffer cb = meActive ? fr.cb2 : cbA;
        if (meActive) {
            vk_.f.ResetCommandBuffer(cb, 0);
            VkCommandBufferBeginInfo bi2{};
            bi2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk_.f.BeginCommandBuffer(cb, &bi2);
            if (fr.qp) {
                vk_.f.CmdResetQueryPool(cb, fr.qp, 0, 3);
                // начало отсчёта здесь, а не в A: ME не должно попадать в «время GPU» governor'а
                vk_.f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, fr.qp, 0);
            }
        }

        // WFG ждёт исходник в GENERAL. При meActive барьер первый в B — и он же закрывает
        // зависимость от blit'а в A (srcAccess TRANSFER_WRITE барьера покрывает запись в A).
        barrier(cb, real->im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, RW, VK_PIPELINE_STAGE_TRANSFER_BIT, ALL);

        if (wfg_ok) {
            vkr_dis_process(wfg_, cb, real->im.image, w, h, planned);      // и при planned==0: копит историю
            for (uint32_t g = 0; g < planned; g++) {
                barrier(cb, gen[g]->im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        0, RW, ALL, ALL, FOREIGN, Q);
                vkr_dis_generate_into(wfg_, cb, g, (uint32_t)(gen[g] - out_), gen[g]->im.image, VK_NULL_HANDLE, w, h, real->im.image);
                if (upGen[g])        // кадр читает HDR-проход, SurfaceFlinger увидит уже перекодированную копию
                    barrier(cb, gen[g]->im.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            RW, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                else
                    barrier(cb, gen[g]->im.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            RW, 0, ALL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
            }
        }
        // отдаём real системе (SurfaceFlinger) и возвращаем исходник игре
        if (upReal)
            barrier(cb, real->im.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    RW, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        else
            barrier(cb, real->im.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    RW, 0, ALL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
        // конец «чистой» генерации: HDR-проходы ниже в governor не попадают
        if (fr.qp) vk_.f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fr.qp, 1);
        if (useUp) {
            const HdrParams hp = hdrParams();
            int n = 0;
            auto upPass = [&](Slot* dst, VkImageView sv) {
                if (hdrDirect_) {
                    // прямая запись в кольцо: один dispatch, старый layout — реальный (GENERAL), а не UNDEFINED
                    const int si = (int)(dst - hdrS_);
                    barrier(cb, dst->im.image, hdrInited_[si] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, FOREIGN, Q);
                    hdrInited_[si] = true;
                    hdr_.record(vk_, cb, frIdx_ * 8 + n++, sv, hdrV_[si], w, h, hp);
                    barrier(cb, dst->im.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_SHADER_WRITE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
                } else {
                    // запасной путь: kPlain промежуточных образов по кругу, проходы не ждут друг друга
                    PlainImage& ph = plainH_[n % kPlain];
                    const int pi = n % kPlain;
                    // образ остался в TRANSFER_SRC_OPTIMAL после копии предыдущего прохода
                    barrier(cb, ph.image, phInited_[pi] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                    phInited_[pi] = true;
                    hdr_.record(vk_, cb, frIdx_ * 8 + n++, sv, ph.view, w, h, hp);
                    barrier(cb, ph.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    barrier(cb, dst->im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT, FOREIGN, Q);
                    VkImageCopy cp{};
                    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    cp.extent = {w, h, 1};
                    vk_.f.CmdCopyImage(cb, ph.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       dst->im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
                    barrier(cb, dst->im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
                }
            };
            for (uint32_t g = 0; g < planned; g++) if (upGen[g]) upPass(upGen[g], outV_[gen[g] - out_]);
            if (upReal) upPass(upReal, realV_[real - real_]);
        }
        if (ext)
            barrier(cb, src->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
        else
            barrier(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Q, FOREIGN);
        if (fr.qp) vk_.f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fr.qp, 2);
        vk_.f.EndCommandBuffer(cb);

        // --- submit B (единственный при выключенном ME) --------------------------------------------
        bool waited_on_gpu = false;
        if (!meActive && j.fence_fd >= 0) {          // при meActive fence игры уже ждёт submit A
            if (vk_.importSyncFd(fr.wait_sem, j.fence_fd)) { waited_on_gpu = true; j.fence_fd = -1; }
            else { struct pollfd p = {j.fence_fd, POLLIN, 0}; poll(&p, 1, 20); }   // CPU-ожидание, если импорт невозможен
        }
        const VkPipelineStageFlags wst = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = waited_on_gpu ? 1 : 0;
        si.pWaitSemaphores = &fr.wait_sem;
        si.pWaitDstStageMask = &wst;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = vk_.can_export_syncfd ? 1 : 0;
        si.pSignalSemaphores = &fr.sig_sem;
        if (vk_.f.QueueSubmit(vk_.queue, 1, &si, fr.fence) != VK_SUCCESS) {
            LOGE("vkQueueSubmit failed");
            // при meActive submit A уже на GPU: отпускать слоты можно только после его завершения
            if (meActive) vk_.f.DeviceWaitIdle(vk_.dev);
            real->hold.store(0);
            for (uint32_t g = 0; g < planned; g++) gen[g]->hold.store(0);
            for (uint32_t g = 0; g < planned; g++) if (upGen[g]) upGen[g]->hold.store(0);
            if (upReal) upReal->hold.store(0);
            return;
        }
        fr.submitted = true;
        fr.timed = fr.qp != VK_NULL_HANDLE;
        fr.planned = planned;
        fr.fence_rel_gen = ringGen_;
        frIdx_ = (frIdx_ + 1) % kFrames;

        int fd = vk_.can_export_syncfd ? vk_.exportSyncFd(fr.sig_sem) : -1;
        if (fd < 0) {                                             // нет sync_fd: ждём на CPU
            // Кадр уходит в очередь без fence: SF скомпозитит буфер — он должен быть готов.
            // Сюда попадаем и при РАЗОВОМ сбое экспорта (can_export_syncd есть, но fd
            // не получился): недописанный буфер нельзя отдавать и в этом случае.
            // При таймауте дожидаемся конца исполнения, а не отдаём недописанный буфер.
            if (vk_.f.WaitForFences(vk_.dev, 1, &fr.fence, VK_TRUE, 100000000ull) == VK_TIMEOUT)
                vk_.f.DeviceWaitIdle(vk_.dev);
        }

        // --- очередь вывода -----------------------------------------------------------------
        // base/dtPts — из фазового блока выше: якорь от метки источника + λ-фильтр, шаг по PTS.
        const int64_t base_out = base;
        // P0-fix: λ считается в ТОЧКЕ ВЫВОДА (после submit), а не в точке захвата.
        // Раньше λ = ts - pts ≈ 0 у игр (метка = момент queueBuffer), и якорь base = pts
        // оказывался в прошлом на величину латентности конвейера (import/ME/GPU submit,
        // 5-20 мс). При 60 fps шаг блока 8.3 мс < латентности: цель gen-кадра (base)
        // постоянно в прошлом, «догон» (dropLate > 1.5 периода) выкидывал gen-кадры
        // вразнобой — итоговый FPS плавал вместо 120. Теперь λ поглощает всю латентность:
        // base = pts + λ ≈ момент готовности блока, цель gen-кадра снова в будущем.
        if (j.pts_ns > 0) {
            const int64_t lam = now_ns() - j.pts_ns;             // метка источника → готовность блока к показу
            if (!lamValid_) { lamEma_ = (double)lam; lamValid_ = true; }
            else if (llabs(lam - (int64_t)lamEma_) > 60000000LL) {   // скачок: seek/смена источника
                if (lamResets_ < 8) LOGD("pts-phase: clock jump %+lld ms, resetting", (long long)(lam - (int64_t)lamEma_) / 1000000);
                lamEma_ = (double)lam; lamResets_++;
            } else lamEma_ += (double)(lam - lamEma_) * 0.20;    // τ ≈ 5 кадров на 30 fps
            lamSamples_++;
        }
        const uint64_t rg = ringGen_.load();          // поколение кольца этих слотов
        for (uint32_t g = 0; g < planned; g++) {
            OutItem it;
            if (upGen[g]) {
                AHardwareBuffer_acquire(upGen[g]->im.ahb);
                it.ahb = upGen[g]->im.ahb; it.slot = upGen[g];
                // gen нужен только до конца submit (его читает HDR-проход того же submit):
                // отпустим по fence в waitFrame, а не holdTimed на 3/fps
                fr.fence_rel.push_back(gen[g]);
            } else {
                AHardwareBuffer_acquire(gen[g]->im.ahb);
                it.ahb = gen[g]->im.ahb; it.slot = gen[g];
            }
            it.fence_fd = fd >= 0 ? dup(fd) : -1;
            it.target_ns = base_out + (int64_t)((double)dtPts * g / (planned + 1));
            it.transform = j.transform; it.w = (int)w; it.h = (int)h;
            it.dataspace = j.dataspace; it.real = false; it.hdr = upGen[g] != nullptr;
            it.rgen = rg;
            push(it);
            gen_cnt_++;
        }
        OutItem it;
        if (upReal) {
            AHardwareBuffer_acquire(upReal->im.ahb);
            it.ahb = upReal->im.ahb; it.slot = upReal;
            holdTimed(real);
        } else {
            AHardwareBuffer_acquire(real->im.ahb);
            it.ahb = real->im.ahb; it.slot = real;
        }
        it.fence_fd = fd;                                          // последний — забирает оригинал
        it.target_ns = planned ? base_out + (int64_t)((double)dtPts * planned / (planned + 1)) : base_out;
        it.transform = j.transform; it.w = (int)w; it.h = (int)h;
        it.dataspace = j.dataspace; it.real = true; it.hdr = upReal != nullptr;
        it.rgen = rg;
        push(it);
        src_cnt_++;
    }

    void printStats() {
        if (!g_cfg.stats) return;
        const int64_t n = now_ns();
        if (n - stat_t_ < (g_cfg.debug ? 1000000000LL : 5000000000LL)) return;
        if (!g_cfg.enable) {                      // пауза: не засорять лог строкой "idle" и не копить интервал
            stat_t_ = n; stat_src_ = src_cnt_; stat_gen_ = gen_cnt_; stat_pres_ = presented_;
            stat_idle_ = true;
            return;
        }
        auto cs = capture::stats();
        const double s = (double)(n - stat_t_) / 1e9;
        const double src = (double)(src_cnt_ - stat_src_) / s, gen = (double)(gen_cnt_ - stat_gen_) / s, shown = (double)(presented_ - stat_pres_) / s;
        stat_t_ = n; stat_src_ = src_cnt_; stat_gen_ = gen_cnt_; stat_pres_ = presented_;
        if (src < 0.5 && shown < 0.5) {           // игра молчит (меню, загрузка): одна строка, дальше тишина
            if (!stat_idle_) { stat_idle_ = true; LOGI("idle: no frames from the game"); }
            return;
        }
        stat_idle_ = false;
        char ex[320]; size_t o = 0;
        auto add = [&](const char* k, unsigned long long v) { if (v && o < sizeof ex - 32) o += (size_t)snprintf(ex + o, sizeof ex - o, " %s=%llu", k, v); };
        add("dropLate", dropped_late_.load()); add("dropOut", dropped_out_); add("dropReal", dropped_real_);
        add("hidden", hidden_cnt_.load()); add("dup", dup_skipped_); add("importFail", import_fail_);
        add("fmtRej", cs.rejected_fmt); add("slotMiss", cs.slot_miss);
        if (g_cfg.debug) { add("queued", cs.queued); add("dropJob", cs.dropped_job); }
        // судьба поля аппаратного ME — me=used/dropped. Печатается всегда при включённом
        // qcom_me (даже 0/0 — само по себе диагноз: поле ни разу не дошло до генерации).
        if (g_cfg.qcom_me && wfg_) {
            uint64_t mu = 0, md = 0;
            vkr_dis_flow_stats(wfg_, &mu, &md);
            if (o < sizeof ex - 32)
                o += (size_t)snprintf(ex + o, sizeof ex - o, " me=%llu/%llu",
                                      (unsigned long long)mu, (unsigned long long)md);
        }
        char gv[64] = "";
        if (gov_level_ > 0) snprintf(gv, sizeof gv, " gov=%d", gov_level_);
        char upn[48] = "";
        if (g_cfg.hdr && nHdr_ > 0) snprintf(upn, sizeof upn, " hdr %s %.1fms%s", hdr_.tenBit() ? "10bit" : "8bit", hdr_ema_, hdrDirect_ ? "" : "+copy");
        char th[56] = "";
        if (g_cfg.thermal && therm_t_ != INT_MIN)
            snprintf(th, sizeof th, therm_on_.load() ? " battery %.1fC capped %dHz" : " battery %.1fC",
                     therm_t_ / 10.0, g_cfg.thermal_hz);
        LOGI("src=%.1f/s gen=%.1f/s shown=%.1f/s | gpu=%.1fms%s%s%s |%s", src, gen, shown, gpu_ema_, gv, upn, th, ex);
    }

    // =============================================================== данные
    VkCtx vk_;
    VkrDis* wfg_ = nullptr;
    Vsync vsync_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    FrameRes fr_[kFrames];
    int frIdx_ = 0;

    Slot real_[kReal];
    Slot out_[kOutMax];
    int nOut_ = 0;
    uint32_t ringW_ = 0, ringH_ = 0;
    PlainImage plain_;
    int last_w_ = 0, last_h_ = 0;

    std::unordered_map<uint64_t, AhbImage> srcCache_;
    uint64_t tick_ = 0;
    PFN_getId getId_ = nullptr;

    int64_t lastTs_ = 0;
    // ---- фаза от меток источника
    int64_t lastPts_ = 0;
    bool lastPtsValid_ = false;       // lastPts_ — действительно PTS источника (не момент прибытия)
    double lamEma_ = 0.0;             // EMA (готовность блока - pts): сдвиг опорных часов + латентность конвейера
    bool lamValid_ = false;
    int lamResets_ = 0, lamSamples_ = 0;
    uint64_t ptsFallback_ = 0;        // кадров без валидной метки (для отладочного лога)
    uint64_t frames_ = 0;
    std::atomic<int64_t> period_ns_{16666667};
    std::atomic<uint64_t> out_seq_{0};

    std::mutex oq_m_;
    OutItem oq_[kQ];
    int on_ = 0;

    std::mutex sc_m_;
    ASurfaceControl* sc_ = nullptr;
    ANativeWindow* win_ = nullptr;
    volatile int win_w_ = 0, win_h_ = 0;
    float applied_rate_ = 0.0f;                 // под sc_m_
    int applied_ds_ = -1;                       // под sc_m_
    std::atomic<float> want_rate_{0.0f};
    std::atomic<bool> visible_{false}, hide_req_{false};
    std::atomic<int64_t> last_apply_ns_{0};
    std::atomic<uint64_t> dropped_late_{0}, hidden_cnt_{0};
    double src_ema_ = 0.0;
    uint32_t src_n_ = 0, plan_stable_ = 0, purge_tick_ = 0;
    int plan_last_ = -1;
    bool slow_paused_ = false, stat_idle_ = false;
    PFN_setFrameTimeline setFrameTimeline_ = nullptr;
    PFN_setFrameRate setFrameRate_ = nullptr;
    PFN_winRate winRate_ = nullptr;

    std::atomic<uint64_t> sc_gen_{0};      // поколение слоя: устаревшие onComplete не трогают его
    ASurfaceControl* sc_grave_ = nullptr;       // прежний слой: освобождается при следующей замене
    Slot* shown_slot_ = nullptr;                // под sc_m_: слот, показанный последней транзакцией
    int64_t rate_ns_ = 0;                       // под sc_m_
    int64_t last_check_ = 0;
    int ringMaxGen_ = 0;
    std::atomic<bool> anchor_run_{false}, anchor_alive_{false};
    int sc_fail_cnt_ = 0;                     // подряд неудачных созданий слоя (лог не заливаем)
    std::mutex anchor_m_;
    bool enabled_ = true;                       // последнее наблюдённое значение enable (для логов пауза/возобновление)
    // детектор повторов
    VkImage sigImg_ = VK_NULL_HANDLE; VkDeviceMemory sigImgMem_ = VK_NULL_HANDLE;
    VkBuffer sigBuf_ = VK_NULL_HANDLE; VkDeviceMemory sigBufMem_ = VK_NULL_HANDLE;
    uint8_t* sigMap_ = nullptr;
    VkCommandBuffer sigCb_ = VK_NULL_HANDLE; VkFence sigFence_ = VK_NULL_HANDLE; VkSemaphore sigSem_ = VK_NULL_HANDLE;
    bool sigReady_ = false, sigFail_ = false, sigCoherent_ = true, prevValid_ = false;
    std::vector<uint8_t> prevSig_;
    uint64_t dup_skipped_ = 0;
    bool dupExtWarned_ = false;
    // адаптивная нагрузка
    double gpu_ema_ = 0.0;
    int gov_level_ = 0, gov_over_ = 0, gov_under_ = 0;
    int64_t gov_check_ns_ = 0, gov_change_ns_ = 0;
    // троттлинг по температуре батареи
    std::atomic<bool> therm_on_{false};
    int64_t therm_chk_ns_ = 0;
    int therm_t_ = INT_MIN;             // последний замер, 0.1°C
    uint32_t therm_over_ = 0;           // замеров подряд выше порога (антидребезг)
    bool therm_no_sensor_ = false;
    // SDR -> HDR
    HdrPass hdr_;
    // ---- аппаратный ME
    bool meFail_ = false;                 // qcom_me включён, но модуль не встал: без повторов
    // ---- пейсинг с обратной связью (см. paceOnComplete)
    static constexpr int64_t kPaceGuardStepNs = 250000;   // ступень надбавки: 250 мкс
    static constexpr int64_t kPaceGuardMaxNs  = 2500000;  // потолок надбавки: 2.5 мс
    double pace_ema_ = 0.0;               // EMA ошибки показа в слотах vsync (callback-поток)
    double pace_bias_ = 0.0;              // медленный EMA постоянной задержки композита, нс
    int pace_calm_ = 0;                   // подряд идущих «спокойных» замеров
    int64_t pace_change_ns_ = 0;          // последний шаг guard (антидребезг)
    int64_t pace_last_log_ns_ = 0;
    uint64_t pace_n_ = 0;
    std::atomic<int64_t> pace_guard_ns_{0};   // читает vsync-поток при выборе таймлайна
    bool hdrInit_ = false, hdrFail_ = false;
    Slot hdrS_[kHdrMax];
    PlainImage plainH_[kPlain];             // запасной путь: промежуточные storage-образы по кругу
    VkImageView realV_[kReal] = {};
    VkImageView outV_[kOutMax] = {};
    VkImageView hdrV_[kHdrMax] = {};        // вид для прямой записи шейдером в hdr-кольцо
    bool hdrInited_[kHdrMax] = {};          // layout слота уже переведён UNDEFINED -> GENERAL
    bool phInited_[kPlain] = {};
    bool hdrDirect_ = false;                // true — шейдер пишет прямо в кольцо (без CmdCopyImage)
    int nHdr_ = 0;
    uint32_t hdrW_ = 0, hdrH_ = 0;
    double hdr_ema_ = 0.0;                  // GPU-время HDR-проходов (в governor не попадает)
    std::atomic<uint64_t> ringGen_{0};           // поколение кольца out_: слоты переиспользуются при пересоздании
                                                // (читает и callback-поток onComplete — см. CbCtx)
    int hdrState_ = 0;                          // 0 не проверяли, 1 панель с HDR, 2 без HDR, 4 запрос не удался, работаем вслепую
    int hdrTries_ = 0, panelPeak_ = 0;
    int64_t hdrTryNs_ = 0;

    uint64_t src_cnt_ = 0, gen_cnt_ = 0, presented_ = 0, dropped_out_ = 0, dropped_real_ = 0, import_fail_ = 0;
    uint64_t stat_src_ = 0, stat_gen_ = 0, stat_pres_ = 0;
    int64_t stat_t_ = 0;
};

Pipeline g_pipeline;

}  // namespace

namespace pipeline {
void start() { g_pipeline.start(); }
}
