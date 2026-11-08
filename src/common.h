#pragma once
#include "config.h"
// Общие определения. Всё "переименуемое" (папка конфигов, тег лога) — здесь.
#include <android/log.h>
#include <stdint.h>
#include <time.h>

#define MYFG_TAG "myfg"
// Настройки живут в каталоге модуля (их пишет WebUI): packages.list, config, config.d/<pkg>.
#define MYFG_DIR "/data/adb/modules/myfg/config"
#define MYFG_DIR_LEGACY "/data/adb/myfg"      // прежнее место: читается, только если в каталоге модуля файла нет

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  MYFG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  MYFG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MYFG_TAG, __VA_ARGS__)
// Диагностика (адреса, размеры, первые кадры): пишется только при debug=1 в настройках.
#define LOGD(...) do { if (g_cfg.debug) __android_log_print(ANDROID_LOG_DEBUG, MYFG_TAG, __VA_ARGS__); } while (0)

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
