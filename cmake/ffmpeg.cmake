# Vendored FFmpeg built directly by the VS toolchain — no configure, no
# shell, no msys, no nasm, no package manager. The configure step is
# replaced by committed generated-text files: config headers and
# component list sources in third_party/ffmpeg-config (harvested once
# from a reference MSVC configure) plus the source list in
# cmake/ffmpeg_sources.txt. SIMD asm is disabled (pure C decode);
# hardware decode later compensates on the video path.

set(FF_SRC ${CMAKE_SOURCE_DIR}/third_party/ffmpeg)
set(FF_CFG ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-config)

if(NOT EXISTS ${FF_SRC}/libavcodec/avcodec.h)
  message(FATAL_ERROR "FFmpeg submodule missing - run: git submodule update --init --depth 1")
endif()

file(STRINGS ${CMAKE_SOURCE_DIR}/cmake/ffmpeg_sources.txt FF_REL_SOURCES
     REGEX "^[^#].*\\.c$")
set(FF_SOURCES "")
foreach(s IN LISTS FF_REL_SOURCES)
  list(APPEND FF_SOURCES ${FF_SRC}/${s})
endforeach()

add_library(ffmpeg STATIC ${FF_SOURCES})
set_target_properties(ffmpeg PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)

# Config/generated tree first so its config.h and *_list.c win; per-lib
# config dirs let quoted includes of generated files resolve.
target_include_directories(ffmpeg BEFORE PRIVATE
  ${FF_CFG}
  ${FF_CFG}/libavcodec ${FF_CFG}/libavformat ${FF_CFG}/libavutil
  ${FF_CFG}/libswscale ${FF_CFG}/libswresample
  ${FF_SRC})
target_include_directories(ffmpeg INTERFACE ${FF_CFG} ${FF_SRC})

target_compile_definitions(ffmpeg PRIVATE
  HAVE_AV_CONFIG_H
  _USE_MATH_DEFINES
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
  _WINSOCK_DEPRECATED_NO_WARNINGS)

if(MSVC)
  # third-party code: silence warnings; C11 atomics needed by ffmpeg>=6
  target_compile_options(ffmpeg PRIVATE /W0 /experimental:c11atomics)
endif()

target_link_libraries(ffmpeg INTERFACE
  ws2_32 secur32 bcrypt mfuuid strmiids ole32 user32)
