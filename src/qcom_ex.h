#pragma once
// Аппаратная экстраполяция кадров Adreno — GL_QCOM_frame_extrapolation (OpenGL ES ext #333):
//   void glExtrapolateTex2DQCOM(GLuint src1, GLuint src2, GLuint output, GLfloat scaleFactor);
// Из двух последних кадров драйвер строит кадр «в будущем»: scaleFactor 0.5 — на полшага дальше src2
// (шаг = интервал между src1 и src2). Алгоритм драйвера закрытый, векторов/глубины/маски UI на входе нет.
//
// Режим qcom_ex заменяет интерполяцию WFG: реальный кадр показывается сразу (без задержки на ожидание
// следующего), а промежуточные кадры — предсказания по двум прошлым. Всё работает на общем GL-острове
// (gl_island, поток конвейера), Vulkan/WFG кадр не трогают:
//   game AHB -> EGLImage (GL_TEXTURE_EXTERNAL_OES, ждём acquire-fence на сервере) -> проход в приватную
//   RGBA8-текстуру P[cur] (обрезка до контент-ректа, любой формат буфера игры, в т.ч. YCbCr)
//   -> glBlitFramebuffer P[cur] -> слот кольца real (EGLImage-текстура AHB)
//   -> glExtrapolateTex2DQCOM(P[prev], P[cur], out, scale) -> прямо в слот кольца gen (или через
//      приватную X + blit, если драйвер не принял EGLImage-текстуру как выход — решает самотест)
//   -> eglDupNativeFenceFD: один sync_fd на весь кадр уходит в OutItem (CPU не ждёт GPU).
//
// Доступность проверяется при первом кадре (ensure): нет расширения/точки входа, ошибка GL или самотест
// на синтетике с известным сдвигом не прошёл — модуль «залипает» в недоступном состоянии (причина в
// логе, "qcom-ex: … (WFG stays the frame generator)"), конвейер продолжает работать через WFG.
// qcom_ex=2 в конфиге пропускает вердикт самотеста (диагностика: результат в логе остаётся).
#include <android/hardware_buffer.h>
#include <stdint.h>

namespace qcomEx {

// Ленивая инициализация (один раз, на потоке конвейера). false — режим недоступен на этом устройстве.
bool ensure();
bool available();

// Оборвать цепочку кадров (следующий кадр станет первым, предсказания не будет).
void reset();

// Уничтожить GL-обёртки слотов колец (кольца пересоздаются). Только на потоке конвейера.
void dropTargets();

// Статистика с прошлого вызова (сбрасывается): кадров, предсказанных кадров, CPU-время begin+finish
// (подача команд), GPU-время кадра (GL_EXT_disjoint_timer_query; samples = 0, если расширения нет).
struct Stats {
    uint64_t frames, extrapolated, gpuSamples;
    double cpuAvgMs, cpuMaxMs, gpuAvgMs, gpuMaxMs;
};
Stats takeStats();

// Скользящее среднее GPU-времени кадра, мс (0 — замеров ещё нет или таймер-запросов нет).
double gpuEmaMs();

enum class Begin { Ok, Dup, Fail };

// Шаг 1 (поток конвейера): забрать кадр игры в приватную текстуру. src — буфер игры (без нашей ссылки),
// fence_fd — acquire-fence (dup'ается внутри, владение НЕ забирается; -1 = готов), cl..cb — контент-рект,
// bw/bh — размер буфера. dupCheck: сравнить с предыдущим принятым кадром по сигнатуре 64x36
// (цена — ожидание CPU конца копии, ~сотни мкс); dupThresh — порог средней разницы (как dup_thresh).
// Dup — кадр повторяет предыдущий (в цепочку не попадает), Fail — кадр потерян (GL/fence).
Begin begin(AHardwareBuffer* src, int fence_fd, int cl, int ct, int cr, int cb, int bw, int bh,
            bool dupCheck, int dupThresh);

// Шаг 2: копия кадра в слот real (его AHB, размер = контент-рект) и n предсказаний в слоты gen[i] со
// scale[i] (доля шага: i-й из n кадров между реальными — (i+1)/(n+1)). allowExtrap=false — только real.
// *done — сколько предсказаний записано (0, если это первый кадр цепочки или драйвер вернул ошибку GL);
// *fenceFd — sync_fd завершения всех записанных команд (-1 = готово). false — real не записан (кадр
// потерян, слоты можно отпускать). Вызывается только после begin()==Ok.
bool finish(AHardwareBuffer* realDst, uint32_t n, AHardwareBuffer* const* genDst, const float* scale,
            bool allowExtrap, uint32_t* done, int* fenceFd);

}  // namespace qcomEx
