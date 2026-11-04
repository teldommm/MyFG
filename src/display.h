#pragma once
#include <jni.h>

namespace display {

void init(JavaVM* vm);

// HDR-возможности основной панели (ActivityThread.currentApplication() ->
// getSystemService("window") -> getDefaultDisplay() -> getHdrCapabilities()).
//   *supported: панель сообщает хотя бы один HDR-тип;  *peak_nits: getDesiredMaxLuminance() (0 — не сообщено).
// false — запрос не удался (нет приложения, ограничения на скрытые API, исключение): состояние неизвестно.
bool hdrInfo(bool* supported, int* peak_nits);

}  // namespace display
