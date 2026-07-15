# vlc-light-win64

A lightweight win64-only video player for local files (.mp4, .mkv, .webm,
.avi) with subtitle support. Built entirely with Visual Studio tooling
(MSVC/clang-cl, CMake, vcpkg) — no mingw, no MSYS, no external library
dependencies: FFmpeg is vendored and statically linked; player structure
follows ffplay; WASAPI output adapted from mpv; rendering uses Windows'
own D3D11VA + ID3D11VideoProcessor (zero-copy hw decode, driver
deinterlacing). Ships as a single ~8–15 MB exe.

See [PLAN.md](PLAN.md) for the full design: reuse strategy and licensing,
the FFmpeg-under-MSVC build solution, renderer decision (staged D3D11, no
GDI/OpenGL), UI decision (raw Win32 v1, WinForms later if needed), and
gaps vs real VLC.

## Building (Windows, VS 2022+)

Requirements: Visual Studio 2022 or later with the **Desktop development
with C++** workload (includes CMake, Ninja, and vcpkg). No other installs.

From a **Developer PowerShell for VS** prompt (it sets `VCPKG_ROOT` to
VS's bundled vcpkg):

```
git clone <this repo> && cd vlc-light-win64
cmake --preset x64-release
cmake --build --preset x64-release
build\x64-release\minimal-player.exe somefile.mkv
```

Or open the folder in the VS IDE — it picks up `CMakePresets.json` and
`vcpkg.json` automatically. The first configure compiles FFmpeg from
source via vcpkg (~30–45 min once, cached afterwards).

CI builds run on every push (`.github/workflows/build.yml`) and upload
`minimal-player.exe` as an artifact.

Keys: Space pause · ←/→ ±10s · PgUp/PgDn ±60s · ↑/↓ volume ·
A audio track · S subtitle track · F or double-click fullscreen · Q quit.
Drop a video file onto the window to play it; a same-name `.srt`/`.ass`
next to the file loads automatically.

Generate test files with `tests\gen-samples.ps1` (needs any ffmpeg CLI on
PATH, used only to create inputs).
