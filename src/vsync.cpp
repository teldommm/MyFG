#include "vsync.h"
#include "common.h"
#include <android/choreographer.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <thread>
#include <atomic>

// Функции API 33 берём через dlsym (minSdk модуля = 29), типы — из заголовка NDK.
struct Vsync::Impl {
    typedef void (*PostVsync)(AChoreographer*, AChoreographer_vsyncCallback, void*);
    typedef int64_t (*GetNs)(const AChoreographerFrameCallbackData*);
    typedef size_t (*GetLen)(const AChoreographerFrameCallbackData*);
    typedef int64_t (*GetIdx)(const AChoreographerFrameCallbackData*, size_t);

    PostVsync postVsync = nullptr;
    GetNs     frameTime = nullptr;
    GetLen    len = nullptr;
    GetIdx    vsyncId = nullptr, expected = nullptr, deadline = nullptr;
    Vsync*    owner = nullptr;
    AChoreographer* ch = nullptr;
    int64_t   last_frame = 0;
    double    period = 16666667.0;
    volatile bool posted = false;       // колбэк сейчас стоит в Choreographer (трогает только поток vsync)

    // ---- фазовая сетка. Трогает только поток vsync,
    // наружу отдаётся через атомики для nextVsyncAfter().
    int64_t   next = 0;                 // прогноз следующего vsync (0 = сетка не заведена)
    static constexpr int kPhaseDiv = 8; // доля фазовой ошибки, поглощаемая за тик (1/8)
    std::atomic<int64_t> grid_origin{0};    // последняя опорная точка сетки (frame_ns тика)
    std::atomic<int64_t> grid_period{16666667};

    void updatePeriod(int64_t ft) {
        if (last_frame) {
            double d = (double)(ft - last_frame);
            if (d > 2e6 && d < 50e6) period = period * 0.9 + d * 0.1;
        }
        last_frame = ft;
    }

    // Обновление сетки по реальному тику. Возвращает фазовую ошибку и факт ре-лока.
    void updateGrid(int64_t ft, VsyncInfo& vi) {
        int64_t p = (int64_t)period;
        if (next == 0) {                       // сетка ещё не заведена (старт / выход из паузы)
            next = ft + p;
            vi.phase_ns = 0; vi.snapped = true;
        } else {
            int64_t phase = ft - next;         // signed: real - прогноз
            vi.phase_ns = phase;
            if (phase > Vsync::kSnapNs || phase < -Vsync::kSnapNs) {
                // Пропуск тиков, джиттер или смена частоты: жёсткий снап к реальному
                // времени, дрейф не копится (donor: last = now).
                next = ft + p;
                vi.snapped = true;
                LOGD("vsync: re-lock, phase %.2f ms", phase / 1e6);
            } else {
                // Плавное следование: сдвигаемся на период и поглощаем 1/8 ошибки.
                next += p + phase / kPhaseDiv;
                vi.snapped = false;
            }
        }
        vi.next_ns = next;
        grid_origin.store(ft, std::memory_order_relaxed);
        grid_period.store(p, std::memory_order_relaxed);
    }

    void fill(VsyncInfo& vi) {
        vi.period_ns = (int64_t)period;
    }

    static void onVsync(const AChoreographerFrameCallbackData* d, void* u) {
        Impl* s = (Impl*)u;
        VsyncInfo vi;
        vi.frame_ns = s->frameTime(d);
        s->updatePeriod(vi.frame_ns);
        s->fill(vi);
        s->updateGrid(vi.frame_ns, vi);
        size_t n = s->len(d);
        if (n > (size_t)VsyncInfo::kMax) n = VsyncInfo::kMax;
        vi.n = (int)n;
        vi.has_timeline = n > 0;
        for (size_t i = 0; i < n; i++) {
            vi.id[i] = s->vsyncId(d, i);
            vi.present[i] = s->expected(d, i);
            vi.deadline[i] = s->deadline(d, i);
        }
        if (s->owner->cb_) s->owner->cb_(vi, s->owner->user_);
        if (!s->owner->stop_ && !s->owner->paused_) s->postVsync(s->ch, onVsync, s);
        else s->posted = false;
    }

    static void onFrame64(int64_t frameTimeNanos, void* u) {
        Impl* s = (Impl*)u;
        VsyncInfo vi;
        vi.frame_ns = frameTimeNanos;
        s->updatePeriod(frameTimeNanos);
        s->fill(vi);
        s->updateGrid(frameTimeNanos, vi);
        vi.n = 0; vi.has_timeline = false;
        if (s->owner->cb_) s->owner->cb_(vi, s->owner->user_);
        if (!s->owner->stop_ && !s->owner->paused_) AChoreographer_postFrameCallback64(s->ch, onFrame64, s);
        else s->posted = false;
    }
};

int64_t Vsync::nextVsyncAfter(int64_t now_ns) const {
    int64_t origin = impl_ ? impl_->grid_origin.load(std::memory_order_relaxed) : 0;
    int64_t p      = impl_ ? impl_->grid_period.load(std::memory_order_relaxed) : 16666667;
    if (!origin || p <= 0) return now_ns + p / 2;   // сетка не набрана — грубая оценка
    if (now_ns <= origin) return origin + p;
    // Сколько целых периодов помещается между опорной точкой и now: сетка не сдвигается.
    int64_t k = (now_ns - origin + p - 1) / p;      // ceil
    return origin + k * p;
}

bool Vsync::start(Callback cb, void* user) {
    if (running_) return true;
    cb_ = cb; user_ = user; stop_ = false;
    impl_ = new Impl();
    impl_->owner = this;
    void* h = dlopen("libandroid.so", RTLD_NOW);
    if (h) {
        impl_->postVsync = (Impl::PostVsync)dlsym(h, "AChoreographer_postVsyncCallback");
        impl_->frameTime = (Impl::GetNs)dlsym(h, "AChoreographerFrameCallbackData_getFrameTimeNanos");
        impl_->len       = (Impl::GetLen)dlsym(h, "AChoreographerFrameCallbackData_getFrameTimelinesLength");
        impl_->vsyncId   = (Impl::GetIdx)dlsym(h, "AChoreographerFrameCallbackData_getFrameTimelineVsyncId");
        impl_->expected  = (Impl::GetIdx)dlsym(h, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
        impl_->deadline  = (Impl::GetIdx)dlsym(h, "AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos");
    }
    running_ = true;
    std::thread([this] { run(); }).detach();
    return true;
}

void Vsync::stop() { stop_ = true; }

void Vsync::run() {
    pthread_setname_np(pthread_self(), "myfg-vsync");
    ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    impl_->ch = AChoreographer_getInstance();
    if (!impl_->ch) { LOGE("vsync: no choreographer"); running_ = false; return; }
    bool tl = impl_->postVsync && impl_->frameTime && impl_->len && impl_->vsyncId && impl_->expected && impl_->deadline;
    LOGD("vsync: %s", tl ? "frame timelines (API 33+)" : "frameTime only (fallback)");
    auto post = [&] {
        impl_->posted = true;
        impl_->last_frame = 0;                 // после паузы первый интервал не считаем периодом
        impl_->next = 0;                       // ...и сетку заводим заново (ре-лок на первом тике)
        if (tl) impl_->postVsync(impl_->ch, Impl::onVsync, impl_);
        else    AChoreographer_postFrameCallback64(impl_->ch, Impl::onFrame64, impl_);
    };
    post();
    while (!stop_) {
        ALooper_pollOnce(100, nullptr, nullptr, nullptr);
        if (!paused_ && !impl_->posted) post();      // снята пауза
    }
    running_ = false;
}
