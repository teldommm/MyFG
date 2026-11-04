#!/usr/bin/env bash
# MyFG: сборка нативной части и упаковка модуля в один flashable zip.
#
#   Termux:      ./build.sh          - сам доставит пакеты и мини-sysroot из NDK, соберёт и упакует
#   хост с NDK:  ANDROID_NDK_HOME=/path/to/ndk ./build.sh       (CI, ПК)
#
# Результат: myfg-module.zip в корне проекта (+ копия MyFG-<версия>.zip в Загрузки, если есть доступ).
#
# Необязательные переменные:
#   ANDROID_NDK_HOME   NDK, который запускается на ЭТОМ устройстве (в Termux - сборка lzhiyong/termux-ndk).
#                      Если не задан или не запускается - собирается родным clang из Termux (без NDK).
#   MYFG_MODE          ndk | termux - принудительно выбрать способ сборки
#   MYFG_NDK_VERSION   версия NDK, из которой берутся заголовки и статическая libc++ (по умолчанию 27c)
#   MYFG_CACHE         где хранить мини-sysroot (по умолчанию ~/.cache/myfg)
#   MYFG_JOBS          число потоков компиляции (по умолчанию nproc)
#   MYFG_CLEAN=1       пересобрать с нуля
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
BUILD="$ROOT/build"
OUT_ZIP="$ROOT/myfg-module.zip"
NDK_SYSROOT_VER="${MYFG_NDK_VERSION:-27c}"
CACHE="${MYFG_CACHE:-${HOME:-$ROOT}/.cache/myfg}"
JOBS="${MYFG_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mОШИБКА:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

IS_TERMUX=0
case "${PREFIX:-}" in *com.termux*) IS_TERMUX=1 ;; esac

# ---- выбор способа сборки ---------------------------------------------------------------------
# ndk    - штатный android.toolchain.cmake (нужен NDK, запускающийся на этом устройстве)
# termux - clang из Termux + мини-sysroot из официального NDK (заголовки, API 29, статическая libc++)
ndk_usable() {  # успех, если $1 - NDK, чей clang реально запускается здесь
  local n="$1" c
  [ -f "$n/build/cmake/android.toolchain.cmake" ] || return 1
  for c in "$n"/toolchains/llvm/prebuilt/*/bin/clang; do
    if [ -x "$c" ] && "$c" --version >/dev/null 2>&1; then return 0; fi
  done
  return 1
}
find_ndk() {
  local n
  for n in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}" \
           "${HOME:-/nonexistent}"/android-ndk* "${HOME:-/nonexistent}"/ndk* \
           "${HOME:-/nonexistent}"/Android/Sdk/ndk/* "${PREFIX:-/nonexistent}"/opt/android-ndk*; do
    if [ -n "$n" ] && [ -d "$n" ] && ndk_usable "$n"; then echo "$n"; return 0; fi
  done
  return 1
}

MODE="${MYFG_MODE:-}"
NDK=""
case "$MODE" in
  ""|ndk)
    if NDK="$(find_ndk)"; then
      MODE=ndk
    elif [ "$MODE" = ndk ]; then
      die "MYFG_MODE=ndk, но запускаемый NDK не найден (ANDROID_NDK_HOME='${ANDROID_NDK_HOME:-}')"
    else
      [ -z "${ANDROID_NDK_HOME:-}" ] || warn "NDK '$ANDROID_NDK_HOME' не запускается на этом устройстве - собираю clang'ом из Termux"
      MODE=termux
    fi ;;
  termux) ;;
  *) die "MYFG_MODE должен быть ndk или termux" ;;
esac

# ---- зависимости ------------------------------------------------------------------------------
PKGS=()
want() { have "$1" || PKGS+=("$2"); }
check_deps() {
  PKGS=()
  want cmake cmake
  want ninja ninja
  want zip zip
  want git git
  if [ "$MODE" = termux ]; then
    want clang clang
    want clang++ clang
    want ld.lld lld
    want python3 python
    if ! have ar && ! have llvm-ar; then PKGS+=(binutils); fi
  fi
  # компилятор шейдеров: NDK-шный glslc - x86_64 и на телефоне не запустится
  if [ "$MODE" = termux ] || [ "$(uname -m)" != x86_64 ]; then
    if ! have glslc && ! have glslangValidator; then PKGS+=(shaderc); fi
  fi
}
install_deps() {
  check_deps
  [ "${#PKGS[@]}" -eq 0 ] && return 0
  if [ "$IS_TERMUX" = 1 ]; then
    say "Устанавливаю пакеты Termux: ${PKGS[*]}"
    pkg install -y "${PKGS[@]}" || die "pkg install не удался (попробуйте 'pkg update' и повторите)"
    check_deps
    [ "${#PKGS[@]}" -eq 0 ] || die "после установки всё ещё не хватает: ${PKGS[*]}"
  else
    die "не хватает: ${PKGS[*]} (установите их и повторите)"
  fi
}

# ---- Dobby: кладём пинованный коммит в third_party/Dobby, чтобы CMake не ходил в сеть сам -------
ensure_dobby() {
  [ -f third_party/Dobby/CMakeLists.txt ] && return 0
  local sha
  sha="$(grep -oE 'GIT_TAG[[:space:]]+[0-9a-f]{40}' CMakeLists.txt | head -1 | awk '{print $2}')"
  [ -n "$sha" ] || return 0   # коммит не найден - пусть FetchContent разбирается сам
  say "Dobby: загружаю коммит ${sha:0:7} в third_party/Dobby"
  rm -rf third_party/Dobby.tmp
  mkdir -p third_party/Dobby.tmp
  if ( cd third_party/Dobby.tmp \
       && git init -q \
       && git remote add origin https://github.com/jmpews/Dobby.git \
       && git fetch -q --depth 1 origin "$sha" \
       && git checkout -q FETCH_HEAD ); then
    rm -rf third_party/Dobby
    mv third_party/Dobby.tmp third_party/Dobby
  else
    rm -rf third_party/Dobby.tmp
    warn "не удалось скачать Dobby заранее - CMake попробует сам (нужен доступ к github.com)"
  fi
}

# ---- мини-sysroot из NDK (для режима termux) ---------------------------------------------------
SR=""
ensure_sysroot() {
  SR="$CACHE/ndk-r${NDK_SYSROOT_VER}-sysroot"
  if [ ! -f "$SR/NDK_VERSION" ]; then
    say "Загружаю заголовки и статическую libc++ из NDK r${NDK_SYSROOT_VER} (~4 МБ, один раз)"
    mkdir -p "$CACHE"
    python3 -I "$ROOT/tools/fetch_ndk_sysroot.py" "$NDK_SYSROOT_VER" "$SR" 29 \
      || die "не удалось получить sysroot из NDK (нужен доступ к dl.google.com). Альтернатива: задайте ANDROID_NDK_HOME"
  fi
}

# ---- конфигурация CMake ------------------------------------------------------------------------
configure() {  # $1 = режим
  local stamp
  EXTRA=()
  # NDK-шный glslc - x86_64; на ARM-устройстве берём установленный (pkg install shaderc)
  if [ "$(uname -m)" != x86_64 ] && have glslc; then EXTRA+=("-DGLSLC=$(command -v glslc)"); fi
  if [ "$1" = ndk ]; then
    stamp="ndk|$NDK"
  else
    stamp="termux|$SR|$(command -v clang)"
  fi
  if [ "${MYFG_CLEAN:-0}" = 1 ] || [ "$(cat "$BUILD/.myfg-stamp" 2>/dev/null || true)" != "$stamp" ]; then
    rm -rf "$BUILD"
  fi
  if [ "$1" = ndk ]; then
    say "Конфигурация: NDK $NDK"
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
      ${EXTRA[@]+"${EXTRA[@]}"} || return 1
  else
    say "Конфигурация: clang $(clang --version | head -1 | sed 's/.*version //;s/ .*//') + sysroot NDK r${NDK_SYSROOT_VER}"
    # clang из Termux по умолчанию прописывает в .so rpath на свой sysroot - здесь он не нужен
    if echo | clang -fno-termux-rpath -x c -fsyntax-only - >/dev/null 2>&1; then
      EXTRA+=("-DMYFG_NO_TERMUX_RPATH=ON")
    fi
    MYFG_SYSROOT="$SR" cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/termux-toolchain.cmake" -DMYFG_SYSROOT="$SR" \
      -DCMAKE_C_COMPILER="$(command -v clang)" -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
      ${EXTRA[@]+"${EXTRA[@]}"} || return 1
  fi
  mkdir -p "$BUILD" && printf '%s' "$stamp" > "$BUILD/.myfg-stamp"
}

install_deps
ensure_dobby
if [ "$MODE" = termux ]; then
  ensure_sysroot
  configure termux || die "конфигурация CMake не удалась"
else
  if ! configure ndk; then
    # NDK нашёлся и запускается, но его cmake-тулчейн не заработал на этом хосте -> без NDK
    if [ "$IS_TERMUX" = 1 ] && [ -z "${MYFG_MODE:-}" ]; then
      warn "NDK $NDK не подошёл для сборки на этом устройстве - переключаюсь на clang из Termux"
      MODE=termux
      install_deps
      ensure_sysroot
      configure termux || die "конфигурация CMake не удалась"
    else
      die "конфигурация CMake не удалась"
    fi
  fi
fi

# ---- сборка ------------------------------------------------------------------------------------
say "Компиляция (потоков: $JOBS)"
cmake --build build --target myfg -j"$JOBS"   # только наш таргет: shared-сборка Dobby не нужна

SO="$BUILD/libmyfg.so"
[ -f "$SO" ] || die "не найден $SO"

# проверка результата: ELF arm64 и никакой зависимости от libc++_shared (в процессе игры её нет)
[ "$(od -An -tx1 -N4 "$SO" | tr -d ' \n')" = 7f454c46 ] || die "$SO - не ELF"
[ "$(od -An -tx1 -j18 -N2 "$SO" | tr -d ' \n')" = b700 ] || die "$SO собран не для arm64"
if grep -qa 'libc++_shared\.so' "$SO"; then
  die "libmyfg.so зависит от libc++_shared.so - STL должна быть статической"
fi
if grep -qa 'com\.termux' "$SO"; then
  warn "в libmyfg.so попал путь Termux (rpath) - на устройстве с игрой он безвреден, но лишний"
fi

# ---- упаковка модуля ---------------------------------------------------------------------------
say "Упаковка модуля"
STAGE="$BUILD/pkg"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a module/. "$STAGE/"
find "$STAGE" \( -name '.DS_Store' -o -name '*.orig' -o -name '*~' \) -delete
mkdir -p "$STAGE/zygisk"
cp "$SO" "$STAGE/zygisk/arm64-v8a.so"
chmod 0644 "$STAGE/zygisk/arm64-v8a.so"
chmod 0755 "$STAGE/META-INF/com/google/android/update-binary" "$STAGE/customize.sh" "$STAGE/live.sh" 2>/dev/null || true
rm -f "$OUT_ZIP"
(cd "$STAGE" && zip -qr9X "$OUT_ZIP" .)

LIST="$(zip -sf "$OUT_ZIP")"
for f in module.prop customize.sh live.sh zygisk/arm64-v8a.so webroot/index.html \
         META-INF/com/google/android/update-binary META-INF/com/google/android/updater-script; do
  printf '%s\n' "$LIST" | grep -q "^[[:space:]]*$f\$" || die "в архиве нет $f"
done

VER="$(sed -n 's/^version=//p' module/module.prop | head -1 | tr -d '\r')"
SIZE="$(du -h "$OUT_ZIP" | cut -f1)"
echo
say "Готово: $OUT_ZIP ($SIZE, ${VER:-?})"
have sha256sum && echo "    sha256: $(sha256sum "$OUT_ZIP" | cut -d' ' -f1)"

# копия в общую память, чтобы сразу выбрать в менеджере модулей
for d in "${HOME:-/nonexistent}/storage/downloads" /sdcard/Download /storage/emulated/0/Download; do
  if [ -d "$d" ] && [ -w "$d" ]; then
    cp -f "$OUT_ZIP" "$d/MyFG-${VER:-build}.zip" && echo "    копия: $d/MyFG-${VER:-build}.zip"
    break
  fi
done
if [ "$IS_TERMUX" = 1 ] && [ ! -d "${HOME:-/nonexistent}/storage/downloads" ]; then
  echo "    (чтобы копия появлялась в Загрузках, один раз выполните: termux-setup-storage)"
fi
echo "    Установка: менеджер (KernelSU/Magisk) -> Модули -> Установить из хранилища -> $(basename "$OUT_ZIP")"
