cmake_minimum_required(VERSION 3.24)
project(vlc_light_win64 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W3 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

find_package(FFMPEG REQUIRED)

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

target_include_directories(minimal-player PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(minimal-player PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(minimal-player PRIVATE
  ${FFMPEG_LIBRARIES}
  d3d11 dxgi d2d1 dwrite
  ole32 avrt winmm
  user32 shell32 gdi32
  ws2_32 secur32 bcrypt
)
