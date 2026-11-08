#include "qcom_me.h"
#include "common.h"

// GL_QCOM_motion_estimation (glsl-спека Khronos, ред. 1.0 от 2020-03-19):
//   void glTexEstimateMotionQCOM(uint ref, uint target, uint output);
//   ref/target — GL_R8 или EGLImage с luma-плоскостью, одинаковые размеры, кратные блоку;
//   output — GL_RGBA16F размером ref/блок: R,G = вектор (пиксели, ref->target, sub-pixel),
//   нет движения / блок пропущен — нули. Размер блока: glGetIntegerv(0x8C90/0x8C91).
// Схема на кадр N (всё на потоке конвейера, ~1-2 мс CPU):
//   import AHB(N) -> GL_TEXTURE_EXTERNAL_OES -> (wait acquire-fence) -> luma-проход в R8
//   -> ME(luma[N-1], luma[N] -> motion RGBA16F) -> refine + reduce на GPU -> glFinish
//   -> glReadPixels(FLOAT) -> host-буфер float. luma-снимки снимаются под защитой fence
//   своего кадра, поэтому переиспользование слота игрой позже не портит поле.
// Ориентация: luma-шейдер обязан быть сквозным БЕЗ переворота — тогда texture row 0 == строка 0
//   содержимого, вектора выходят в координатах содержимого (y вниз), как ждёт WFG
//   (vkr_dis_supply_flow). Readback с FBO идёт в порядке строк текстуры, без переворота.
//
//   - всё работает внутри нашего GL-конвейера ME, поле остаётся на GPU до финального readback
//     (WFG ждёт сетку блоков через vkr_dis_supply_flow, поэтому после уточнения делаем
//     блочную редукцию);
//   - сиды: аппаратное поле glTexEstimateMotionQCOM + временное поле прошлого кадра
//     (медиана 17 отводов по величине вектора — гасит блочный шум и дрожание ME
//     кадр-к-кадру);
//   - кандидаты: «лёгкий» режим — 5x5 вокруг сида с 4-угловой SAD, «полный» —
//     34 кандидата гексагональными кольцами (0.65..3.0 px) с 9-отводной SAD;
//   - gain-компенсация экспозиции в SAD (фейды/вспышки не ломают поиск) и «доверие» к
//     gain сида (sameDir/near1/diverged);
//   - защита центра экрана (r < 0.25 с поправкой на аспект): нулевой MV втрое лучше —
//     откат к нулю (HUD/прицел не искажается);
//   - выход: попиксельное поле rgba16f (xy = MV, z = gain) с временно́й обратной связью,
//     затем блочная редукция (медиана 17 на блок) в сетку ME -> readback -> WFG.
//   Требует ES 3.1 (compute); устройства с GL_QCOM_motion_estimation — это Adreno 6xx+,
//   у них ES 3.2, так что frag/comp dual-компиляция не нужна: нет 3.1 — тихо работаем
//   с сырым аппаратным полем (me_refine игнорируется, запись в лог).
//   Разрешение refine — ПОЛОВИННОЕ от luma (w/2 x h/2): диспатч и память mvFull в 4 раза
//   меньше (4 байта на пиксель luma вместо 16), точность для сетки блоков ME сохраняется
//   (блок ME >= 4x4 half-res текселя). Полное luma семплится LINEAR'ом со stride 2 (box 2x2),
//   аппаратный сид делится на 2 при чтении, редукция возвращает вектора в пикселях luma (x2)
//   — контракт с WFG не меняется.
//
// Самотест знака Y. Спека GL_QCOM_motion_estimation НЕ определяет направление
//   оси Y векторов (R/G = x/y, ref->target, субпиксельные — да; знак Y — нет), а наш контракт
//   с WFG — координаты содержимого, y вниз. При инициализации прогоняется синтетика с известным
//   вертикальным сдвигом (selfTestDy): драйвер, отдающий Y «вверх», определяется по медиане dy,
//   и поле приводится к контентным координатам — флип применяется к сырому полю на CPU и к
//   аппаратному сиду в refine (uniform uDySign); уточнённое поле уже в контентных координатах.
//   Результат — в логе ("dy self-test"), ручной override — ключ me_dy_sign (live).
//
// EGL-контекст и синтаксис fence'ей — общий GL-остров (gl_island): контекст один на модуль,
//   им пользуются и ME, и экстраполяция QCOM. ME не зависит от чужих расширений на этапе
//   контекста — только своего.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include "gl_island.h"
#include <dlfcn.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <vector>

#define GL_MOTION_ESTIMATION_SEARCH_BLOCK_X_QCOM 0x8C90
#define GL_MOTION_ESTIMATION_SEARCH_BLOCK_Y_QCOM 0x8C91
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif

namespace {

typedef void (*PFN_me)(GLuint, GLuint, GLuint);
// ES 3.1 (compute): разрешаются динамически — модуль линкуется с ES 3.0
typedef void (*PFN_glDispatchCompute_fn)(GLuint, GLuint, GLuint);
typedef void (*PFN_glBindImageTexture_fn)(GLuint, GLuint, GLint, GLboolean, GLenum, GLenum, GLenum);
typedef void (*PFN_glMemoryBarrier_fn)(GLbitfield);
typedef void (*PFN_glBindSampler_fn)(GLuint, GLuint);

// ---- импорты игрового буфера (кэш по указателю, lru kCache) ---------------------------------
// Кольцо буферов игры бывает 3–5 штук: при кэше меньше кольца LRU вытесняет ровно тот буфер,
// что понадобится следующим, и EGLImage + текстура пересоздаются каждый кадр.
constexpr int kCache = 8;
// Сетка потока WFG больше этого числа текселей не используется для выхода refine: readback
// растёт как fw*fh*16 байт (RGBA float), и выше порога возвращаемся к блочной сетке ME.
constexpr int64_t kMaxFlowGrid = 262144;   // ~ 512x512 (flow_side до ~400 при 16:10)

struct ExtTex {
    AHardwareBuffer* ahb = nullptr;    // с нашей ссылкой
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    GLuint tex = 0;
    uint64_t use = 0;
};

struct Me {
    void* gles = nullptr;              // libGLESv2.so для dlsym точек ES 3.1
    bool inited = false;             // пытались
    bool ready = false;              // работает
    bool failedReasonLogged = false;

    PFN_me estimateMotion = nullptr;

    // ES 3.1 compute: уточнение поля
    PFN_glDispatchCompute_fn dispatchCompute = nullptr;
    PFN_glBindImageTexture_fn bindImage = nullptr;
    PFN_glMemoryBarrier_fn memoryBarrier = nullptr;
    PFN_glBindSampler_fn bindSampler = nullptr;
    bool refineFailLogged = false;

    int blockX = 16, blockY = 16;
    GLuint prog = 0, vaolessVbo = 0;
    GLint uRect = -1, uSize = -1, uLuma = -1, uTex = -1;
    GLuint lumaTex[2] = {0, 0};      // ping-pong: [0]=prev после первого кадра
    int lumaSlot = 0;               // какой из них только что писали
    bool havePrev = false;
    GLuint motionTex = 0;            // сырое аппаратное поле (mw x mh)
    GLuint mvFull[2] = {0, 0};       // уточнённое попиксельное поле (w x h), ping-pong
    int fullSlot = 0;
    int fullW = 0, fullH = 0;         // хранилище уточнения (лениво: конфиг может включиться позже)
    GLuint motionClean = 0;          // поле после блочной редукции (mw x mh) — его читаем
    int cleanW = 0, cleanH = 0;
    bool temporalValid = false;      // mvFull[1-fullSlot] соответствует прошлой паре
    float dySign = 1.0f;             // знак Y аппаратного ME по самотесту: +1 = y вниз (контент), -1 = перевёрнуто
    GLuint progRefine[3] = {0, 0, 0};// [1] = лёгкий, [2] = полный
    GLint uUseTemporal[3] = {-1, -1, -1}, uTexelLuma[3] = {-1, -1, -1};
    GLint uTexelMv[3] = {-1, -1, -1}, uOut[3] = {-1, -1, -1};
    GLint uDySign[3] = {-1, -1, -1};// знак Y аппаратного сида (приведение к координатам содержимого)
    GLuint progReduce = 0;
    GLint ruBlockSize = -1, ruOutSize = -1;
    GLuint sampLin = 0;              // LINEAR для luma в refine (субпиксельные кандидаты)
    GLuint fboLuma = 0, fboRead = 0;
    int lumaW = 0, lumaH = 0;
    int moW = 0, moH = 0;
    std::vector<float> halfBuf;                      // RGBA16F readback (как GL_FLOAT), outW*outH*4
    std::vector<float> vecBuf;                       // x,y на ячейку сетки — наружу, outW*outH*2
    ExtTex cache[kCache];
    uint64_t tick = 0;
    // статистика кадра ME (снимается раз в интервал лога через takeStats)
    uint64_t stFrames = 0;
    double stTotMs = 0, stFinMs = 0, stMaxMs = 0;
    int lastOutW = 0, lastOutH = 0;                  // для лога смены сетки

    void fail(const char* why) {
        if (ready || !failedReasonLogged) LOGW("qcom-me: %s (DIS stays the flow solver)", why);
        failedReasonLogged = true;
        ready = false;
    }
} g;

void dropExt(ExtTex& e) {
    if (e.tex) { glDeleteTextures(1, &e.tex); e.tex = 0; }
    if (e.img != EGL_NO_IMAGE_KHR && glIsland::available())
        glIsland::fns().DestroyImageKHR(glIsland::display(), e.img);
    e.img = EGL_NO_IMAGE_KHR;
    if (e.ahb) { AHardwareBuffer_release(e.ahb); e.ahb = nullptr; }
}

ExtTex* importAhb(AHardwareBuffer* ahb) {
    int slot = -1;
    uint64_t minUse = ~0ull;
    for (int i = 0; i < kCache; i++) {
        ExtTex& e = g.cache[i];
        if (e.ahb == ahb && e.img != EGL_NO_IMAGE_KHR) { e.use = ++g.tick; return &e; }
        if (!e.ahb) { slot = i; break; }             // свободный — лучше всего
        if (e.use < minUse) { minUse = e.use; slot = i; }
    }
    if (slot < 0) return nullptr;
    dropExt(g.cache[slot]);
    ExtTex& e = g.cache[slot];
    AHardwareBuffer_acquire(ahb);
    e.ahb = ahb;
    EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    e.img = glIsland::fns().CreateImageKHR(glIsland::display(), EGL_NO_CONTEXT,
                                           EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)ahb, attrs);
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

// Ожидание acquire-fence игры — в общем GL-острове (gl_island.cpp): серверное eglWaitSync
// с CPU-фолбэком на poll. Контракт тот же: fd остаётся во владении вызывающего.

const char* kLumaVert =
    "#version 300 es\n"
    "const vec2 p[3]=vec2[3](vec2(-1.,-1.),vec2(3.,-1.),vec2(-1.,3.));\n"
    "void main(){gl_Position=vec4(p[gl_VertexID],0.,1.);}\n";
// ES3-шейдеру нужен GL_OES_EGL_image_external_essl3; запасной вариант — старое расширение
// (часть драйверов принимает только его).
const char* kLumaFragHead =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n";
const char* kLumaFragHeadOes =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n";
const char* kLumaFragBody =
    "uniform samplerExternalOES uTex;\n"     // буфер игры (RGB или YCbCr — неважно)
    "uniform vec4 uRect;uniform vec2 uSize;uniform vec2 uLuma;\n"
    "out vec4 o;\n"
    "void main(){\n"
    // сквозной БЕЗ переворота: texture row 0 == строка 0 содержимого, y вниз (см. шапку)
    "  vec2 c=gl_FragCoord.xy;\n"
    "  vec2 uv=(uRect.xy+c*(uRect.zw-uRect.xy)/uLuma)/uSize;\n"
    "  vec3 rgb=texture(uTex,uv).rgb;\n"
    "  o=vec4(dot(rgb,vec3(0.299,0.587,0.114)),0.,0.,1.);\n"
    "}\n";

// ---- compute-уточнение аппаратного поля ------------------------------------------------------
// Компилируется дважды: с "#define LIGHT 1" (me_refine=1) и "#define FULL 1"
// (me_refine=2). Разрешение ПОЛОВИННОЕ: диспатч идёт по сетке w/2 x h/2, p — центр
// half-res текселя (y вниз), mv — вектор ref->target в пикселях half-res. Полное luma (w x h)
// семплится через uTexelLuma = 2/(w,h): билинейная выборка на половинной сетке = точный box 2x2.
// Аппаратный сид приходит в пикселях luma и делится на 2; выход — half-res, блочная редукция
// (kReduceBody) возвращает пиксели luma. Никаких переворотов (см. шапку файла).
const char* kRefineBody =
    // Ветвление LIGHT/FULL — на уровне препроцессора GLSL (#define инжектится после
    // #version при сборке программы): C++-препроцессор строку не трогает.
    "layout(local_size_x=8, local_size_y=8) in;\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform mediump sampler2D uLumaPrev;\n"   // luma ref-кадра (N-1), R8, ПОЛНОЕ разрешение, LINEAR (sampLin)
    "uniform mediump sampler2D uLumaCur;\n"    // luma target-кадра (N), R8, ПОЛНОЕ разрешение, LINEAR (sampLin)
    "uniform mediump sampler2D uMvHw;\n"       // аппаратное поле (mw x mh): xy = MV блока В ПИКСЕЛЯХ LUMA, NEAREST
    "uniform mediump sampler2D uMvPrev;\n"     // прошлое уточнённое поле (w/2 x h/2): xy = MV half-res px, z = gain, NEAREST
    "uniform mediump float uUseTemporal;\n"    // 1 = uMvPrev соответствует прошлой паре
    "uniform mediump vec2 uTexelLuma;\n"       // 2 / (w, h): half-res пиксель -> UV полного luma
    "uniform mediump vec2 uTexelMv;\n"         // 1 / (w/2, h/2): сетка полей (temporal, выход)
    "uniform mediump vec2 uOutSize;\n"         // (w/2, h/2): границы диспатча
    "uniform mediump float uDySign;\n"         // знак Y аппаратного поля (самотест): сид -> координаты содержимого
    "layout(binding=0, rgba16f) uniform writeonly mediump image2D uMvOut;\n"   // half-res, MV half-res px
    // 9-отводный гексагональный шаблон SAD
    "const vec2 kTap9[9] = vec2[9](\n"
    "    vec2(0.0, 0.0),\n"
    "    vec2(2.0, 0.0), vec2(-2.0, 0.0),\n"
    "    vec2(1.0, 1.732), vec2(-1.0, 1.732), vec2(1.0, -1.732), vec2(-1.0, -1.732),\n"
    "    vec2(0.0, 3.464), vec2(0.0, -3.464));\n"
    "#ifdef LIGHT\n"
    "const float kStep5[5] = float[5](-1.5, -0.5, 0.0, 0.5, 1.5);\n"
    "#endif\n"
    "#ifdef FULL\n"
    // гексагональные кольца вокруг сида: 6 + 8 + 10 + 10 кандидатов
    "const vec2 kRing[34] = vec2[34](\n"
    "    vec2(0.65, 0.0), vec2(0.325, 0.5629), vec2(-0.325, 0.5629), vec2(-0.65, 0.0),\n"
    "    vec2(-0.325, -0.5629), vec2(0.325, -0.5629),\n"
    "    vec2(1.2472, 0.5166), vec2(0.5166, 1.2472), vec2(-0.5166, 1.2472), vec2(-1.2472, 0.5166),\n"
    "    vec2(-1.2472, -0.5166), vec2(-0.5166, -1.2472), vec2(0.5166, -1.2472), vec2(1.2472, -0.5166),\n"
    "    vec2(2.0448, 0.6644), vec2(1.2637, 1.7394), vec2(0.0, 2.15), vec2(-1.2637, 1.7394),\n"
    "    vec2(-2.0448, 0.6644), vec2(-2.0448, -0.6644), vec2(-1.2637, -1.7394), vec2(0.0, -2.15),\n"
    "    vec2(1.2637, -1.7394), vec2(2.0448, -0.6644),\n"
    "    vec2(3.0, 0.0), vec2(2.4271, 1.7634), vec2(0.9271, 2.8532), vec2(-0.9271, 2.8532),\n"
    "    vec2(-2.4271, 1.7634), vec2(-3.0, 0.0), vec2(-2.4271, -1.7634), vec2(-0.9271, -2.8532),\n"
    "    vec2(0.9271, -2.8532), vec2(2.4271, -1.7634));\n"
    "#endif\n"
    // медиана 17 векторов по величине (сеть обменов, элемент [8]) — гасит выбросы поля
    "vec2 median17(inout vec2 v[17]) {\n"
    "    for (int i = 0; i <= 8; i++)\n"
    "        for (int j = i + 1; j < 17; j++)\n"
    "            if (dot(v[j], v[j]) < dot(v[i], v[i])) { vec2 t = v[i]; v[i] = v[j]; v[j] = t; }\n"
    "    return v[8];\n"
    "}\n"
    "#ifdef FULL\n"
    // двунаправленная gain-компенсированная SAD по 9 отводам:
    // cur(p) ~ g0*prev(p-mv) и prev(p) ~ g1*cur(p+mv); усиления — из средних, клэмп для
    // устойчивости. Фейды/вспышки не ломают поиск: смена экспозиции уходит в g, а не в ошибку.
    "float sad(vec2 p, vec2 mv, float sg, out float gOut) {\n"   // sg не используется (доверие — режим LIGHT)
    "    float a[9]; float b[9]; float bw[9]; float aw[9];\n"
    "    float ma = 0.0; float mb = 0.0; float mbw = 0.0; float maw = 0.0;\n"
    "    for (int i = 0; i < 9; i++) {\n"
    "        vec2 q = p + kTap9[i];\n"
    "        a[i]  = textureLod(uLumaCur,  q * uTexelLuma, 0.0).r;\n"
    "        b[i]  = textureLod(uLumaPrev, q * uTexelLuma, 0.0).r;\n"
    "        bw[i] = textureLod(uLumaPrev, (q - mv) * uTexelLuma, 0.0).r;\n"
    "        aw[i] = textureLod(uLumaCur,  (q + mv) * uTexelLuma, 0.0).r;\n"
    "        ma += a[i]; mb += b[i]; mbw += bw[i]; maw += aw[i];\n"
    "    }\n"
    "    float g0 = clamp(ma / max(mbw, 0.001), 0.5, 2.0);\n"
    "    float g1 = clamp(mb / max(maw, 0.001), 0.5, 2.0);\n"
    "    gOut = g0;\n"
    "    float e = 0.0;\n"
    "    for (int i = 0; i < 9; i++)\n"
    "        e += abs(a[i] - bw[i] * g0) + abs(b[i] - aw[i] * g1);\n"
    "    return e;\n"
    "}\n"
    "#else\n"
    // 4-угловая SAD с «доверием» к gain сида: направление отклонения
    // усиления совпало с сидом и не разошлось больше 25% — верим сиду, иначе клэмп [0.8, 1.2]
    "float sad(vec2 p, vec2 mv, float sg, out float gOut) {\n"
    "    float a[4]; float b[4]; float bw[4]; float aw[4];\n"
    "    float ma = 0.0; float mb = 0.0; float mbw = 0.0; float maw = 0.0;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        vec2 c = p + vec2(float((i & 1) * 2 - 1), float((i & 2) - 1));\n"   // углы (±1, ±1)
    "        a[i]  = textureLod(uLumaCur,  c * uTexelLuma, 0.0).r;\n"
    "        b[i]  = textureLod(uLumaPrev, c * uTexelLuma, 0.0).r;\n"
    "        bw[i] = textureLod(uLumaPrev, (c - mv) * uTexelLuma, 0.0).r;\n"
    "        aw[i] = textureLod(uLumaCur,  (c + mv) * uTexelLuma, 0.0).r;\n"
    "        ma += a[i]; mb += b[i]; mbw += bw[i]; maw += aw[i];\n"
    "    }\n"
    "    float r0 = clamp(ma / max(mbw, 0.001), 0.0, 2.0);\n"
    "    float r1 = clamp(mb / max(maw, 0.001), 0.0, 2.0);\n"
    "    float s0 = sg; float s1 = sg;\n"
    "    if (!((sg - 1.0) * (r0 - 1.0) > 0.0) || abs(r0 - 1.0) < 0.05 ||\n"
    "        abs(r0 / max(sg, 0.001) - 1.0) > 0.25) s0 = clamp(r0, 0.8, 1.2);\n"
    "    if (!((sg - 1.0) * (r1 - 1.0) > 0.0) || abs(r1 - 1.0) < 0.05 ||\n"
    "        abs(r1 / max(sg, 0.001) - 1.0) > 0.25) s1 = clamp(r1, 0.8, 1.2);\n"
    "    gOut = s0;\n"
    "    float e = 0.0;\n"
    "    for (int i = 0; i < 4; i++)\n"
    "        e += abs(a[i] - bw[i] * s0) + abs(b[i] - aw[i] * s1);\n"
    "    return e;\n"
    "}\n"
    "#endif\n"
    "void main() {\n"
    "    ivec2 id = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (float(id.x) >= uOutSize.x || float(id.y) >= uOutSize.y) return;\n"
    "    vec2 p = vec2(id) + 0.5;\n"                     // центр half-res текселя, y вниз
    "    vec2 uv = p * uTexelMv;\n"                      // нормализованная позиция в кадре (общая для всех полей)
    // база поиска: аппаратный сид (блочный, NEAREST-апсемпл); сид в пикселях luma -> half-res.
    // Y сида приводится к координатам содержимого (y вниз): SAD ищет по luma в контентных
    // координатах, сид с перевёрнутым Y указывал бы не туда и отбрасывался бы поиском.
    "    float sg_ = 1.0;\n"
    "    vec2 hw = textureLod(uMvHw, uv, 0.0).xy;\n"
    "    hw.y *= uDySign;\n"
    "    vec2 base = hw * 0.5;\n"
    "    float g;\n"
    "    float bestErr = sad(p, base, sg_, g);\n"
    // временное поле: медиана 17 по кольцам 1.25 / 2.5 текселя
    "    if (uUseTemporal > 0.5) {\n"
    "        vec2 v[17];\n"
    "        int n = 1;\n"
    "        v[0] = textureLod(uMvPrev, uv, 0.0).xy;\n"
    "        for (int r = 0; r < 2; r++) {\n"
    "            float d = (r == 0) ? 1.25 : 2.5;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2( d, 0.0) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2(-d, 0.0) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2(0.0,  d) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2(0.0, -d) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2( d,  d) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2(-d,  d) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2( d, -d) * uTexelMv, 0.0).xy;\n"
    "            v[n++] = textureLod(uMvPrev, uv + vec2(-d, -d) * uTexelMv, 0.0).xy;\n"
    "        }\n"
    "        vec2 mvT = median17(v);\n"
    "        float eT = sad(p, mvT, sg_, g);\n"
    "        if (eT < bestErr) {\n"
    "            base = mvT; bestErr = eT;\n"
    "            sg_ = clamp(textureLod(uMvPrev, uv, 0.0).z, 0.25, 4.0);\n"
    "        }\n"
    "    }\n"
    "    vec2 bestMv = base;\n"
    "#ifdef FULL\n"
    "    for (int i = 0; i < 34; i++) {\n"
    "        vec2 mv = base + kRing[i];\n"
    "        float e = sad(p, mv, sg_, g);\n"
    "        if (e < bestErr) { bestErr = e; bestMv = mv; }\n"
    "    }\n"
    "#else\n"
    "    for (int j = 0; j < 5; j++)\n"
    "        for (int i = 0; i < 5; i++) {\n"
    "            vec2 mv = base + vec2(kStep5[i], kStep5[j]);\n"
    "            float e = sad(p, mv, sg_, g);\n"
    "            if (e < bestErr) { bestErr = e; bestMv = mv; }\n"
    "        }\n"
    "#endif\n"
    // защита центра (HUD/прицел): r < 0.25 с поправкой на аспект и нулевой MV
    // втрое лучше найденного — откат к нулю + ближний досмотр
    "    vec2 dc = uv - vec2(0.5);\n"
    "    dc.x *= uOutSize.x / uOutSize.y;\n"             // аспект half-res сетки = аспект luma
    "    if (dot(dc, dc) < 0.0625) {\n"
    "        float e0 = sad(p, vec2(0.0), sg_, g);\n"
    "        if (e0 * 3.0 < bestErr) {\n"
    "            bestMv = vec2(0.0); bestErr = e0;\n"
    "#ifdef FULL\n"
    "            for (int i = 0; i < 3; i++) {\n"       // ближние кандидаты вокруг нуля
    "                float e = sad(p, kRing[i], sg_, g);\n"
    "                if (e < bestErr) { bestErr = e; bestMv = kRing[i]; }\n"
    "            }\n"
    "#else\n"
    "            for (int j = 1; j < 4; j++)\n"
    "                for (int i = 1; i < 4; i++) {\n"
    "                    vec2 mv = vec2(kStep5[i], kStep5[j]);\n"
    "                    float e = sad(p, mv, sg_, g);\n"
    "                    if (e < bestErr) { bestErr = e; bestMv = mv; }\n"
    "                }\n"
    "#endif\n"
    "        }\n"
    "    }\n"
    // выход: xy = MV (пиксели luma, ref->target), z = gain (для «доверия» следующего кадра)
    "    float gz = 1.0;\n"
    "    sad(p, bestMv, sg_, gz);\n"
    "    imageStore(uMvOut, id, vec4(bestMv, clamp(gz, 0.25, 4.0), 1.0));\n"
    "}\n";

// ---- редукция уточнённого поля в сетку выхода (WFG принимает любую сетку) ---------------------
// Вход — half-res поле (w/2 x h/2, MV в half-res пикселях). Ячейка выхода покрывает
// uBlockSize пикселей luma (дробное: w/outW x h/outH) = uBlockSize/2 текселей half-res; 16
// отсчётов 4x4 по ячейке + центр -> медиана 17, вектора умножаются на 2 (half-res px ->
// пиксели luma). Выход — сетка outW x outH в пикселях luma. Сетка либо блочная (mw x mh, когда
// сетка потока WFG недоступна), либо РАЗМЕРА ПОТОКА WFG (fw x fh): тогда vkr_dis_supply_flow
// не пересэмплирует поле (тождественный путь), а разрешение refine не пропадает впустую.
const char* kReduceBody =
    "layout(local_size_x=8, local_size_y=8) in;\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform mediump sampler2D uMvFull;\n"     // half-res поле (w/2 x h/2), MV half-res px, NEAREST
    "uniform mediump vec2 uBlockSize;\n"       // блок ME в пикселях LUMA (полного разрешения)
    "uniform mediump vec2 uOutSize;\n"         // (mw, mh)
    "layout(binding=0, rgba16f) uniform writeonly mediump image2D uMvOut;\n"
    "vec2 median17(inout vec2 v[17]) {\n"
    "    for (int i = 0; i <= 8; i++)\n"
    "        for (int j = i + 1; j < 17; j++)\n"
    "            if (dot(v[j], v[j]) < dot(v[i], v[i])) { vec2 t = v[i]; v[i] = v[j]; v[j] = t; }\n"
    "    return v[8];\n"
    "}\n"
    "void main() {\n"
    "    ivec2 id = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (float(id.x) >= uOutSize.x || float(id.y) >= uOutSize.y) return;\n"
    "    vec2 texel = vec2(1.0) / vec2(textureSize(uMvFull, 0));\n"
    "    vec2 bh = uBlockSize * 0.5;\n"                     // блок в текселях half-res
    "    vec2 org = vec2(id) * bh;\n"                       // левый верхний угол блока (half-res px)
    "    vec4 c = textureLod(uMvFull, (org + bh * 0.5) * texel, 0.0);\n"
    "    vec2 v[17];\n"
    "    int n = 0;\n"                                        // сетка 4x4 по блоку
    "    for (int j = 0; j < 4; j++)\n"
    "        for (int i = 0; i < 4; i++)\n"
    "            v[n++] = textureLod(uMvFull,\n"
    "                (org + (vec2(i, j) + 0.5) * (bh * 0.25)) * texel, 0.0).xy * 2.0;\n"
    "    v[16] = c.xy * 2.0;\n"
    "    imageStore(uMvOut, id, vec4(median17(v), c.z, 1.0));\n"
    "}\n";

GLuint compile(GLenum kind, const char* src, bool quiet = false) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        if (!quiet) LOGW("qcom-me: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// compute-программа из тела (без #version): define инжектится после #version 310 es
GLuint buildCompute(const char* body, const char* define) {
    std::string src = std::string("#version 310 es\n") + define + body;
    GLuint s = compile(GL_COMPUTE_SHADER, src.c_str());
    if (!s) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    glDeleteShader(s);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOGW("qcom-me: compute link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ---- самотест знака Y аппаратного ME ---------------------------------------------------------
// Спека GL_QCOM_motion_estimation не определяет направление оси Y векторов. Наш контракт с WFG —
// координаты содержимого (y вниз, texture row 0 == строка 0: luma-проход сквозной). Прогоняем
// пару синтетических R8-текстур с детерминированным шумом: target — это ref, сдвинутый ВНИЗ на
// shift строк (контент уехал на +shift, вектор ref->target обязан быть (0, +shift) в контентных
// координатах). Медиана dy по всем блокам даёт фактическое направление оси Y драйвера:
// dy > 0 — контентные координаты (флип не нужен), dy < 0 — перевёрнутые (dySign = -1).
// Шум, а не градиент: вертикальный сдвиг сопоставляется однозначно, без апертуры неоднозначности.
// Бонус: самотест прогоняет весь путь ME (upload -> estimateMotion -> RGBA16F readback).
// Работает в контексте ensure() (контекст уже текущий), motionTex/fboRead уже созданы.
bool selfTestDy() {
    const int nb = 8;                              // сетка самотеста: 8x8 = 64 вектора
    const int tw = nb * g.blockX, th = nb * g.blockY;
    const int shift = g.blockY / 2;                // сдвиг полблока — заведомо в зоне поиска
    if (tw < 16 || th < 16 || shift < 1) return false;
    std::vector<uint8_t> ref((size_t)tw * th), tgt((size_t)tw * th);
    uint32_t h = 0x9E3779B9u;                      // фиксированный seed: тест воспроизводим
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            h ^= (uint32_t)x * 0x85EBCA6Bu;
            h = (h << 13) | (h >> 19);
            h *= 0xC2B2AE35u;
            ref[(size_t)y * tw + x] = (uint8_t)(h >> 24);
        }
    for (int y = 0; y < th; y++) {                 // контент уезжает ВНИЗ: target[y] = ref[y - shift]
        const int sy = y - shift >= 0 ? y - shift : 0;   // верхняя полоса без источника — медиана её погасит
        memcpy(&tgt[(size_t)y * tw], &ref[(size_t)sy * tw], (size_t)tw);
    }
    GLuint t[2] = {0, 0};
    glGenTextures(2, t);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, t[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, tw, th, 0, GL_RED, GL_UNSIGNED_BYTE, i ? tgt.data() : ref.data());
    }
    glBindTexture(GL_TEXTURE_2D, g.motionTex);     // под сетку самотеста (первый кадр всё равно пересоздаст)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, nb, nb, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    g.estimateMotion(t[0], t[1], g.motionTex);
    glFinish();
    std::vector<float> out((size_t)nb * nb * 4, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboRead);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.motionTex, 0);
    glReadPixels(0, 0, nb, nb, GL_RGBA, GL_FLOAT, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboLuma);
    glDeleteTextures(2, t);
    if (glGetError() != GL_NO_ERROR) return false;
    float dys[64], dxs[64];                        // медианы по всем блокам
    int n = 0;
    for (int k = 0; k < nb * nb; k++) {
        const float dx = out[k * 4], dy = out[k * 4 + 1];
        if (isfinite(dx) && isfinite(dy)) { dys[n] = dy; dxs[n] = dx; n++; }
    }
    if (n < nb) return false;                      // поле почти пустое — тест не состоялся
    std::sort(dys, dys + n);
    std::sort(dxs, dxs + n);
    const float dy = dys[n / 2], dx = dxs[n / 2];
    const float expect = (float)shift;
    g.dySign = dy < 0.0f ? -1.0f : 1.0f;
    LOGI("qcom-me: dy self-test: shift +%d px -> median dy %+.2f (dx %+.2f, %d blocks): "
         "driver y-axis %s -> output %s",
         shift, dy, dx, n,
         dy >= 0.0f ? "matches content (y down)" : "is flipped (y up)",
         g.dySign < 0.0f ? "corrected, y negated" : "as is");
    if (fabsf(dy) < expect * 0.5f)
        LOGW("qcom-me: dy self-test inconclusive (|dy| %.2f for shift %d) — assuming y down; "
             "if interpolation drifts vertically, set me_dy_sign=1|2", fabsf(dy), shift);
    if (fabsf(dx) > expect * 0.5f)
        LOGW("qcom-me: dy self-test: unexpected dx %+.2f for a vertical shift — pattern matched "
             "wrongly, treat the sign result with care", dx);
    return true;
}

}  // namespace

namespace qcomMe {

bool available() { return g.ready; }

void reset() {                  // без GL-вызовов: безопасно из любого состояния
    g.havePrev = false;
    g.temporalValid = false;    // временное поле соответствует разорванной цепочке
}

bool ensure() {
    if (g.inited) return g.ready;
    g.inited = true;
    auto die = [&](const char* why) { g.fail(why); return false; };

    // Контекст/fence-синтаксис — общий GL-остров. Поднялся один раз — им пользуются
    // и ME, и экстраполяция; unavailable здесь означает «на этом устройстве нет острова».
    if (!glIsland::ensure()) return die("GL island unavailable (see gl-island reason above)");
    g.gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);   // gl* может быть уже в процессе (игра)

    // расширение может быть у драйвера GLES или у загруженной игры — ищем везде
    g.estimateMotion = (PFN_me)dlsym(RTLD_DEFAULT, "glTexEstimateMotionQCOM");
    if (!g.estimateMotion && g.gles) g.estimateMotion = (PFN_me)dlsym(g.gles, "glTexEstimateMotionQCOM");
    if (!g.estimateMotion) g.estimateMotion = (PFN_me)eglGetProcAddress("glTexEstimateMotionQCOM");
    if (!g.estimateMotion) return die("glTexEstimateMotionQCOM not resolved");

    if (!glIsland::hasExt("GL_QCOM_motion_estimation")) return die("GL_QCOM_motion_estimation not advertised");

    glGetIntegerv(GL_MOTION_ESTIMATION_SEARCH_BLOCK_X_QCOM, &g.blockX);
    glGetIntegerv(GL_MOTION_ESTIMATION_SEARCH_BLOCK_Y_QCOM, &g.blockY);
    if (g.blockX < 4 || g.blockX > 64 || g.blockY < 4 || g.blockY > 64)
        return die("implausible ME block size");

    GLuint vs = compile(GL_VERTEX_SHADER, kLumaVert);
    std::string fsSrc = std::string(kLumaFragHead) + kLumaFragBody;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc.c_str(), true);
    if (!fs) {
        fsSrc = std::string(kLumaFragHeadOes) + kLumaFragBody;
        fs = compile(GL_FRAGMENT_SHADER, fsSrc.c_str());
    }
    if (!vs || !fs) return die("luma shader broken");
    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glLinkProgram(g.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &linked);
    if (!linked) return die("luma program link failed");
    g.uRect = glGetUniformLocation(g.prog, "uRect");
    g.uSize = glGetUniformLocation(g.prog, "uSize");
    g.uLuma = glGetUniformLocation(g.prog, "uLuma");
    g.uTex = glGetUniformLocation(g.prog, "uTex");

    glGenFramebuffers(1, &g.fboLuma);
    glGenFramebuffers(1, &g.fboRead);
    glGenTextures(2, g.lumaTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g.lumaTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenTextures(1, &g.motionTex);
    glBindTexture(GL_TEXTURE_2D, g.motionTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // проверка readback: RGBA16F обязан быть колор-рендерабельным (EXT_color_buffer_half_float / ES3.2)
    const int tw = 4 * g.blockX, th = 4 * g.blockY;
    glBindTexture(GL_TEXTURE_2D, g.motionTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tw / g.blockX, th / g.blockY, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboRead);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.motionTex, 0);
    const bool rbOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboLuma);
    if (!rbOk) return die("RGBA16F not color-renderable (readback impossible)");
    if (glGetError() != GL_NO_ERROR) return die("GL error during init");

    // ---- знак Y аппаратного ME. Спека направление Y не определяет, а WFG ждёт координаты
    // содержимого (y вниз). Приоритет — ручной ключ me_dy_sign (1/2), иначе самотест на синтетике.
    // ДО сборки refine-программ: знак входит в uniform uDySign сида.
    if (g_cfg.me_dy_sign == 1) {
        g.dySign = 1.0f;
        LOGI("qcom-me: dy sign forced by me_dy_sign=1 (no flip)");
    } else if (g_cfg.me_dy_sign == 2) {
        g.dySign = -1.0f;
        LOGI("qcom-me: dy sign forced by me_dy_sign=2 (y negated)");
    } else if (!selfTestDy()) {
        g.dySign = 1.0f;
        LOGW("qcom-me: dy self-test failed (GL error / empty field) — assuming y down; "
             "if interpolation drifts vertically, set me_dy_sign=1|2");
    }

    // ---- compute-уточнение поля (ES 3.1). Устройств с GL_QCOM_motion_estimation
    // без ES 3.1 на практике нет (Adreno 6xx+): нет 3.1 — просто работаем с сырым
    // аппаратным полем.
    int vmaj = 0, vmin = 0;
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (ver) sscanf(ver, "OpenGL ES %d.%d", &vmaj, &vmin);
    if (vmaj > 3 || (vmaj == 3 && vmin >= 1)) {
        g.dispatchCompute = (PFN_glDispatchCompute_fn)dlsym(RTLD_DEFAULT, "glDispatchCompute");
        if (!g.dispatchCompute && g.gles) g.dispatchCompute = (PFN_glDispatchCompute_fn)dlsym(g.gles, "glDispatchCompute");
        if (!g.dispatchCompute) g.dispatchCompute = (PFN_glDispatchCompute_fn)eglGetProcAddress("glDispatchCompute");
        g.bindImage = (PFN_glBindImageTexture_fn)dlsym(RTLD_DEFAULT, "glBindImageTexture");
        if (!g.bindImage && g.gles) g.bindImage = (PFN_glBindImageTexture_fn)dlsym(g.gles, "glBindImageTexture");
        if (!g.bindImage) g.bindImage = (PFN_glBindImageTexture_fn)eglGetProcAddress("glBindImageTexture");
        g.memoryBarrier = (PFN_glMemoryBarrier_fn)dlsym(RTLD_DEFAULT, "glMemoryBarrier");
        if (!g.memoryBarrier && g.gles) g.memoryBarrier = (PFN_glMemoryBarrier_fn)dlsym(g.gles, "glMemoryBarrier");
        if (!g.memoryBarrier) g.memoryBarrier = (PFN_glMemoryBarrier_fn)eglGetProcAddress("glMemoryBarrier");
        g.bindSampler = (PFN_glBindSampler_fn)dlsym(RTLD_DEFAULT, "glBindSampler");
        if (!g.bindSampler && g.gles) g.bindSampler = (PFN_glBindSampler_fn)dlsym(g.gles, "glBindSampler");
        if (!g.bindSampler) g.bindSampler = (PFN_glBindSampler_fn)eglGetProcAddress("glBindSampler");
        if (g.dispatchCompute && g.bindImage && g.memoryBarrier) {
            g.progRefine[1] = buildCompute(kRefineBody, "#define LIGHT 1\n");
            g.progRefine[2] = buildCompute(kRefineBody, "#define FULL 1\n");
            g.progReduce = buildCompute(kReduceBody, "");
            if (g.progRefine[1] && g.progRefine[2] && g.progReduce) {
                for (int m = 1; m <= 2; m++) {
                    glUseProgram(g.progRefine[m]);
                    glUniform1i(glGetUniformLocation(g.progRefine[m], "uLumaPrev"), 0);
                    glUniform1i(glGetUniformLocation(g.progRefine[m], "uLumaCur"), 1);
                    glUniform1i(glGetUniformLocation(g.progRefine[m], "uMvHw"), 2);
                    glUniform1i(glGetUniformLocation(g.progRefine[m], "uMvPrev"), 3);
                    g.uUseTemporal[m] = glGetUniformLocation(g.progRefine[m], "uUseTemporal");
                    g.uTexelLuma[m] = glGetUniformLocation(g.progRefine[m], "uTexelLuma");
                    g.uTexelMv[m] = glGetUniformLocation(g.progRefine[m], "uTexelMv");
                    g.uOut[m] = glGetUniformLocation(g.progRefine[m], "uOutSize");
                    g.uDySign[m] = glGetUniformLocation(g.progRefine[m], "uDySign");
                    glUniform1f(g.uDySign[m], g.dySign);   // уточняется каждый кадр (me_dy_sign live)
                }
                glUseProgram(g.progReduce);
                glUniform1i(glGetUniformLocation(g.progReduce, "uMvFull"), 0);
                g.ruBlockSize = glGetUniformLocation(g.progReduce, "uBlockSize");
                g.ruOutSize = glGetUniformLocation(g.progReduce, "uOutSize");
                glUseProgram(0);
                // LINEAR-сэмплер для luma: субпиксельные кандидаты не должны квантоваться
                glGenSamplers(1, &g.sampLin);
                glSamplerParameteri(g.sampLin, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glSamplerParameteri(g.sampLin, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glSamplerParameteri(g.sampLin, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glSamplerParameteri(g.sampLin, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glGenTextures(2, g.mvFull);
                for (int i = 0; i < 2; i++) {
                    glBindTexture(GL_TEXTURE_2D, g.mvFull[i]);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glGenTextures(1, &g.motionClean);
                glBindTexture(GL_TEXTURE_2D, g.motionClean);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else {
                glDeleteProgram(g.progRefine[1]); g.progRefine[1] = 0;
                glDeleteProgram(g.progRefine[2]); g.progRefine[2] = 0;
                glDeleteProgram(g.progReduce); g.progReduce = 0;
            }
        }
    }
    if (!g.progRefine[1] || !g.progRefine[2] || !g.progReduce)
        LOGW("qcom-me: refine unavailable (no ES 3.1 compute) — raw hardware field, me_refine ignored");

    LOGI("qcom-me: Adreno hardware motion estimation ready (block %dx%d, refine %s)",
         g.blockX, g.blockY, (g.progRefine[1] && g.progRefine[2]) ? "light+full@half" : "off");
    g.ready = true;
    return true;
}

Stats takeStats() {
    Stats s;
    s.frames = g.stFrames; s.avgMs = g.stFrames ? g.stTotMs / (double)g.stFrames : 0.0;
    s.avgStallMs = g.stFrames ? g.stFinMs / (double)g.stFrames : 0.0; s.maxMs = g.stMaxMs;
    g.stFrames = 0; g.stTotMs = g.stFinMs = g.stMaxMs = 0.0;
    return s;
}

bool frame(AHardwareBuffer* ahb, int fence_fd, int cl, int ct, int cr, int cb, int bw, int bh,
           int flowW, int flowH, const float** vec, uint32_t* gw, uint32_t* gh) {
    *vec = nullptr; *gw = *gh = 0;
    if (!g.ready) return false;
    const int64_t tStart = now_ns();
    if (!ahb) { g.havePrev = g.temporalValid = false; return false; }
    if (bw < 256 || bh < 256) { g.havePrev = g.temporalValid = false; return false; }   // ME на мелком окне не имеет смысла

    // рабочая область: кратна блоку
    const int w = (cr - cl) / g.blockX * g.blockX;
    const int h = (cb - ct) / g.blockY * g.blockY;
    if (w < 4 * g.blockX || h < 4 * g.blockY) { g.havePrev = g.temporalValid = false; return false; }
    const int mw = w / g.blockX, mh = h / g.blockY;

    if (w != g.lumaW || h != g.lumaH) {
        g.havePrev = g.temporalValid = false;
        g.fullW = g.fullH = g.cleanW = g.cleanH = 0;   // хранилища уточнения больше не соответствуют размеру
        g.lumaW = w; g.lumaH = h; g.moW = mw; g.moH = mh;
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, g.lumaTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
        glBindTexture(GL_TEXTURE_2D, g.motionTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, mw, mh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    }

    ExtTex* ext = importAhb(ahb);
    if (!ext) { g.havePrev = g.temporalValid = false; return false; }
    if (!glIsland::waitFence(fence_fd)) { g.havePrev = g.temporalValid = false; return false; }   // контент ещё не готов — цепочка с нуля

    // luma-проход: prev не трогаем, пишем в свободный слот
    const int cur = g.havePrev ? 1 - g.lumaSlot : g.lumaSlot;
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboLuma);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.lumaTex[cur], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { g.fail("luma FBO incomplete"); return false; }
    glViewport(0, 0, w, h);
    glUseProgram(g.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext->tex);
    glUniform1i(g.uTex, 0);
    glUniform4f(g.uRect, (float)cl, (float)ct, (float)cr, (float)cb);
    glUniform2f(g.uSize, (float)bw, (float)bh);
    glUniform2f(g.uLuma, (float)w, (float)h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (glGetError() != GL_NO_ERROR) { g.fail("GL error in luma pass"); return false; }

    if (!g.havePrev) {                    // первый кадр цепочки: он станет prev
        g.havePrev = true;
        g.lumaSlot = cur;
        g.temporalValid = false;           // временное поле (если было) больше не соответствует парам
        return false;
    }

    // аппаратный ME: prev -> cur, результат в motion (RGBA16F, R/G = вектор, ref->target)
    g.estimateMotion(g.lumaTex[g.lumaSlot], g.lumaTex[cur], g.motionTex);
    if (glGetError() != GL_NO_ERROR) { g.fail("GL error in motion estimation"); g.havePrev = g.temporalValid = false; return false; }

    // Эффективный знак Y на этот кадр — ручной ключ me_dy_sign (live) поверх самотеста.
    // Сырое поле и аппаратный сид приводятся к координатам содержимого (y вниз);
    // уточнённое поле уже в них (refine ищет SAD по luma без переворотов).
    const float dys = g_cfg.me_dy_sign == 1 ? 1.0f : g_cfg.me_dy_sign == 2 ? -1.0f : g.dySign;

    // ---- GPU-уточнение (me_refine: 0 — сырьё, 1 — лёгкое, 2 — полное; читается каждый кадр — live)
    // Работает на ПОЛОВИННОМ разрешении luma (w/2 x h/2): в 4 раза меньше диспатч и память
    // (mvFull ping-pong RGBA16F — 4 байта на пиксель luma вместо 16). w >= 4*block >= 16,
    // поэтому w2/h2 >= 8 всегда; чётность w не гарантируется (нечётный блок) — floor и
    // CLAMP_TO_EDGE на краю, MV-масштаб (x0.5/x2) от этого не зависят.
    GLuint readTex = g.motionTex;
    bool refined = false;
    int outW = mw, outH = mh;             // сетка, которая уйдёт наружу: блочная (сырое поле) или сетка потока WFG
    const int mode = g_cfg.me_refine;
    const int w2 = w / 2, h2 = h / 2;
    if ((mode == 1 || mode == 2) && g.progRefine[mode] && g.progReduce && g.mvFull[0] && g.mvFull[1] && g.motionClean) {
        // Сетка выхода refine = сетка потока WFG (если известна и не слишком велика): supply_flow
        // тогда не пересэмплирует поле, а разрешение refine не теряется на блочной редукции.
        int ow = mw, oh = mh;
        if (flowW >= 4 && flowH >= 4 && (int64_t)flowW * flowH <= kMaxFlowGrid) { ow = flowW; oh = flowH; }
        if (g.fullW != w2 || g.fullH != h2) {
            // хранилище попиксельного поля (лениво: me_refine могли включить после старта ME)
            g.fullW = w2; g.fullH = h2;
            g.temporalValid = false;
            for (int i = 0; i < 2; i++) {
                glBindTexture(GL_TEXTURE_2D, g.mvFull[i]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w2, h2, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            }
        }
        if (g.cleanW != ow || g.cleanH != oh) {
            // выходная сетка меняется при смене flow_side (governor): временное поле от этого не страдает
            g.cleanW = ow; g.cleanH = oh;
            glBindTexture(GL_TEXTURE_2D, g.motionClean);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ow, oh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        }
        glUseProgram(g.progRefine[mode]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.lumaTex[g.lumaSlot]);
        if (g.bindSampler) g.bindSampler(0, g.sampLin);      // luma — LINEAR (субпиксельные кандидаты)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g.lumaTex[cur]);
        if (g.bindSampler) g.bindSampler(1, g.sampLin);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g.motionTex);            // аппаратный сид — NEAREST
        if (g.bindSampler) g.bindSampler(2, 0);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, g.mvFull[1 - g.fullSlot]);   // временное поле (валидность — юниформом)
        if (g.bindSampler) g.bindSampler(3, 0);
        glActiveTexture(GL_TEXTURE0);
        glUniform1f(g.uUseTemporal[mode], g.temporalValid ? 1.0f : 0.0f);
        glUniform1f(g.uDySign[mode], dys);                      // Y сида -> координаты содержимого
        glUniform2f(g.uTexelLuma[mode], 2.0f / (float)w, 2.0f / (float)h);   // half-res px -> UV полного luma
        glUniform2f(g.uTexelMv[mode], 1.0f / (float)w2, 1.0f / (float)h2);
        glUniform2f(g.uOut[mode], (float)w2, (float)h2);
        g.bindImage(0, g.mvFull[g.fullSlot], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        g.dispatchCompute((GLuint)((w2 + 7) / 8), (GLuint)((h2 + 7) / 8), 1);
        g.memoryBarrier(0xFFFFFFFFu);

        // блочная редукция: half-res поле -> сетка ME (что ждёт vkr_dis_supply_flow);
        // блок в half-res текселях = blockSize/2, MV *2 в шейдере — выход в пикселях luma
        glUseProgram(g.progReduce);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.mvFull[g.fullSlot]);
        if (g.bindSampler) g.bindSampler(0, 0);
        glUniform2f(g.ruBlockSize, (float)w / (float)g.cleanW, (float)h / (float)g.cleanH);   // luma px на ячейку выхода
        glUniform2f(g.ruOutSize, (float)g.cleanW, (float)g.cleanH);
        g.bindImage(0, g.motionClean, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        g.dispatchCompute((GLuint)((g.cleanW + 7) / 8), (GLuint)((g.cleanH + 7) / 8), 1);
        g.memoryBarrier(0xFFFFFFFFu);
        g.bindImage(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);   // отвязать image перед readback
        glActiveTexture(GL_TEXTURE0);

        if (glGetError() != GL_NO_ERROR) {
            // уточнение сломалось — ME не бросаем, дальше кадры идут с сырым полем
            if (!g.refineFailLogged) LOGW("qcom-me: GL error in refine — raw hardware field from now on");
            g.refineFailLogged = true;
            g.progRefine[1] = g.progRefine[2] = g.progReduce = 0;
        } else {
            readTex = g.motionClean;
            outW = g.cleanW; outH = g.cleanH;
            refined = true;
        }
    }
    if (outW != g.lastOutW || outH != g.lastOutH) {
        g.lastOutW = outW; g.lastOutH = outH;
        LOGD("qcom-me: output grid %dx%d (%s), ME block grid %dx%d", outW, outH,
             refined ? (outW == mw && outH == mh ? "refined, block grid" : "refined, flow resolution") : "raw hardware field", mw, mh);
    }
    if (g.halfBuf.size() < (size_t)outW * outH * 4) g.halfBuf.resize((size_t)outW * outH * 4);
    if (g.vecBuf.size() < (size_t)outW * outH * 2) g.vecBuf.resize((size_t)outW * outH * 2);

    const int64_t tFin0 = now_ns();
    glFinish();                           // вектора нужны на CPU: блокирует поток конвейера до конца ME+refine на GPU
    const int64_t tFin1 = now_ns();
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboRead);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readTex, 0);
    glReadPixels(0, 0, outW, outH, GL_RGBA, GL_FLOAT, g.halfBuf.data());   // (RGBA, FLOAT) гарантирован для float-таргета
    glBindFramebuffer(GL_FRAMEBUFFER, g.fboLuma);
    if (glGetError() != GL_NO_ERROR) { g.fail("GL error in readback"); g.havePrev = g.temporalValid = false; return false; }

    // Luma-проход растягивает ВЕСЬ контент (cr-cl)x(cb-ct) на w x h (кратно блоку), поэтому вектора
    // измерены в пикселях растянутой сетки. Возвращаем их в пиксели контента: иначе амплитуда
    // занижена на (cw-w)/cw (до нескольких % на малых окнах).
    const float kx = (float)(cr - cl) / (float)w;
    const float ky = (float)(cb - ct) / (float)h;
    const float dysRead = readTex == g.motionTex ? dys : 1.0f;   // сырое поле — флип; уточнённое уже контентное
    // не-конечные (их не бывает по спеке, но драйверы разные) — нули
    for (size_t k = 0; k < (size_t)outW * outH; k++) {
        const size_t s = k * 4, d = k * 2;
        const float x = g.halfBuf[s];
        const float y = g.halfBuf[s + 1];
        g.vecBuf[d] = isfinite(x) ? x * kx : 0.0f;
        g.vecBuf[d + 1] = isfinite(y) ? y * ky * dysRead : 0.0f;
    }
    g.lumaSlot = cur;                     // только что снятый luma — prev следующего кадра
    if (refined) { g.fullSlot = 1 - g.fullSlot; g.temporalValid = true; }
    else g.temporalValid = false;
    *vec = g.vecBuf.data();
    *gw = (uint32_t)outW; *gh = (uint32_t)outH;
    const int64_t tEnd = now_ns();
    const double totMs = (double)(tEnd - tStart) / 1e6;
    g.stFrames++;
    g.stTotMs += totMs;
    g.stFinMs += (double)(tFin1 - tFin0) / 1e6;
    if (totMs > g.stMaxMs) g.stMaxMs = totMs;
    return true;
}

}  // namespace qcomMe
