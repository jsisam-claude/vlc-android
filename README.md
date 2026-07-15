# vlc-light-win64

A lightweight win64-only video player for local files (.mp4, .mkv, .webm,
.avi) with subtitle support. Built entirely with the Visual Studio
toolchain from source text — no mingw, no MSYS, no vcpkg, no package
manager, no prebuilt binaries of any kind: FFmpeg is vendored as a pinned
source submodule and compiled by cl.exe using committed config headers
(its configure step is never run). Player structure follows ffplay;
WASAPI output adapted from mpv; rendering uses Windows' own D3D11 +
ID3D11VideoProcessor. Ships as a single statically-linked exe.

See [PLAN.md](PLAN.md) for the full design: reuse strategy and licensing,
the FFmpeg-under-MSVC build solution, renderer decision (staged D3D11, no
GDI/OpenGL), UI decision (raw Win32 v1, WinForms later if needed), and
gaps vs real VLC.

## Building (Windows, VS 2022+)

Requirements: Visual Studio 2022 or later with the **Desktop development
with C++** workload (includes CMake and Ninja). No other installs, no
downloads at build time.

From a **Developer PowerShell for VS** prompt:

```
git clone --recurse-submodules <this repo> && cd vlc-light-win64
cmake --preset x64-release
cmake --build --preset x64-release
build\x64-release\minimal-player.exe somefile.mkv
```

(If already cloned: `git submodule update --init --depth 1`.)

Or open the folder in the VS IDE — it picks up `CMakePresets.json`
automatically. The first build compiles the vendored FFmpeg sources
(~5–15 min once); incremental builds take seconds. FFmpeg's configure
never runs: the generated config headers are committed under
`third_party/ffmpeg-config/` (harvested once from a reference MSVC
configure, then hand-pinned; SIMD asm disabled — pure C).

CI builds run on every push (`.github/workflows/build.yml`) and upload
`minimal-player.exe` as an artifact.

Keys: Space pause · ←/→ ±10s · PgUp/PgDn ±60s · ↑/↓ volume ·
A audio track · S subtitle track · F or double-click fullscreen · Q quit.
Drop a video file onto the window to play it; a same-name `.srt`/`.ass`
next to the file loads automatically.

Generate test files with `tests\gen-samples.ps1` (needs any ffmpeg CLI on
PATH, used only to create inputs).
