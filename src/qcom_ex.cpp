#include "qcom_ex.h"
#include "common.h"
#include "gl_island.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// Контракт и схема — в qcom_ex.h. Ориентация: проход копирования сквозной БЕЗ переворота (строка 0 буфера
// игры == строка 0 текстуры == строка 0 слота), поэтому blit и предсказание идут в координатах кадра.

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

namespace {

typedef void (*PFN_extrapolate)(GLuint, GLuint, GLuint, GLfloat);
typedef void (*PFN_genQ)(GLsizei, GLuint*);
typedef void (*PFN_delQ)(GLsizei, const GLuint*);
typedef void (*PFN_beginQ)(GLenum, GLuint);
typedef void (*PFN_endQ)(GLenum);
typedef void (*PFN_getQ)(GLuint, GLenum, GLuint*);
typedef void (*PFN_getQ64)(GLuint, GLenum, uint64_t*);

constexpr int kExtCache = 8;       // импорты буферов игры (LRU): кольцо игры бывает 3–5 штук
constexpr int kMaxTargets = 48;    // слоты колец real (6) + gen (до 24) с запасом
constexpr int kQueries = 4;        // таймер-запросы в полёте
constexpr uint32_t kSigW = 64, kSigH = 36;
constexpr int kTestSize = 256;     // сторона синтетики самотеста

struct ExtTex {                    // буфер игры как GL_TEXTURE_EXTERNAL_OES
    AHardwareBuffer* ahb = nullptr;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;
    uint64_t use = 0;
};

struct Target {                    // слот кольца как GL_TEXTURE_2D + FBO (запись/blit/предсказание)
    AHardwareBuffer* ahb = nullptr;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
};

struct Ex {
    bool inited = false, ready = false, reasonLogged = false;
    PFN_extrapolate extrapolate = nullptr;

    GLuint prog = 0;
    GLint uRect = -1, uSize = -1, uTex = -1;

    // приватные кадры цепочки: P[cur] пишется, P[1-cur] — предыдущий
    GLuint P[2] = {0, 0}, fboP[2] = {0, 0};
    int cur = 0, pw = 0, ph = 0;
    bool prevValid = false;
    // приватный выход предсказания (если драйвер не пишет прямо в EGLImage-текстуру)
    GLuint X = 0, fboX = 0;
    bool direct = false;

    // сигнатура для детектора повторов
    GLuint dupTex = 0, fboDup = 0;
    std::vector<uint8_t> sig, prevSig;
    bool prevSigValid = false;

    ExtTex ext[kExtCache];
    uint64_t tick = 0;
    std::vector<Target> targets;

    bool begun = false;            // begin() прошёл, ждём finish()
    int w = 0, h = 0;              // размер кадра текущей пары begin/finish

    // GPU-таймер
    PFN_genQ genQ = nullptr; PFN_delQ delQ = nullptr; PFN_beginQ beginQ = nullptr; PFN_endQ endQ = nullptr;
    PFN_getQ getQ = nullptr; PFN_getQ64 getQ64 = nullptr;
    GLuint q[kQueries] = {0, 0, 0, 0};
    bool qPending[kQueries] = {false, false, false, false};
    int qNext = 0;
    bool qActive = false;
    int qActiveIdx = -1;

    // ошибки времени исполнения: подряд — и всё, возвращаемся на WFG
    int errRun = 0, errLogged = 0;
    int64_t t0 = 0;                // начало CPU-замера кадра (begin)
    double cpuAccMs = 0;           // накопление begin+finish текущего кадра

    // статистика
    uint64_t stFrames = 0, stExtra = 0, stGpuN = 0;
    double stCpuSum = 0, stCpuMax = 0, stGpuSum = 0, stGpuMax = 0;
    double gpuEma = 0;
} g;

void die(const char* why) {
    if (!g.reasonLogged) LOGW("qcom-ex: %s (WFG stays the frame generator)", why);
    g.reasonLogged = true;
    g.ready = false;
}

void drainGlErrors() { for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {} }

GLuint compile(GLenum kind, const char* src, bool quiet = false) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (!quiet) {
            char log[512] = {};
            glGetShaderInfoLog(s, sizeof log - 1, nullptr, log);
            LOGW("qcom-ex: shader compile failed: %s", log);
        }
        glDeleteShader(s);
        return 0;
    }
    return s;
}

const char* kVert =
    "#version 300 es\n"
    "const vec2 p[3]=vec2[3](vec2(-1.,-1.),vec2(3.,-1.),vec2(-1.,3.));\n"
    "void main(){gl_Position=vec4(p[gl_VertexID],0.,1.);}\n";
const char* kFragHead =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n";
const char* kFragHeadOes =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n";
const char* kFragBody =
    "uniform samplerExternalOES uTex;\n"
    "uniform vec4 uRect;uniform vec2 uSize;\n"
    "out vec4 o;\n"
    "void main(){\n"
    // строка r кадра = строка r текстуры (без переворота); gl_FragCoord.xy — центр пикселя
    "  vec2 uv=(uRect.xy+gl_FragCoord.xy)/uSize;\n"
    "  o=vec4(texture(uTex,uv).rgb,1.);\n"
    "}\n";

// ---- импорт буфера игры -----------------------------------------------------------------------
void dropExt(ExtTex& e) {
    if (e.tex) { glDeleteTextures(1, &e.tex); e.tex = 0; }
    if (e.img != EGL_NO_IMAGE_KHR) glIsland::fns().DestroyImageKHR(glIsland::display(), e.img);
    e.img = EGL_NO_IMAGE_KHR;
    if (e.ahb) { AHardwareBuffer_release(e.ahb); e.ahb = nullptr; }
}

ExtTex* importExt(AHardwareBuffer* ahb) {
    int slot = -1;
    uint64_t minUse = ~0ull;
    for (int i = 0; i < kExtCache; i++) {
        ExtTex& e = g.ext[i];
        if (e.ahb == ahb && e.img != EGL_NO_IMAGE_KHR) { e.use = ++g.tick; return &e; }
        if (!e.ahb) { slot = i; break; }
        if (e.use < minUse) { minUse = e.use; slot = i; }
    }
    if (slot < 0) return nullptr;
    dropExt(g.ext[slot]);
    ExtTex& e = g.ext[slot];
    AHardwareBuffer_acquire(ahb);
    e.ahb = ahb;
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    e.img = glIsland::fns().CreateImageKHR(glIsland::display(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                           (EGLClientBuffer)ahb, attrs);
    if (e.img == EGL_NO_IMAGE_KHR) { AHardwareBuffer_release(ahb); e.ahb = nullptr; return nullptr; }
    glGenTextures(1, &e.tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, e.tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glIsland::fns().ImageTargetTex(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)e.img);
    e.use = ++g.tick;
    return &e;
}

// ---- слоты колец как GL-цели ----------------------------------------------------------------------
void destroyTarget(Target& t) {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    if (t.img != EGL_NO_IMAGE_KHR) glIsland::fns().DestroyImageKHR(glIsland::display(), t.img);
    if (t.ahb) AHardwareBuffer_release(t.ahb);
    t = Target();
}

// Создаёт обёртку AHB (EGLImage -> GL_TEXTURE_2D -> FBO). nullptr — драйвер не принял буфер как цель.
Target* makeTarget(AHardwareBuffer* ahb) {
    Target t;
    AHardwareBuffer_acquire(ahb);
    t.ahb = ahb;
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    t.img = glIsland::fns().CreateImageKHR(glIsland::display(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                           (EGLClientBuffer)ahb, attrs);
    if (t.img == EGL_NO_IMAGE_KHR) { destroyTarget(t); return nullptr; }
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glIsland::fns().ImageTargetTex(GL_TEXTURE_2D, (GLeglImageOES)t.img);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
    if (!ok) { drainGlErrors(); destroyTarget(t); return nullptr; }
    g.targets.push_back(t);
    return &g.targets.back();
}

Target* getTarget(AHardwareBuffer* ahb) {
    for (auto& t : g.targets) if (t.ahb == ahb) return &t;
    if ((int)g.targets.size() >= kMaxTargets) return nullptr;
    g.targets.reserve(kMaxTargets);                 // указатели на элементы не должны переезжать
    return makeTarget(ahb);
}

// ---- приватные текстуры -------------------------------------------------------------------------------
GLuint makeTex(int w, int h) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

GLuint makeFbo(GLuint tex) {
    GLuint f = 0;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { glDeleteFramebuffers(1, &f); return 0; }
    return f;
}

void freeChain() {
    if (g.fboP[0]) glDeleteFramebuffers(2, g.fboP);
    if (g.P[0]) glDeleteTextures(2, g.P);
    if (g.fboX) glDeleteFramebuffers(1, &g.fboX);
    if (g.X) glDeleteTextures(1, &g.X);
    g.fboP[0] = g.fboP[1] = 0; g.P[0] = g.P[1] = 0; g.fboX = 0; g.X = 0;
    g.pw = g.ph = 0;
    g.prevValid = false;
    g.cur = 0;
}

bool ensureChain(int w, int h) {
    if (g.pw == w && g.ph == h && g.P[0]) return true;
    freeChain();
    for (int i = 0; i < 2; i++) {
        g.P[i] = makeTex(w, h);
        g.fboP[i] = makeFbo(g.P[i]);
        if (!g.fboP[i]) { freeChain(); return false; }
    }
    if (!g.direct) {
        g.X = makeTex(w, h);
        g.fboX = makeFbo(g.X);
        if (!g.fboX) { freeChain(); return false; }
    }
    if (glGetError() != GL_NO_ERROR) { drainGlErrors(); freeChain(); return false; }
    g.pw = w; g.ph = h;
    LOGD("qcom-ex: chain %dx%d (%s output)", w, h, g.direct ? "direct" : "copied");
    return true;
}

// Проход «буфер игры (external) -> P[cur]». Под активным состоянием острова; viewport = размер кадра.
void drawCopy(ExtTex* e, int cl, int ct, int cr, int cb, int bw, int bh, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboP[g.cur]);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST); glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, e->tex);
    glUniform1i(g.uTex, 0);
    glUniform4f(g.uRect, (float)cl, (float)ct, (float)cr, (float)cb);
    glUniform2f(g.uSize, (float)bw, (float)bh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    (void)cr; (void)cb;
}

// ---- GPU-таймер ----------------------------------------------------------------------------------------
void timerCollect() {
    if (!g.getQ) return;
    for (int i = 0; i < kQueries; i++) {
        if (!g.qPending[i]) continue;
        GLuint avail = 0;
        g.getQ(g.q[i], GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
        if (!avail) continue;
        uint64_t ns = 0;
        g.getQ64(g.q[i], GL_QUERY_RESULT_EXT, &ns);
        g.qPending[i] = false;
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        if (disjoint) continue;                        // замер недостоверен (смена частоты и т.п.)
        const double ms = (double)ns / 1.0e6;
        if (ms <= 0.0 || ms > 1000.0) continue;
        g.stGpuN++; g.stGpuSum += ms; g.stGpuMax = std::max(g.stGpuMax, ms);
        g.gpuEma = g.gpuEma > 0.0 ? g.gpuEma + (ms - g.gpuEma) * 0.15 : ms;
    }
}
void timerBegin() {
    if (!g.beginQ) return;
    if (g.qActive) { g.endQ(GL_TIME_ELAPSED_EXT); g.qActive = false; g.qPending[g.qActiveIdx] = false; }  // прошлый кадр не дошёл до finish
    timerCollect();
    for (int k = 0; k < kQueries; k++) {
        const int i = (g.qNext + k) % kQueries;
        if (g.qPending[i]) continue;
        g.qNext = (i + 1) % kQueries;
        g.beginQ(GL_TIME_ELAPSED_EXT, g.q[i]);
        g.qActive = true; g.qActiveIdx = i;
        return;
    }
}
void timerEnd() {
    if (!g.qActive) return;
    g.endQ(GL_TIME_ELAPSED_EXT);
    g.qActive = false;
    g.qPending[g.qActiveIdx] = true;
}
void timerDiscard() {
    if (!g.qActive) return;
    g.endQ(GL_TIME_ELAPSED_EXT);
    g.qActive = false;                                 // результат не нужен: pending не ставим
}

// ---- самотест --------------------------------------------------------------------------------------------
// Синтетика: гладкий шум ячейками 8 px (три канала), весь кадр сдвигается на 16 px между src1 и src2;
// предсказание на scale=1.0 должно сдвинуться ещё на 16 (всего 32 от src1). Мерим сдвиг перебором.
uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float cellVal(int cx, int cy, int ch) {
    return (float)(hash32((uint32_t)(cx * 73856093) ^ (uint32_t)(cy * 19349663) ^ (uint32_t)(ch * 83492791)) & 255);
}
// значение шума в точке (x,y) пространства содержимого (билинейно между ячейками 8 px)
void patternAt(float x, float y, uint8_t* rgb) {
    const float fx = x / 8.0f, fy = y / 8.0f;
    const int ix = (int)floorf(fx), iy = (int)floorf(fy);
    const float ax = fx - (float)ix, ay = fy - (float)iy;
    for (int c = 0; c < 3; c++) {
        const float v00 = cellVal(ix, iy, c), v10 = cellVal(ix + 1, iy, c);
        const float v01 = cellVal(ix, iy + 1, c), v11 = cellVal(ix + 1, iy + 1, c);
        const float v = (v00 * (1 - ax) + v10 * ax) * (1 - ay) + (v01 * (1 - ax) + v11 * ax) * ay;
        rgb[c] = (uint8_t)std::min(255.0f, std::max(0.0f, v));
    }
}
void fillPattern(std::vector<uint8_t>& px, float sx, float sy) {   // содержимое сдвинуто на (sx, sy) пикселей
    px.assign((size_t)kTestSize * kTestSize * 4, 255);
    for (int y = 0; y < kTestSize; y++)
        for (int x = 0; x < kTestSize; x++) {
            uint8_t rgb[3];
            patternAt((float)x - sx, (float)y - sy, rgb);
            uint8_t* o = &px[((size_t)y * kTestSize + x) * 4];
            o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2]; o[3] = 255;
        }
}
// Лучший сдвиг по оси (axisX: горизонталь) для кадра pix относительно паттерна: перебор s в [-64,64], SAD по центру.
int measureShift(const std::vector<uint8_t>& pix, bool axisX) {
    int best = 0;
    double bestSad = 1e30;
    for (int s = -64; s <= 64; s++) {
        double sad = 0;
        for (int y = 80; y < 176; y += 2)
            for (int x = 80; x < 176; x += 2) {
                uint8_t rgb[3];
                patternAt((float)x - (axisX ? (float)s : 0.0f), (float)y - (axisX ? 0.0f : (float)s), rgb);
                const uint8_t* o = &pix[((size_t)y * kTestSize + x) * 4];
                sad += abs((int)o[0] - rgb[0]) + abs((int)o[1] - rgb[1]) + abs((int)o[2] - rgb[2]);
            }
        if (sad < bestSad) { bestSad = sad; best = s; }
    }
    return best;
}
void uploadTex(GLuint tex, const std::vector<uint8_t>& px) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTestSize, kTestSize, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
}
bool readTex(GLuint fbo, std::vector<uint8_t>& out) {
    out.assign((size_t)kTestSize * kTestSize * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kTestSize, kTestSize, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    return glGetError() == GL_NO_ERROR;
}

struct TestResult { bool ran = false; int dx = 0, dy = 0; bool gl = true; };

// Один прогон: src1/src2 — приватные, выход — outTex/outFbo. axisX: сдвиг по x, иначе по y.
bool testRun(GLuint a, GLuint b, GLuint outTex, GLuint outFbo, bool axisX, int* shift) {
    std::vector<uint8_t> px;
    fillPattern(px, 0.0f, 0.0f);
    uploadTex(a, px);
    fillPattern(px, axisX ? 16.0f : 0.0f, axisX ? 0.0f : 16.0f);
    uploadTex(b, px);
    drainGlErrors();
    g.extrapolate(a, b, outTex, 1.0f);
    if (glGetError() != GL_NO_ERROR) return false;
    if (!readTex(outFbo, px)) return false;
    *shift = measureShift(px, axisX);
    return true;
}

// Возвращает true, если режим пригоден; direct выставляется по результату пробы прямой записи в EGLImage.
bool selfTest() {
    GLuint a = makeTex(kTestSize, kTestSize), b = makeTex(kTestSize, kTestSize), c = makeTex(kTestSize, kTestSize);
    GLuint fc = makeFbo(c);
    if (!fc) {
        glDeleteTextures(1, &a); glDeleteTextures(1, &b); glDeleteTextures(1, &c);
        LOGW("qcom-ex: self-test: RGBA8 render target unavailable");
        return false;
    }
    int dx = 0, dy = 0;
    const bool rx = testRun(a, b, c, fc, true, &dx);
    const bool ry = rx && testRun(a, b, c, fc, false, &dy);
    bool good = rx && ry;
    // ожидание: 32 от src1 (16 уже между кадрами + 16 предсказания). Сдвиг ровно 16 — это копия src2, т.е.
    // «предсказания нет»; считаем пригодным, если предсказание продвинулось хотя бы на ~6 px за src2 (>= 22).
    const bool okX = rx && dx >= 22 && dx <= 46;
    const bool okY = ry && dy >= 22 && dy <= 46;
    if (!rx || !ry) LOGW("qcom-ex: self-test: glExtrapolateTex2DQCOM raised a GL error (private RGBA8 textures)");
    else LOGI("qcom-ex: self-test: predicted shift dx=%+d dy=%+d px (ideal +32/+32)%s", dx, dy, (okX && okY) ? "" : " — NOT plausible");
    good = good && okX && okY;
    const bool force = g_cfg.qcom_ex == 2;

    // проба прямого вывода в слот кольца (EGLImage-текстура AHB, как у реальных слотов)
    g.direct = false;
    if (rx && ry) {
        AHardwareBuffer_Desc d = {};
        d.width = kTestSize; d.height = kTestSize; d.layers = 1; d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        AHardwareBuffer* ahb = nullptr;
        if (AHardwareBuffer_allocate(&d, &ahb) != 0) {
            d.usage &= ~(uint64_t)AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
            if (AHardwareBuffer_allocate(&d, &ahb) != 0) ahb = nullptr;
        }
        if (ahb) {
            Target* t = makeTarget(ahb);
            if (t) {
                int sx = 0;
                if (testRun(a, b, t->tex, t->fbo, true, &sx) && abs(sx - dx) <= 2) {
                    g.direct = true;
                } else {
                    drainGlErrors();
                    LOGI("qcom-ex: direct output into ring buffers rejected (shift %+d vs %+d), using a private texture + copy", sx, dx);
                }
                destroyTarget(*t);
                g.targets.pop_back();
            } else {
                LOGI("qcom-ex: ring buffer is not usable as a GL target, using a private texture + copy");
            }
            AHardwareBuffer_release(ahb);
        }
    }
    glDeleteFramebuffers(1, &fc);
    glDeleteTextures(1, &a); glDeleteTextures(1, &b); glDeleteTextures(1, &c);
    drainGlErrors();
    if (!good && force) { LOGW("qcom-ex: self-test not passed, qcom_ex=2 forces the mode on"); return true; }
    return good;
}

}  // namespace

namespace qcomEx {

bool available() { return g.ready; }
double gpuEmaMs() { return g.gpuEma; }

bool ensure() {
    if (g.inited) return g.ready;
    g.inited = true;
    if (!glIsland::ensure()) return (die("GL island unavailable (see gl-island reason above)"), false);
    drainGlErrors();
    if (!glIsland::hasExt("GL_QCOM_frame_extrapolation")) return (die("GL_QCOM_frame_extrapolation not advertised"), false);
    g.extrapolate = (PFN_extrapolate)eglGetProcAddress("glExtrapolateTex2DQCOM");
    if (!g.extrapolate) g.extrapolate = (PFN_extrapolate)dlsym(RTLD_DEFAULT, "glExtrapolateTex2DQCOM");
    if (!g.extrapolate) return (die("glExtrapolateTex2DQCOM not resolved"), false);

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    std::string fsSrc = std::string(kFragHead) + kFragBody;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc.c_str(), true);
    if (!fs) {
        fsSrc = std::string(kFragHeadOes) + kFragBody;
        fs = compile(GL_FRAGMENT_SHADER, fsSrc.c_str());
    }
    if (!vs || !fs) return (die("copy shader broken"), false);
    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glLinkProgram(g.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &linked);
    if (!linked) return (die("copy program link failed"), false);
    g.uRect = glGetUniformLocation(g.prog, "uRect");
    g.uSize = glGetUniformLocation(g.prog, "uSize");
    g.uTex = glGetUniformLocation(g.prog, "uTex");

    // детектор повторов: сигнатура 64x36 через blit с LINEAR
    g.dupTex = makeTex((int)kSigW, (int)kSigH);
    g.fboDup = makeFbo(g.dupTex);
    if (!g.fboDup) return (die("signature target unavailable"), false);
    g.sig.assign((size_t)kSigW * kSigH * 4, 0);
    g.prevSig.assign((size_t)kSigW * kSigH * 4, 0);

    // GPU-таймер (необязателен)
    if (glIsland::hasExt("GL_EXT_disjoint_timer_query")) {
        g.genQ = (PFN_genQ)eglGetProcAddress("glGenQueriesEXT");
        g.delQ = (PFN_delQ)eglGetProcAddress("glDeleteQueriesEXT");
        g.beginQ = (PFN_beginQ)eglGetProcAddress("glBeginQueryEXT");
        g.endQ = (PFN_endQ)eglGetProcAddress("glEndQueryEXT");
        g.getQ = (PFN_getQ)eglGetProcAddress("glGetQueryObjectuivEXT");
        g.getQ64 = (PFN_getQ64)eglGetProcAddress("glGetQueryObjectui64vEXT");
        if (g.genQ && g.delQ && g.beginQ && g.endQ && g.getQ && g.getQ64) g.genQ(kQueries, g.q);
        else g.genQ = nullptr, g.beginQ = nullptr, g.getQ = nullptr;
    }
    if (glGetError() != GL_NO_ERROR) { drainGlErrors(); return (die("GL error during init"), false); }

    if (!selfTest()) return (die("self-test failed (the driver extrapolation is not usable; qcom_ex=2 forces it)"), false);

    g.ready = true;
    LOGI("qcom-ex: ready (%s output%s)", g.direct ? "direct" : "copied", g.beginQ ? ", GPU timer" : "");
    return true;
}

void reset() { g.prevValid = false; g.prevSigValid = false; }

void dropTargets() {
    if (!g.inited) return;
    if (!g.targets.empty()) glFinish();            // GL ещё может писать в слоты, которые вот-вот освободятся
    for (auto& t : g.targets) destroyTarget(t);
    g.targets.clear();
    g.begun = false;
    g.prevValid = false;
}

Stats takeStats() {
    Stats s{};
    s.frames = g.stFrames; s.extrapolated = g.stExtra; s.gpuSamples = g.stGpuN;
    s.cpuAvgMs = g.stFrames ? g.stCpuSum / (double)g.stFrames : 0.0;
    s.cpuMaxMs = g.stCpuMax;
    s.gpuAvgMs = g.stGpuN ? g.stGpuSum / (double)g.stGpuN : 0.0;
    s.gpuMaxMs = g.stGpuMax;
    g.stFrames = g.stExtra = g.stGpuN = 0;
    g.stCpuSum = g.stCpuMax = g.stGpuSum = g.stGpuMax = 0;
    return s;
}

Begin begin(AHardwareBuffer* src, int fence_fd, int cl, int ct, int cr, int cb, int bw, int bh,
            bool dupCheck, int dupThresh) {
    g.begun = false;
    if (!g.ready) return Begin::Fail;
    const int w = cr - cl, h = cb - ct;
    if (w < 16 || h < 16 || bw < w || bh < h) return Begin::Fail;
    g.t0 = now_ns();
    if (!ensureChain(w, h)) { LOGW("qcom-ex: could not allocate the frame chain %dx%d", w, h); return Begin::Fail; }
    if (!glIsland::waitFence(fence_fd)) return Begin::Fail;      // контент так и не готов
    ExtTex* e = importExt(src);
    if (!e) return Begin::Fail;
    timerBegin();
    drawCopy(e, cl, ct, cr, cb, bw, bh, w, h);

    if (dupCheck) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fboP[g.cur]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.fboDup);
        glBlitFramebuffer(0, 0, w, h, 0, 0, (GLint)kSigW, (GLint)kSigH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, g.fboDup);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, (GLint)kSigW, (GLint)kSigH, GL_RGBA, GL_UNSIGNED_BYTE, g.sig.data());   // ждёт конца копии
        bool dup = false;
        if (g.prevSigValid) {
            uint64_t sum = 0;
            const size_t n = (size_t)kSigW * kSigH * 4;
            for (size_t i = 0; i < n; i += 4)
                sum += (uint64_t)abs((int)g.sig[i] - g.prevSig[i]) + (uint64_t)abs((int)g.sig[i + 1] - g.prevSig[i + 1]) +
                       (uint64_t)abs((int)g.sig[i + 2] - g.prevSig[i + 2]);
            dup = ((double)sum / ((double)kSigW * kSigH * 3.0)) <= (double)dupThresh;
        }
        if (!dup) { g.prevSig.swap(g.sig); g.prevSigValid = true; }
        if (dup) {
            timerDiscard();
            g.cpuAccMs = 0;
            return Begin::Dup;
        }
    } else {
        g.prevSigValid = false;
    }
    g.w = w; g.h = h;
    g.begun = true;
    g.cpuAccMs = (double)(now_ns() - g.t0) / 1.0e6;
    return Begin::Ok;
}

bool finish(AHardwareBuffer* realDst, uint32_t n, AHardwareBuffer* const* genDst, const float* scale,
            bool allowExtrap, uint32_t* done, int* fenceFd) {
    *done = 0;
    *fenceFd = -1;
    if (!g.ready || !g.begun) return false;
    g.begun = false;
    const int64_t t1 = now_ns();
    const int w = g.w, h = g.h;
    Target* real = getTarget(realDst);
    if (!real) {
        if (g.errLogged++ < 3) LOGW("qcom-ex: ring buffer cannot be wrapped as a GL target");
        timerDiscard();
        return false;
    }
    // real: P[cur] -> слот (blit 1:1)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fboP[g.cur]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, real->fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    uint32_t made = 0;
    bool glErr = false;
    if (allowExtrap && g.prevValid && n > 0) {
        const GLuint prev = g.P[1 - g.cur], curT = g.P[g.cur];
        drainGlErrors();
        for (uint32_t i = 0; i < n; i++) {
            Target* dst = getTarget(genDst[i]);
            if (!dst) { glErr = true; break; }
            g.extrapolate(prev, curT, g.direct ? dst->tex : g.X, scale[i]);
            if (!g.direct) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fboX);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
                glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            made++;
        }
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) glErr = true;
        if (glErr) {
            drainGlErrors();
            if (g.errLogged++ < 5) LOGW("qcom-ex: GL error 0x%x in glExtrapolateTex2DQCOM — predicted frames dropped", (unsigned)err);
            made = 0;
            if (++g.errRun >= 3) {                         // подряд — режим сломан: возвращаемся на WFG
                die("repeated GL errors while extrapolating");
            }
        } else {
            g.errRun = 0;
        }
    }

    timerEnd();
    *fenceFd = glIsland::exportFence();
    g.cur = 1 - g.cur;
    g.prevValid = true;
    *done = made;

    const double cpu = g.cpuAccMs + (double)(now_ns() - t1) / 1.0e6;
    g.stFrames++; g.stExtra += made;
    g.stCpuSum += cpu; g.stCpuMax = std::max(g.stCpuMax, cpu);
    return true;
}

}  // namespace qcomEx
