cmake_minimum_required(VERSION 3.24)
project(vlc_light_win64 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  # The default "Developer Command Prompt/PowerShell" targets x86; its LIB
  # paths make every x64 link fail with LNK4272/LNK2001 on all symbols.
  if(DEFINED ENV{LIB} AND "$ENV{LIB}" MATCHES "[\\\\/]x86(;|$)")
    message(FATAL_ERROR "This shell's LIB points at x86 libraries. "
      "Use the 'x64 Native Tools Command Prompt for VS' (or "
      "VsDevCmd.bat -arch=x64 -host_arch=x64), then reconfigure.")
  endif()
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
  d3d11 dxgi d3dcompiler d2d1 dwrite
  ole32 avrt winmm
  user32 shell32 gdi32 comctl32 comdlg32
)
