cmake_minimum_required(VERSION 3.22)
project(myfg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Dobby (inline hook engine, Apache-2.0) ---------------------------------
# Берём либо third_party/Dobby (submodule / копия), либо тянем по git при configure.
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)

if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/Dobby/CMakeLists.txt)
  add_subdirectory(third_party/Dobby dobby EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(dobby
    GIT_REPOSITORY https://github.com/jmpews/Dobby.git
    # последний коммит до поломки master (рефакторинг апреля 2023, a588a0d)
    GIT_TAG        e9fe7fbecae47a2287e761080f8b1133cc22e8fa)
  FetchContent_MakeAvailable(dobby)
endif()

# Dobby (2023) собирает и .so-вариант (не используем): его линковка падает на известном баге
# Logger::Shared() в заголовке без inline. Нужен нам только dobby_static.
if(TARGET dobby)
  set_target_properties(dobby PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(TARGET dobby-shared)
  set_target_properties(dobby-shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Предупреждения чужого кода (Dobby, 2023) нам не нужны - в логе остаются только наши.
if(TARGET dobby_static)
  target_compile_options(dobby_static PRIVATE -w)
endif()

# ---- WFG (MIT) + компилятор шейдеров ---------------------------------------------
# Порядок поиска: -DGLSLC=... (build.sh подставляет glslc из Termux/PATH) -> shader-tools из NDK ->
# glslc в PATH -> glslangValidator (wfg/CMakeLists.txt умеет и его).
set(_ndk "$ENV{ANDROID_NDK_HOME}")
if(NOT _ndk)
  set(_ndk "${CMAKE_ANDROID_NDK}")
endif()
set(_shader_hints "")
if(_ndk)
  set(_shader_hints "${_ndk}/shader-tools/linux-x86_64" "${_ndk}/shader-tools/darwin-x86_64" "${_ndk}/shader-tools/windows-x86_64")
endif()
find_program(GLSLC glslc HINTS ${_shader_hints})
if(NOT GLSLC)
  find_program(GLSLANG_VALIDATOR glslangValidator)
endif()
if(NOT GLSLC AND NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "нет компилятора шейдеров: нужен glslc (NDK shader-tools / Termux: pkg install shaderc) или glslangValidator")
endif()
add_subdirectory(wfg)

# ---- сам модуль ---------------------------------------------------------------
add_library(myfg SHARED
  src/main.cpp
  src/config.cpp
  src/util.cpp
  src/hooks.cpp
  src/capture.cpp
  src/sc_capture.cpp
  src/fallback_egl.cpp
  src/fallback_vk.cpp
  src/live.cpp
  src/hud.cpp
  src/hdr.cpp
  src/qcom_me.cpp
  src/qcom_ex.cpp
  src/gl_island.cpp
  src/display.cpp
  src/vsync.cpp
  src/vk_ctx.cpp
  src/pipeline.cpp
)

target_include_directories(myfg PRIVATE src)
target_compile_options(myfg PRIVATE
  -O2 -g0 -fvisibility=hidden -fno-exceptions -fno-rtti
  -Wall -Wextra -Wno-unused-parameter -Wno-unguarded-availability)
target_link_options(myfg PRIVATE
  -Wl,--exclude-libs,ALL
  -Wl,--allow-multiple-definition # Dobby (2023) определяет Logger::Shared() в заголовке без inline
  -Wl,-z,max-page-size=16384      # Android 15+/16: страницы 16 КБ
  -Wl,--gc-sections -Wl,--build-id=none -s)

target_link_libraries(myfg PRIVATE dobby_static wfg log android nativewindow sync EGL GLESv3)

# Сборка родным clang из Termux (cmake/termux-toolchain.cmake): STL линкуем статически сами.
# Через NDK (android.toolchain.cmake) это делает ANDROID_STL=c++_static.
if(MYFG_LIBCXX_LIBS)
  target_link_options(myfg PRIVATE -nostdlib++)
  target_link_libraries(myfg PRIVATE ${MYFG_LIBCXX_LIBS})
endif()
