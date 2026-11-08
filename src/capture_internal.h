#pragma once
// Внутренний интерфейс capture.cpp для sc_capture.cpp (выбор источника, очередь заданий, счётчики).
#include "capture.h"

namespace capture {
namespace detail {
int rateTouch(const void* key);
bool formatOk(uint32_t f);
void countRejectedFmt();
void countRejectedProt();
void countQueued();
bool scElect(const void* key, int w, int h, int rate_n);
void scSubmit(Job& j);
const void* electedKey();
bool dropElected(const void* key);
}  // namespace detail
}  // namespace capture
