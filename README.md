# MyFG

**Zygisk frame generation module for Android games — fully open source.**

MyFG injects into the game process, captures its frames, builds intermediate frames using optical flow, and presents them in a separate layer on top of the game. The game itself is untouched: no APK patching or repacking — just add the package to the list via the WebUI.

> 🏆 **The first and only open-source Zygisk FG module.**

> ⚠️ **Work in progress.** The module is under active development: bugs, crashes and behavior changes between versions are expected.

> 📱 **Tested on Qualcomm (Snapdragon) devices only.** It may or may not work on other SoCs.

🇷🇺 Русская версия — в раскрывающемся блоке в конце файла.

## Features

- **Frame capture** via a `BufferQueue` hook (`android::Surface`), engine-agnostic. Fallback paths: `eglSwapBuffers` (GLES) and `vkQueuePresentKHR` (Vulkan).
- **WFG optical flow** (DIS on Vulkan compute) — the interpolation core.
- **PTS-phase interpolation** and vsync alignment (AChoreographer) for smooth frame pacing.
- **Output via `ASurfaceControl`** — a child layer above the game window.
- **Source classes:** video (MediaCodec) > game / remote render > the app's own UI window (HWUI, off by default: `ui_source`). Frames that arrive over Binder (video, a browser GPU process drawing into a `SurfaceView`) get their window from `BLASTBufferQueue` (`hook_bbq`), so the overlay sits exactly on the `SurfaceView` under the app UI. DRM (`PROTECTED_CONTENT`) frames are never captured. Chrome draws pages and video in its GPU process (`com.android.chrome:privileged_process0`) through SurfaceControl layers: the module hooks them (`hook_sc`, by default only in such a process), and the overlay is created as a sibling layer just above the source, mirroring its geometry, transparency and visibility. Add the Chrome package to `packages.list` and fully restart the browser. Overlay YUV video (a hardware decoder straight into a layer) is not captured yet.
- **Frame multiplier** x2–x4 (up to 7 generated frames per game frame), auto-selected from the display refresh rate.
- **Adaptive load:** when the GPU is overloaded, the module lowers the flow resolution first, then the number of generated frames.
- **Thermal throttling** based on battery temperature.
- **SDR → HDR10** (BT.2020 + PQ) with brightness, contrast and saturation controls.
- **HUD** with FPS / GAME / GEN / GPU counters.
- **Adreno extrapolation** (`qcom_ex`, experimental): instead of interpolating, the Adreno driver predicts frames from the last two (`GL_QCOM_frame_extrapolation`). No interpolation delay, but lower quality; replaces WFG. If the extension is missing or its self-test fails, WFG interpolation runs as before.
- **Duplicate frame detection** (game posts 60 fps with 30 unique frames).
- **WebUI**: global settings plus per-game profiles; some settings apply live, without restarting the game.

## Requirements

- Android 10+ (API 29+), **arm64**
- Root + **Zygisk must be installed and enabled** (Magisk with Zygisk turned on, or KernelSU / APatch with a Zygisk implementation). The module does not work without it.
- A Qualcomm device (the only hardware it has been tested on)
- An HDR-capable display for the HDR feature

## Installation

1. Download `myfg-module.zip` (Releases or GitHub Actions artifact).
2. Flash it in your root manager and reboot.
3. Open the module WebUI (manager → MyFG), add your games and tune the settings.
4. Restart the game.

Settings live in `/data/adb/modules/myfg/config/` (`config`, `packages.list`, `config.d/<package>`) and are carried over when the module is updated.

Logs: `logcat -s myfg WFG`

## Building

Requires NDK r27c (or Termux with clang) and the `glslc` shader compiler.

```bash
./build.sh
```

The script detects the environment (Termux or a host with NDK), builds the native part and packs a flashable `myfg-module.zip`. CI build: `.github/workflows/build.yml`.

## Project layout

| Path | Purpose |
|---|---|
| `src/` | Zygisk module: hooks, capture, pipeline, HDR, HUD, vsync |
| `wfg/` | WFG optical flow core and its compute shaders |
| `module/` | Magisk module files and WebUI |
| `third_party/Dobby/` | Inline hooking |
| `cmake/`, `tools/` | Toolchain and build utilities |

## License and credits

- **MyFG** — © 2026 [teldommm](https://github.com/teldommm), **GPL-3.0** (see `LICENSE`).
- **WFG** (optical flow core) — © 2026 [qwertypower](https://github.com/qwertypower) (DEVAR Entertainment LLC), MIT (see `wfg/LICENSE`).
- **Dobby** — Apache-2.0.

## Disclaimer

The project is **in development** and provided as is, without any warranty. The module injects into the game process — use at your own risk: in games with anti-cheat this may result in an account ban.

---

Made with love for the community. 💚

<details>
<summary>🇷🇺 Русский</summary>

# MyFG

**Zygisk-модуль генерации кадров (Frame Generation) для Android-игр — с полностью открытым исходным кодом.**

MyFG внедряется в процесс игры, перехватывает её кадры, строит промежуточные кадры по оптическому потоку и выводит их поверх игры отдельным слоем. Игра остаётся нетронутой: ни патчей APK, ни перепаковки — достаточно добавить пакет в список через WebUI.

> 🏆 **Первый и единственный FG-модуль для Zygisk с открытым исходником.**

> ⚠️ **Проект в разработке.** Возможны ошибки, вылеты и изменения поведения между версиями.

> 📱 **Проверено только на устройствах Qualcomm (Snapdragon).** На других SoC может не работать.

## Возможности

- **Захват кадров** через хук `BufferQueue` (`android::Surface`), без привязки к движку. Запасные пути: `eglSwapBuffers` (GLES) и `vkQueuePresentKHR` (Vulkan).
- **Оптический поток WFG** (DIS на Vulkan compute) — ядро интерполяции.
- **PTS-фазовая интерполяция** и привязка к vsync (AChoreographer) для ровного фрейм-пейсинга.
- **Вывод через `ASurfaceControl`** — дочерний слой поверх окна игры.
- **Классы источников:** видео (MediaCodec) > игра / удалённый рендер > собственное окно приложения (HWUI, по умолчанию выкл.: `ui_source`). Кадры, пришедшие по Binder (видео, GPU-процесс браузера в `SurfaceView`), получают окно из `BLASTBufferQueue` (`hook_bbq`): оверлей встаёт ровно на `SurfaceView` под интерфейсом приложения. Защищённые (DRM, `PROTECTED_CONTENT`) кадры не захватываются. Chrome рисует страницу и видео в GPU-процессе (`com.android.chrome:privileged_process0`) через слои SurfaceControl: модуль цепляется к ним (`hook_sc`, по умолчанию только в таком процессе), оверлей создаётся соседним слоем с z выше источника и повторяет его геометрию, прозрачность и видимость. Нужно добавить пакет Chrome в `packages.list` и полностью перезапустить браузер. Оверлейное YUV-видео (аппаратный декодер напрямую в слой) пока не захватывается.
- **Множитель кадров** x2–x4 (до 7 сгенерированных кадров на кадр игры), автоподбор по частоте дисплея.
- **Адаптивная нагрузка:** при перегрузе GPU сначала снижается разрешение потока, затем число генерируемых кадров.
- **Термо-троттлинг** по температуре батареи.
- **SDR → HDR10** (BT.2020 + PQ) с настройкой яркости, контраста и насыщенности.
- **HUD** со счётчиком FPS / GAME / GEN / GPU.
- **Экстраполяция Adreno** (`qcom_ex`, экспериментально): вместо интерполяции драйвер Adreno предсказывает кадры по двум последним (`GL_QCOM_frame_extrapolation`). Без задержки интерполяции, но качество ниже; заменяет WFG. Нет расширения или самотест не прошёл — работает интерполяция WFG, как раньше.
- **Детект повторных кадров** (игра постит 60 к/с при 30 уникальных).
- **WebUI:** глобальные настройки и отдельный профиль на каждую игру; часть настроек применяется на лету.

## Требования

- Android 10+ (API 29+), **arm64**
- Root + **установленный и включённый Zygisk** (Magisk с включённым Zygisk либо KernelSU / APatch с реализацией Zygisk). Без него модуль не работает.
- Устройство на Qualcomm (единственное железо, на котором проверялось)
- Для HDR — панель с поддержкой HDR

## Установка

1. Скачайте `myfg-module.zip` (Releases или артефакт GitHub Actions).
2. Установите zip через менеджер root-прав и перезагрузите устройство.
3. Откройте WebUI модуля (менеджер → MyFG), добавьте игры и настройте параметры.
4. Перезапустите игру.

Настройки хранятся в `/data/adb/modules/myfg/config/` и переносятся при обновлении модуля. Логи: `logcat -s myfg WFG`

## Сборка

Нужны NDK r27c (или Termux с clang) и `glslc`.

```bash
./build.sh
```

Результат — flashable-архив `myfg-module.zip`. Сборка в CI: `.github/workflows/build.yml`.

## Лицензии

- **MyFG** — © 2026 [teldommm](https://github.com/teldommm), **GPL-3.0**.
- **WFG** — © 2026 [qwertypower](https://github.com/qwertypower), MIT.
- **Dobby** — Apache-2.0.

## Дисклеймер

Проект **находится в разработке** и предоставляется «как есть», без гарантий. Модуль внедряется в процесс игры — используйте на свой риск: в играх с античитом это может привести к блокировке аккаунта.

Сделано с любовью к комьюнити. 💚

</details>
