#pragma once
#include <android/native_window.h>

namespace hooks {
// Одна попытка установки всего, что ещё не стоит. true — всё нужное стоит.
// Вызывается циклом из myfg::start() (libvulkan/libEGL могут подгрузиться позже).
bool tryInstall();

// Реестр окон игры (до 4 последних, с acquire). Основной источник — хук Surface в capture.cpp
// (точное окно, в которое игра отдаёт кадры); запасные — eglCreateWindowSurface /
// ANativeWindow_fromSurface (включаются, если хук Surface не встал).
void noteWindow(ANativeWindow* w);

// pickWindow возвращает окно, размер которого подходит под (w,h), иначе самое новое;
// результат — с взятой ссылкой, вызывающий делает ANativeWindow_release.
// Используется, только если capture::electedWindow() пуст.
ANativeWindow* pickWindow(int w, int h);

// Если окно w есть в реестре — берёт на него ссылку и возвращает его, иначе nullptr.
ANativeWindow* acquireIfKnown(ANativeWindow* w);
}
