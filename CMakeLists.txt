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

# Embeddable engine: hosts (the bundled shell, or another app such as a
# gallery) link this and include only src/player.h.
add_library(player_engine STATIC
  src/player.cpp
  src/demux.cpp
  src/decode.cpp
  src/queue.cpp
  src/audio_out.cpp
  src/video_out.cpp
  src/subs.cpp
)
target_include_directories(player_engine PUBLIC src)
target_link_libraries(player_engine
  PRIVATE ffmpeg
  PUBLIC d3d11 dxgi d3dcompiler d2d1 dwrite ole32 avrt winmm user32
)

add_executable(minimal-player WIN32
  src/main.cpp
  src/version.rc
  src/app.manifest
)
target_link_libraries(minimal-player PRIVATE
  player_engine
  shell32 gdi32 comctl32 comdlg32
)

if(MSVC)
  target_compile_options(player_engine PRIVATE /W3 /utf-8)
  target_compile_options(minimal-player PRIVATE /W3 /utf-8)
endif()
