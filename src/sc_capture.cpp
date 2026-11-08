// Захват слоёв SurfaceControl.
//
// Зачем. Chrome (и браузеры на Chromium) показывают страницу и видео не через BufferQueue процесса браузера:
// их GPU-процесс (`пакет:privileged_process*`) собирает кадр сам и отдаёт слою через
// ASurfaceTransaction_setBuffer. В процессе браузера этих кадров нет вовсе, поэтому модуль загружается и в GPU-процесс
// (main.cpp) и ловит там вызовы libandroid.
//
// Модель.
//  * Слой-источник — слой приложения, которому оно отдаёт буферы. Выбирается теми же правилами, что и очередь игры
//    (>= 6 кадров/с, больший размер), см. capture.cpp / elect().
//  * Оверлей создаётся СОСЕДОМ источника: тем же родителем (ASurfaceControl_create у родителя либо
//    createFromWindow на том же окне). Геометрия (src/dst/transform), прозрачность, альфа и видимость повторяют
//    источник, поэтому оверлей встаёт в точности туда, где приложение показывает свои кадры.
//  * Порядок слоёв. Среди соседей слои с равным z упорядочены по порядку создания, поэтому «z источника + 1» ничего не
//    гарантирует: у приложения может быть слой ровно с z+1, и оверлей встал бы поверх него (скрыл бы интерфейс,
//    который приложение рисует над источником). Поэтому z, который приложение задаёт СВОИМ слоям, умножается на 2
//    (порядок слоёв при этом не меняется), а оверлей получает 2*z+1: строго над источником, строго под любым слоем,
//    который приложение поставило выше. Это возможно, только если хук стоял с первого слоя: иначе у уже созданных
//    слоёв z остался бы «сырым», и порядок смешался бы. Поэтому режим фиксируется по первому событию процесса:
//    первое событие — создание слоя => множитель включён; первое событие — вызов для слоя, которого мы не видели,
//    => хуки встали поздно, захват слоёв отключён до перезапуска процесса, z приложения не трогается.
//  * Ссылки на слои приложения мы НЕ берём: слой живёт, пока приложение держит его, и «призрак» от лишней ссылки
//    остался бы на экране после его release.
#include "capture.h"
#include "capture_internal.h"
#include "common.h"
#include "config.h"
#include "util.h"

#include <android/data_space.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <android/surface_control.h>
#include <atomic>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr int kMaxLayers = 128;

struct Layer {
    bool used = false;
    ASurfaceControl* sc = nullptr;          // ключ (ссылки не держим)
    ASurfaceControl* parent = nullptr;      // родитель-слой (ключ), nullptr — слой создан на окне
    ANativeWindow* window = nullptr;        // окно корня дерева (со ссылкой), если известно
    bool own = false;                       // наш слой (myfg_*)
    bool known = false;                     // создан после установки хуков: родитель/окно известны, можно ставить соседа
    bool z_set = false;
    int32_t z = 0;                          // z, как его задало приложение (до умножения)
    bool geom = false;
    ARect src{}, dst{};
    int32_t tf = 0;
    bool hidden = false;
    float alpha = 1.0f;
    int transp = -1;
    int32_t ds = 0;
    int64_t touched = 0;
};

Layer g_l[kMaxLayers];
std::mutex g_m;

std::atomic<bool> g_armed{false};           // все нужные хуки стоят
std::atomic<bool> g_gave_up{false};         // установка окончательно отказана
std::atomic<bool> g_late{false};            // хуки встали после создания слоёв приложения: захват слоёв выключен
bool g_first_done = false;                  // под g_m
std::atomic<bool> g_zmode{false};           // z приложения умножается на 2
std::atomic<bool> g_invalid{false};
std::atomic<uint64_t> g_buffers{0};
std::atomic<uint64_t> g_calls_logged{0};
// Родитель, у которого прямо сейчас создаётся наш слой: его release приложением придерживается (ASurfaceControl_acquire
// есть только с API 31, а модуль рассчитан на 29).
std::atomic<const void*> g_pin{nullptr};

// ---- оригиналы -----------------------------------------------------------------------------
typedef ASurfaceControl* (*PFN_createFromWindow)(ANativeWindow*, const char*);
typedef ASurfaceControl* (*PFN_create)(ASurfaceControl*, const char*);
typedef void (*PFN_release)(ASurfaceControl*);
typedef void (*PFN_setBuffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int);
typedef void (*PFN_setGeometry)(ASurfaceTransaction*, ASurfaceControl*, const ARect&, const ARect&, int32_t);
typedef void (*PFN_setZOrder)(ASurfaceTransaction*, ASurfaceControl*, int32_t);
typedef void (*PFN_setVisibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
typedef void (*PFN_setAlpha)(ASurfaceTransaction*, ASurfaceControl*, float);
typedef void (*PFN_setTransparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
typedef void (*PFN_setDataSpace)(ASurfaceTransaction*, ASurfaceControl*, ADataSpace);
typedef void (*PFN_reparent)(ASurfaceTransaction*, ASurfaceControl*, ASurfaceControl*);

PFN_createFromWindow o_cfw = nullptr;
PFN_create o_create = nullptr;
PFN_release o_release = nullptr;
PFN_setBuffer o_setBuffer = nullptr;
PFN_setGeometry o_setGeometry = nullptr;
PFN_setZOrder o_setZ = nullptr;
PFN_setVisibility o_setVis = nullptr;
PFN_setAlpha o_setAlpha = nullptr;
PFN_setTransparency o_setTransp = nullptr;
PFN_setDataSpace o_setDs = nullptr;
PFN_reparent o_reparent = nullptr;

// ---- таблица слоёв (все функции — под g_m) ----------------------------------------------------
Layer* find_locked(const void* sc) {
    for (auto& l : g_l)
        if (l.used && l.sc == sc) return &l;
    return nullptr;
}

void free_locked(Layer* l) {
    if (l->window) ANativeWindow_release(l->window);
    *l = Layer();
}

Layer* add_locked(ASurfaceControl* sc) {
    Layer* e = nullptr;
    Layer* oldest = &g_l[0];
    for (auto& l : g_l) {
        if (!l.used) { e = &l; break; }
        if (l.touched < oldest->touched) oldest = &l;
    }
    if (!e) {                                   // таблица полна: вытесняем самый давний (приложение держит их единицы)
        e = oldest;
        free_locked(e);
    }
    *e = Layer();
    e->used = true;
    e->sc = sc;
    e->touched = now_ns();
    return e;
}

// Первое событие процесса решает, возможно ли умножение z (см. шапку). Под g_m. false — захват слоёв выключен.
bool first_event_locked(bool is_create) {
    if (g_first_done) return !g_late.load();
    g_first_done = true;
    if (is_create) {
        g_zmode = true;
        LOGI("surface-control capture: hooks armed before the app's first layer (z ordering is exact)");
    } else {
        g_late = true;
        LOGW("surface-control capture is off: hooks were installed after the app created its layers "
             "(z order could not be kept exact). Restart the browser to enable it");
    }
    return !g_late.load();
}

// Слой для события не-создания. Незнакомый слой — признак того, что его создали до хуков.
Layer* touch_locked(ASurfaceControl* sc) {
    Layer* l = find_locked(sc);
    if (l && l->own) { l->touched = now_ns(); return l; }      // наш слой: на решение о «позднем» старте не влияет
    if (!first_event_locked(false)) return nullptr;
    if (!l) l = add_locked(sc);                 // known=false: z запоминаем, источником такой слой быть не может
    l->touched = now_ns();
    return l;
}

bool is_elected_locked(const void* sc) { return capture::detail::electedKey() == sc && capture::electedSc(); }

int32_t clamp_z(int64_t v) {
    if (v > 0x3fffffff) v = 0x3fffffff;
    if (v < -0x3fffffff) v = -0x3fffffff;
    return (int32_t)v;
}

bool rect_eq(const ARect& a, const ARect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void on_create(ASurfaceControl* sc, ASurfaceControl* parent, ANativeWindow* window, const char* name) {
    if (!sc) return;
    const bool own = name && strncmp(name, "myfg_", 5) == 0;
    std::lock_guard<std::mutex> g(g_m);
    if (!own && !first_event_locked(true)) return;
    Layer* old = find_locked(sc);              // адрес мог быть переиспользован после release
    if (old) free_locked(old);
    Layer* l = add_locked(sc);
    l->own = own;
    l->known = !own;
    l->parent = parent;
    if (!own) {
        ANativeWindow* w = window;
        if (!w && parent) {
            Layer* p = find_locked(parent);
            if (p) w = p->window;
        }
        if (w) { ANativeWindow_acquire(w); l->window = w; }
        if (g_calls_logged++ < 12 || g_cfg.debug)
            LOGD("layer created %p '%s' parent=%p window=%p", (void*)sc, name ? name : "", (void*)parent, (void*)l->window);
    }
}

// ---- хуки -----------------------------------------------------------------------------------
ASurfaceControl* hook_createFromWindow(ANativeWindow* w, const char* name) {
    ASurfaceControl* sc = o_cfw(w, name);
    if (g_armed.load() && !g_late.load()) on_create(sc, nullptr, w, name);
    return sc;
}

ASurfaceControl* hook_create(ASurfaceControl* parent, const char* name) {
    ASurfaceControl* sc = o_create(parent, name);
    if (g_armed.load() && !g_late.load()) on_create(sc, parent, nullptr, name);
    return sc;
}

void hook_release(ASurfaceControl* sc) {
    if (g_armed.load() && !g_late.load()) {
        std::lock_guard<std::mutex> g(g_m);
        if (Layer* l = find_locked(sc)) {
            if (capture::detail::dropElected(sc)) {
                g_invalid = true;
                LOGI("surface-control layer %p released by the app: source dropped", (void*)sc);
            }
            free_locked(l);
        }
    }
    for (int i = 0; i < 500 && g_pin.load() == (const void*)sc; i++) usleep(100);   // мы как раз создаём слой у этого родителя
    o_release(sc);
}

void hook_setBuffer(ASurfaceTransaction* t, ASurfaceControl* sc, AHardwareBuffer* b, int fence) {
    if (!g_armed.load() || g_late.load() || !b) { o_setBuffer(t, sc, b, fence); return; }
    bool eligible = false, geom = false;
    ARect src{};
    int32_t tf = 0, ds = 0;
    {
        std::lock_guard<std::mutex> g(g_m);
        Layer* l = touch_locked(sc);
        if (l && !l->own && l->known) {
            eligible = true;
            geom = l->geom; src = l->src; tf = l->tf; ds = l->ds;
        }
    }
    if (!eligible) { o_setBuffer(t, sc, b, fence); return; }
    g_buffers++;

    capture::Job job;
    bool want = false;
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(b, &d);
    if (d.width >= 16 && d.width <= 16384 && d.height >= 16 && d.height <= 16384) {
        if (!capture::detail::formatOk(d.format)) {
            capture::detail::countRejectedFmt();
        } else if (d.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) {
            capture::detail::countRejectedProt();          // DRM: наш контекст его не прочитает, а оверлей закрыл бы видео
        } else {
            int cl = 0, ct = 0, cr = (int)d.width, cb = (int)d.height;
            if (geom && src.left >= 0 && src.top >= 0 && src.right > src.left && src.bottom > src.top &&
                src.right <= (int)d.width && src.bottom <= (int)d.height) {
                cl = src.left; ct = src.top; cr = src.right; cb = src.bottom;
            }
            const int rate_n = capture::detail::rateTouch(sc);
            if (capture::detail::scElect(sc, cr - cl, cb - ct, rate_n)) {
                job.cl = cl; job.ct = ct; job.cr = cr; job.cb = cb;
                job.bw = (int)d.width; job.bh = (int)d.height;
                job.ts_ns = now_ns();
                job.pts_ns = 0;
                job.transform = (uint32_t)tf;
                job.dataspace = (ds >= 0 && ds < 0x40000000) ? ds : 0;
                // fence отдаётся транзакции (она его закроет): копию берём ДО оригинала
                job.fence_fd = (g_cfg.acq_fence && fence >= 0) ? fcntl(fence, F_DUPFD_CLOEXEC, 0) : -1;
                want = true;
            }
        }
    }
    o_setBuffer(t, sc, b, fence);
    if (want) {
        AHardwareBuffer_acquire(b);
        job.src = b;
        capture::detail::countQueued();
        capture::detail::scSubmit(job);
    }
}

void hook_setGeometry(ASurfaceTransaction* t, ASurfaceControl* sc, const ARect& src, const ARect& dst, int32_t tf) {
    o_setGeometry(t, sc, src, dst, tf);
    if (!g_armed.load() || g_late.load()) return;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = touch_locked(sc);
    if (!l || l->own) return;
    const bool changed = !(l->geom && rect_eq(l->src, src) && rect_eq(l->dst, dst) && l->tf == tf);
    l->geom = true; l->src = src; l->dst = dst; l->tf = tf;
    if (changed && is_elected_locked(sc)) g_invalid = true;     // показанный оверлей лежит по старой геометрии
}

void hook_setZOrder(ASurfaceTransaction* t, ASurfaceControl* sc, int32_t z) {
    if (!g_armed.load() || g_late.load()) { o_setZ(t, sc, z); return; }
    bool own = false, mul = false;
    {
        std::lock_guard<std::mutex> g(g_m);
        // Первое событие процесса решает, возможно ли умножение: touch_locked вызывается и тогда, когда множитель ещё
        // не включён (иначе вызов setZOrder для слоя, созданного до хуков, не распознался бы как поздний старт).
        Layer* l = touch_locked(sc);
        if (!l) { o_setZ(t, sc, z); return; }
        own = l->own;
        if (!own) { l->z_set = true; l->z = z; }
        mul = !own && g_zmode.load();
    }
    o_setZ(t, sc, mul ? clamp_z((int64_t)z * 2) : z);
}

void hook_setVisibility(ASurfaceTransaction* t, ASurfaceControl* sc, int8_t v) {
    o_setVis(t, sc, v);
    if (!g_armed.load() || g_late.load()) return;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = touch_locked(sc);
    if (!l || l->own) return;
    const bool hidden = v == ASURFACE_TRANSACTION_VISIBILITY_HIDE;
    if (hidden != l->hidden) {
        l->hidden = hidden;
        if (is_elected_locked(sc)) g_invalid = true;
    }
}

void hook_setBufferAlpha(ASurfaceTransaction* t, ASurfaceControl* sc, float a) {
    o_setAlpha(t, sc, a);
    if (!g_armed.load() || g_late.load()) return;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = touch_locked(sc);
    if (l && !l->own) l->alpha = a;
}

void hook_setBufferTransparency(ASurfaceTransaction* t, ASurfaceControl* sc, int8_t v) {
    o_setTransp(t, sc, v);
    if (!g_armed.load() || g_late.load()) return;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = touch_locked(sc);
    if (l && !l->own) l->transp = v;
}

void hook_setBufferDataSpace(ASurfaceTransaction* t, ASurfaceControl* sc, ADataSpace ds) {
    o_setDs(t, sc, ds);
    if (!g_armed.load() || g_late.load()) return;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = touch_locked(sc);
    if (l && !l->own) l->ds = (int32_t)ds;
}

void hook_reparent(ASurfaceTransaction* t, ASurfaceControl* sc, ASurfaceControl* np) {
    o_reparent(t, sc, np);
    if (!g_armed.load() || g_late.load()) return;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = touch_locked(sc);
    if (!l || l->own || l->parent == np) return;
    l->parent = np;
    Layer* p = np ? find_locked(np) : nullptr;
    if (l->window) { ANativeWindow_release(l->window); l->window = nullptr; }
    if (p && p->window) { ANativeWindow_acquire(p->window); l->window = p->window; }
    if (!np || !p) l->known = false;               // слой отцеплен или ушёл под родителя, которого мы не знаем: не источник
    LOGD("layer %p reparented to %p", (void*)sc, (void*)np);
    if (is_elected_locked(sc)) {
        g_invalid = true;
        if (!l->known) capture::detail::dropElected(sc);
    }
}

struct HookDef {
    const char* name;
    void* hook;
    void** orig;
    bool required;
    bool done;
};

}  // namespace

namespace capture {

uint64_t scBuffers() { return g_buffers.load(); }
bool scArmed() { return g_armed.load() && !g_late.load(); }

bool installSc() {
    static HookDef defs[] = {
        {"ASurfaceControl_createFromWindow", (void*)hook_createFromWindow, (void**)&o_cfw, true, false},
        {"ASurfaceControl_create", (void*)hook_create, (void**)&o_create, true, false},
        {"ASurfaceControl_release", (void*)hook_release, (void**)&o_release, true, false},
        {"ASurfaceTransaction_setBuffer", (void*)hook_setBuffer, (void**)&o_setBuffer, true, false},
        {"ASurfaceTransaction_setGeometry", (void*)hook_setGeometry, (void**)&o_setGeometry, true, false},
        {"ASurfaceTransaction_setZOrder", (void*)hook_setZOrder, (void**)&o_setZ, true, false},
        {"ASurfaceTransaction_setVisibility", (void*)hook_setVisibility, (void**)&o_setVis, false, false},
        {"ASurfaceTransaction_setBufferAlpha", (void*)hook_setBufferAlpha, (void**)&o_setAlpha, false, false},
        {"ASurfaceTransaction_setBufferTransparency", (void*)hook_setBufferTransparency, (void**)&o_setTransp, false, false},
        {"ASurfaceTransaction_setBufferDataSpace", (void*)hook_setBufferDataSpace, (void**)&o_setDs, false, false},
        {"ASurfaceTransaction_reparent", (void*)hook_reparent, (void**)&o_reparent, false, false},
    };
    const bool want = g_cfg.hook_sc == 1 || (g_cfg.hook_sc < 0 && myfg::auxProcess());
    if (!want || g_gave_up.load()) return true;
    if (g_armed.load()) return true;

    static bool loaded = false;
    if (!loaded) { loaded = true; dlopen("libandroid.so", RTLD_NOW); }     // нужна в карте процесса для разбора ELF
    bool missing = false;
    for (auto& d : defs) {
        if (d.done) continue;
        size_t sz = 0;
        void* p = resolve_symbol_ex("libandroid.so", d.name, false, &sz);
        if (!p) {
            if (d.required) missing = true;
            else d.done = true;                        // необязательная точка входа отсутствует: идём без неё
            continue;
        }
        if (inline_hook(d.name, p, sz, false, d.hook, d.orig)) {
            d.done = true;
        } else if (d.required) {
            LOGW("surface-control capture is off: hook %s was refused", d.name);
            g_gave_up = true;                          // установка окончательно отказана; поставленные хуки — проброс
            return true;
        } else {
            d.done = true;
        }
    }
    if (missing) return false;                         // libandroid ещё не загружена/символ не найден — повторим
    g_armed = true;
    LOGI("surface-control hooks installed (layers of this process are tracked)");
    return true;
}

bool scLive(ScLive* out) {
    const void* key = detail::electedKey();
    if (!key || !electedSc() || !g_armed.load() || g_late.load()) return false;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = find_locked(key);
    if (!l || !l->known || l->own) return false;
    out->visible = !l->hidden;
    out->z = g_zmode.load() ? clamp_z((int64_t)l->z * 2 + 1) : l->z;
    out->geom = l->geom;
    out->dl = l->dst.left; out->dt = l->dst.top; out->dr = l->dst.right; out->db = l->dst.bottom;
    out->transform = l->tf;
    out->alpha = l->alpha;
    out->transparency = l->transp;
    return true;
}

uint64_t scParentKey() {
    const void* key = detail::electedKey();
    if (!key || !electedSc()) return 0;
    std::lock_guard<std::mutex> g(g_m);
    Layer* l = find_locked(key);
    if (!l || !l->known) return 0;
    return l->parent ? (uint64_t)(uintptr_t)l->parent : (uint64_t)(uintptr_t)l->window;
}

ASurfaceControl* scCreateOverlay(const char* name, uint64_t* parentKey, ANativeWindow** rootWindow) {
    if (!g_armed.load() || g_late.load() || !g_zmode.load()) return nullptr;
    const void* key = detail::electedKey();
    if (!key || !electedSc()) return nullptr;
    ASurfaceControl* parent = nullptr;
    ANativeWindow* win = nullptr;       // окно, на котором создан источник (если у него нет родителя-слоя)
    ANativeWindow* root = nullptr;
    uint64_t pk = 0;
    {
        std::lock_guard<std::mutex> g(g_m);
        Layer* l = find_locked(key);
        if (!l || !l->known) return nullptr;
        if (l->parent) {
            Layer* p = find_locked(l->parent);
            if (!p) return nullptr;     // родителя мы не видели: жив ли он — неизвестно, ссылку на него не берём
            parent = l->parent;
            g_pin.store(parent);                    // release родителя будет придержан, пока слой создаётся
            pk = (uint64_t)(uintptr_t)l->parent;
        } else if (l->window) {
            ANativeWindow_acquire(l->window);
            win = l->window;
            pk = (uint64_t)(uintptr_t)l->window;
        } else {
            return nullptr;
        }
        if (l->window) { ANativeWindow_acquire(l->window); root = l->window; }
    }
    // Вызов идёт через наши же хуки (имя myfg_* помечает слой как свой); под g_m его делать нельзя.
    ASurfaceControl* sc = parent ? ASurfaceControl_create(parent, name) : ASurfaceControl_createFromWindow(win, name);
    g_pin.store(nullptr);
    if (win) ANativeWindow_release(win);
    if (!sc) {
        if (root) ANativeWindow_release(root);
        return nullptr;
    }
    *parentKey = pk;
    if (rootWindow) *rootWindow = root;
    else if (root) ANativeWindow_release(root);
    LOGI("overlay layer created next to the source layer %p (%s %p)", key, parent ? "parent layer" : "window", parent ? (void*)parent : (void*)win);
    return sc;
}

bool scTakeInvalidated() { return g_invalid.exchange(false); }

}  // namespace capture
