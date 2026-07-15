# Vendored FFmpeg built directly by the VS toolchain — no configure, no
# shell, no msys, no nasm, no package manager. The configure step is
# replaced by committed generated-text files: config headers and
# component list sources in third_party/ffmpeg-config (harvested once
# from a reference MSVC configure) plus the source list in
# cmake/ffmpeg_sources.txt. SIMD asm is disabled (pure C decode);
# hardware decode later compensates on the video path.

set(FF_SRC ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-src)
set(FF_CFG ${CMAKE_SOURCE_DIR}/third_party/ffmpeg-config)

if(NOT EXISTS ${FF_SRC}/libavcodec/avcodec.h)
  message(FATAL_ERROR "third_party/ffmpeg-src missing - the vendored FFmpeg subset should be committed in this repo (see tools/vendor_ffmpeg.py)")
endif()

file(STRINGS ${CMAKE_SOURCE_DIR}/cmake/ffmpeg_sources.txt FF_REL_SOURCES
     REGEX "^[^#].*\\.c$")

# One static lib per FFmpeg library, mirroring FFmpeg's own build: each
# compiles with its *own* directory on the include path (subdirectory
# files include parent headers like "bsf_internal.h", and every lib has
# its own "internal.h" that must not cross-resolve).
set(FF_LIBS libavutil libswresample libswscale libavcodec libavformat)
foreach(lib IN LISTS FF_LIBS)
  set(srcs "")
  foreach(s IN LISTS FF_REL_SOURCES)
    if(s MATCHES "^${lib}/")
      list(APPEND srcs ${FF_SRC}/${s})
    endif()
  endforeach()
  add_library(ff_${lib} STATIC ${srcs})
  set_target_properties(ff_${lib} PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
  target_include_directories(ff_${lib} BEFORE PRIVATE
    ${FF_CFG}            # config.h, config_components.h, generated *_list.c
    ${FF_SRC})           # path-qualified includes ("libavutil/...")
  if(lib STREQUAL "libavcodec")
    # Only avcodec has non-arch subdirectories (aac/, opus/, bsf/) whose
    # sources include parent-dir headers. Do NOT do this for libavutil:
    # it contains time.h, which would hijack #include <time.h>.
    target_include_directories(ff_${lib} BEFORE PRIVATE ${FF_SRC}/${lib})
  endif()
  target_compile_definitions(ff_${lib} PRIVATE
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS)
  if(MSVC)
    # third-party code: silence warnings; C11 atomics needed by ffmpeg>=6
    target_compile_options(ff_${lib} PRIVATE /W0 /experimental:c11atomics)
  endif()
endforeach()

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE ${FF_CFG} ${FF_SRC})
target_link_libraries(ffmpeg INTERFACE
  ff_libavformat ff_libavcodec ff_libswresample ff_libswscale ff_libavutil
  ws2_32 secur32 bcrypt mfuuid strmiids ole32 user32)
