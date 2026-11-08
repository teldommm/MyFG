#pragma once
#include "config.h"
// Общие определения. Всё "переименуемое" (папка конфигов, тег лога) — здесь.
#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define MYFG_TAG "myfg"
// Настройки живут в каталоге модуля (их пишет WebUI): packages.list, config, config.d/<pkg>.
#define MYFG_DIR "/data/adb/modules/myfg/config"
#define MYFG_DIR_LEGACY "/data/adb/myfg"      // прежнее место: читается, только если в каталоге модуля файла нет

// log=0 в настройках — ни одной записи в logcat (процесс игры читает свой logcat без прав).
#define LOGI(...) do { if (g_cfg.log) __android_log_print(ANDROID_LOG_INFO,  MYFG_TAG, __VA_ARGS__); } while (0)
#define LOGW(...) do { if (g_cfg.log) __android_log_print(ANDROID_LOG_WARN,  MYFG_TAG, __VA_ARGS__); } while (0)
#define LOGE(...) do { if (g_cfg.log) __android_log_print(ANDROID_LOG_ERROR, MYFG_TAG, __VA_ARGS__); } while (0)
// Диагностика (адреса, размеры, первые кадры): пишется только при debug=1 в настройках.
#define LOGD(...) do { if (g_cfg.log && g_cfg.debug) __android_log_print(ANDROID_LOG_DEBUG, MYFG_TAG, __VA_ARGS__); } while (0)

// Имя потока видно в /proc/self/task/*/comm. Без debug=1 имя не ставим: поток наследует имя создателя
// и не выделяется среди потоков игры.
static inline void name_thread(const char* n) { if (g_cfg.debug) pthread_setname_np(pthread_self(), n); }

static inline int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   // та же база, что у Choreographer/ASurfaceTransaction
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Точка входа "в процессе игры": вызывается из postAppSpecialize (main.cpp).
namespace myfg {
void start(const char* config_blob, const char* package, bool aux);
// true — модуль загружен во вспомогательный GPU-процесс браузера (`пакет:privileged_process*`).
bool auxProcess();
}
