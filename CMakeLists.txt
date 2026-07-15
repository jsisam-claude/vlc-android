cmake_minimum_required(VERSION 3.24)
project(vlc_light_win64 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# Vendored FFmpeg, compiled by this toolchain from source text only —
# see cmake/ffmpeg.cmake. No package manager, no shell, no prebuilts.
include(cmake/ffmpeg.cmake)

add_executable(minimal-player WIN32
  src/main.cpp
  src/player.cpp
  src/demux.cpp
  src/decode.cpp
  src/queue.cpp
  src/audio_out.cpp
  src/video_out.cpp
  src/subs.cpp
)

if(MSVC)
  target_compile_options(minimal-player PRIVATE /W3 /utf-8)
endif()

target_link_libraries(minimal-player PRIVATE
  ffmpeg
  d3d11 dxgi d2d1 dwrite
  ole32 avrt winmm
  user32 shell32 gdi32
)
