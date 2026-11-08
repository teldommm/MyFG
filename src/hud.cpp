#include "hud.h"
#include "common.h"
#include "config.h"
#include "capture.h"

#include <android/hardware_buffer.h>
#include <pthread.h>
#include <android/surface_control.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace hud {
namespace {

// ---- шрифт 5x7: только нужные символы (цифры, "FPS GAME GEN GPU MS", точка, '>', '+', '-') ---------
struct Glyph { char c; uint8_t r[7]; };
const Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'>', {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
};
const uint8_t* glyph(char c) {
    for (const Glyph& g : kFont) if (g.c == c) return g.r;
    return nullptr;                                   // пробел и неизвестные символы — пусто
}

constexpr int kMaxCols = 11;                          // самая длинная строка: "GPU 100.0MS"
constexpr int kMaxLines = 4;
constexpr int kBufs = 3;                              // SurfaceFlinger может ещё читать предыдущий буфер
constexpr uint32_t kBg = 0x99000000u;                 // чёрный, alpha 0.6 (premultiplied RGBA8, little-endian)
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kGreen = 0xFF8DD902u;              // #02D98D

struct Line { std::string text; uint32_t color; };

struct State {
    std::mutex m;                                     // защищает запуск/выход потока (ensure против завершения loop)
    std::atomic<bool> alive{false}, run{false}, failed{false};
    Source src{};
};
State g_s;

int textWidth(const std::string& t, int s) { return t.empty() ? 0 : (int)t.size() * 6 * s - s; }

// Закрашивает прямоугольник, обрезая его по границам буфера bufW x bufH: длинная строка (например
// "GPU 100.0MS" при перегрузе) не должна выходить за буфер и писать в чужую память.
void fillRect(uint32_t* px, int stride, int bufW, int bufH, int x, int y, int w, int h, uint32_t c) {
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(bufW, x + w), y1 = std::min(bufH, y + h);
    if (x1 <= x0 || y1 <= y0) return;
    for (int j = y0; j < y1; j++) { uint32_t* r = px + (size_t)j * stride; for (int i = x0; i < x1; i++) r[i] = c; }
}

// Рисует строки в буфер bufW x bufH; фон и текст прижаты к углу, заданному right/bottom.
void render(uint32_t* px, int stride, int bufW, int bufH, int s, const std::vector<Line>& lines, bool right, bool bottom) {
    for (int y = 0; y < bufH; y++) memset(px + (size_t)y * stride, 0, (size_t)bufW * 4);
    if (lines.empty()) return;
    const int pad = 2 * s;
    int tw = 0;
    for (auto& l : lines) tw = std::max(tw, textWidth(l.text, s));
    const int bw = tw + 2 * pad, bh = (int)lines.size() * 9 * s - 2 * s + 2 * pad;
    // блок больше буфера (длинный текст): прижимаем к краю и обрезаем, а не отказываемся рисовать
    const int ox = std::max(0, right ? bufW - bw : 0), oy = std::max(0, bottom ? bufH - bh : 0);
    fillRect(px, stride, bufW, bufH, ox, oy, bw, bh, kBg);
    int y0 = oy + pad;
    for (auto& l : lines) {
        // выравнивание строк по тому же краю, что и сам блок
        int x0 = ox + pad + (right ? tw - textWidth(l.text, s) : 0);
        for (char ch : l.text) {
            if (const uint8_t* g = glyph(ch)) {
                for (int row = 0; row < 7; row++)
                    for (int col = 0; col < 5; col++)
                        if (g[row] & (0x10 >> col)) fillRect(px, stride, bufW, bufH, x0 + col * s, y0 + row * s, s, s, l.color);
            }
            x0 += 6 * s;
        }
        y0 += 9 * s;
    }
}

std::vector<Line> compose(int mode, double src, double gen, double shown, double gpu_ms) {
    char b[48];
    std::vector<Line> v;
    const int sh = (int)(shown + 0.5), sr = (int)(src + 0.5), ge = (int)(gen + 0.5);
    if (mode == 0) { snprintf(b, sizeof b, "FPS %d", sh); v.push_back({b, kWhite}); }
    else if (mode == 1) { snprintf(b, sizeof b, "%d>%d", sr, sh); v.push_back({b, kWhite}); }
    else {
        snprintf(b, sizeof b, "FPS %d", sh);  v.push_back({b, kWhite});
        snprintf(b, sizeof b, "GAME %d", sr); v.push_back({b, kWhite});
        snprintf(b, sizeof b, "GEN +%d", ge); v.push_back({b, kGreen});
        if (gpu_ms > 0.05) { snprintf(b, sizeof b, "GPU %.1fMS", gpu_ms); v.push_back({b, kWhite}); }
    }
    return v;
}

void sleepMs(int ms) { usleep((useconds_t)ms * 1000); }

void hideAndRelease(ASurfaceControl*& a) {
    if (!a) return;
    ASurfaceTransaction* t = ASurfaceTransaction_create();
    ASurfaceTransaction_setVisibility(t, a, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    ASurfaceTransaction_apply(t);
    ASurfaceTransaction_delete(t);
    ASurfaceControl_release(a);
    a = nullptr;
}

void loop() {
    name_thread("myfg-hud");
    AHardwareBuffer* bufs[kBufs] = {};
    int bufW = 0, bufH = 0, bufScale = 0;
    ASurfaceControl* a = nullptr;
    ANativeWindow* win = nullptr;
    int64_t chk = 0;
    int idx = 0;
    uint64_t p_src = 0, p_gen = 0, p_shown = 0;
    int64_t p_ns = 0;
    double e_src = 0, e_gen = 0, e_shown = 0;
    bool have = false;
    LOGD("hud thread started");

    auto freeBufs = [&] { for (auto& b : bufs) if (b) { AHardwareBuffer_release(b); b = nullptr; } };

    while (g_s.run.load()) {
        const int64_t now = now_ns();
        // --- слой следует за окном игры (раз в секунду) ---
        if (!a || now - chk > 1000000000LL) {
            chk = now;
            ANativeWindow* cur = g_s.src.acquire_window ? g_s.src.acquire_window(g_s.src.ctx) : nullptr;
            if (cur != win) {
                hideAndRelease(a);
                if (win) ANativeWindow_release(win);
                win = cur;
                if (win) {
                    a = ASurfaceControl_createFromWindow(win, "myfg_hud");
                    if (a) LOGI("hud layer created on window %p (%dx%d)", win, ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
                    else LOGW("hud: ASurfaceControl_createFromWindow failed (%dx%d)", ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
                }
            } else if (cur) ANativeWindow_release(cur);
        }
        if (!a) { sleepMs(300); continue; }

        // --- частоты за прошедший интервал, сглаживание ---
        uint64_t c_src = 0, c_gen = 0, c_shown = 0; double gpu = 0;
        if (g_s.src.counters) g_s.src.counters(g_s.src.ctx, &c_src, &c_gen, &c_shown, &gpu);
        if (have && now - p_ns > 100000000LL) {
            const double dt = (double)(now - p_ns) / 1e9;
            const double r_src = (double)(c_src - p_src) / dt, r_gen = (double)(c_gen - p_gen) / dt, r_shown = (double)(c_shown - p_shown) / dt;
            e_src += (r_src - e_src) * 0.6; e_gen += (r_gen - e_gen) * 0.6; e_shown += (r_shown - e_shown) * 0.6;
            if (r_shown < 0.5 && r_src < 0.5) { e_src = e_gen = e_shown = 0; }       // игра молчит: сразу нули
        }
        if (!have || now - p_ns > 100000000LL) { p_src = c_src; p_gen = c_gen; p_shown = c_shown; p_ns = now; have = true; }

        // --- буферы под текущий масштаб ---
        const int s = std::max(1, std::min(8, g_cfg.hud_scale));
        if (s != bufScale || !bufs[0]) {
            freeBufs();
            bufScale = s;
            bufW = kMaxCols * 6 * s - s + 4 * s;
            bufH = kMaxLines * 9 * s - 2 * s + 4 * s;
            AHardwareBuffer_Desc d{};
            d.width = (uint32_t)bufW; d.height = (uint32_t)bufH; d.layers = 1; d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
            bool ok = true;
            for (auto& b : bufs) if (AHardwareBuffer_allocate(&d, &b) != 0 || !b) { ok = false; break; }
            if (!ok) { LOGW("hud: buffer %dx%d could not be allocated, hud disabled", bufW, bufH); freeBufs(); g_s.failed.store(true); break; }
        }

        // --- рисуем в следующий по кругу буфер и показываем ---
        const int pos = g_cfg.hud_pos;                          // 0 ЛВ, 1 ПВ, 2 ЛН, 3 ПН
        const bool right = pos == 1 || pos == 3, bottom = pos == 2 || pos == 3;
        AHardwareBuffer* b = bufs[idx++ % kBufs];
        void* p = nullptr;
        if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p) == 0 && p) {
            AHardwareBuffer_Desc dd{};
            AHardwareBuffer_describe(b, &dd);
            render((uint32_t*)p, (int)dd.stride, bufW, bufH, s, compose(g_cfg.hud_mode, e_src, e_gen, e_shown, gpu), right, bottom);
            AHardwareBuffer_unlock(b, nullptr);

            const int ww = ANativeWindow_getWidth(win), wh = ANativeWindow_getHeight(win);
            const int tf = g_cfg.hud_tf;
            const int dw = (tf & 4) ? bufH : bufW, dh = (tf & 4) ? bufW : bufH;
            const int m = std::max(8, std::min(ww, wh) / 50);
            const int x = right ? std::max(0, ww - dw - m) : m, y = bottom ? std::max(0, wh - dh - m) : m;
            ARect srcR = {0, 0, bufW, bufH}, dst = {x, y, x + dw, y + dh};
            ASurfaceTransaction* t = ASurfaceTransaction_create();
            ASurfaceTransaction_setBuffer(t, a, b, -1);
            ASurfaceTransaction_setVisibility(t, a, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
            ASurfaceTransaction_setBufferTransparency(t, a, ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
            ASurfaceTransaction_setZOrder(t, a, capture::electedSc() ? 0x3fffffff : g_cfg.zorder + 1);   // выше слоя оверлея (в SC-режиме — выше всех слоёв приложения)
            ASurfaceTransaction_setGeometry(t, a, srcR, dst, tf);
            ASurfaceTransaction_apply(t);
            ASurfaceTransaction_delete(t);
        }
        for (int i = 0; i < 10 && g_s.run.load(); i++) sleepMs(50);   // остановка (hud=0, генерация выключена) срабатывает сразу
    }
    hideAndRelease(a);
    if (win) ANativeWindow_release(win);
    freeBufs();
    LOGD("hud thread stopped");
}

// Поток: крутит loop(); выходит, только если за время завершения никто не попросил запуск снова.
void entry() {
    for (;;) {
        loop();
        std::lock_guard<std::mutex> g(g_s.m);
        if (!g_s.run.load() || g_s.failed.load()) { g_s.alive.store(false); return; }
    }
}

}  // namespace

void ensure(const Source& s) {
    if (g_s.failed.load()) return;
    std::lock_guard<std::mutex> g(g_s.m);
    g_s.run.store(true);
    if (g_s.alive.load()) return;       // поток уже работает (или как раз проверяет run перед выходом)
    g_s.src = s;
    g_s.alive.store(true);
    std::thread(entry).detach();
}

void stop() { g_s.run.store(false); }

}  // namespace hud
