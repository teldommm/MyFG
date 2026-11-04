# Тулчейн для сборки MyFG родным clang из Termux (aarch64) без установленного NDK.
#
# Компилятор и линкер - из Termux (pkg install clang lld). Заголовки и системные заглушки берутся
# из мини-sysroot, который build.sh выкачивает из официального NDK (tools/fetch_ndk_sysroot.py), поэтому:
#   - API 29 (ASurfaceControl и т.п.) вне зависимости от Termux;
#   - libc++ СТАТИЧЕСКАЯ (модуль грузится в процесс игры, где libc++_shared.so нет);
#   - заголовки Termux (другая версия libc++, нет EGL/GLES/Vulkan) не подмешиваются.
#
# Использование (делает build.sh):
#   MYFG_SYSROOT=<каталог> cmake -DCMAKE_TOOLCHAIN_FILE=cmake/termux-toolchain.cmake ...

if(NOT MYFG_SYSROOT)
  set(MYFG_SYSROOT "$ENV{MYFG_SYSROOT}")
endif()
if(NOT MYFG_SYSROOT OR NOT EXISTS "${MYFG_SYSROOT}/usr/include/android/surface_control.h")
  message(FATAL_ERROR "MYFG_SYSROOT не задан или пуст (${MYFG_SYSROOT}); запускайте через build.sh")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MYFG_SYSROOT)

set(CMAKE_SYSTEM_NAME Linux)        # Dobby: ветка SYSTEM.Linux (исходники те же, что для Android)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(MYFG_TRIPLE aarch64-linux-android29)
set(_common "--target=${MYFG_TRIPLE} --sysroot=${MYFG_SYSROOT}")
set(CMAKE_C_FLAGS_INIT   "${_common} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${_common}")
# Заголовки libc++ - только из sysroot (в Termux своя, другой версии, лежит рядом с clang).
set(CMAKE_CXX_FLAGS_INIT "${_common} -ffunction-sections -fdata-sections -nostdinc++ -isystem ${MYFG_SYSROOT}/usr/include/c++/v1")
set(_link "${_common} -fuse-ld=lld")
if(MYFG_NO_TERMUX_RPATH)   # опция есть только у clang из Termux (build.sh проверяет)
  string(APPEND _link " -fno-termux-rpath")
endif()
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_link}")

# Статическая libc++ + libunwind: подключаются к цели myfg в CMakeLists.txt вместе с -nostdlib++.
set(_lib "${MYFG_SYSROOT}/usr/lib/aarch64-linux-android")
set(MYFG_LIBCXX_LIBS "${_lib}/libc++_static.a" "${_lib}/libc++abi.a" "${_lib}/libunwind.a")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)   # проверка компилятора без линковки
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
