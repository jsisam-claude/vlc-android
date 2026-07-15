# vlc-light-win64

A lightweight win64-only video player for local files (.mp4, .mkv, .webm,
.avi) with subtitle support. Fully self-contained source-only repo built
entirely with the Visual Studio toolchain — no mingw, no MSYS, no vcpkg,
no package manager, no submodules, no downloads, no prebuilt binaries of
any kind: the needed FFmpeg source subset (927 files) is committed under
`third_party/ffmpeg-src/` and compiled by cl.exe using committed config
headers (FFmpeg's configure never runs). Player structure follows ffplay;
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

From an **x64 Native Tools Command Prompt for VS** (important: the plain
"Developer Command Prompt/PowerShell" targets x86 and the link will fail
with LNK4272 machine-type conflicts):

```
git clone <this repo> && cd vlc-light-win64
cmake --preset x64-release
cmake --build --preset x64-release
build\x64-release\minimal-player.exe somefile.mkv
```

Or open the folder in the VS IDE — it picks up `CMakePresets.json`
automatically. The first build compiles the vendored FFmpeg sources
(~5–15 min once); incremental builds take seconds. FFmpeg's configure
never runs: `tools/gen_ffmpeg_build.cmake` derives the config headers and
source list from the source text (committed under
`third_party/ffmpeg-config/` and `cmake/ffmpeg_sources.txt`), and
`tools/config_msvc_win64.h` is the hand-authored platform config. SIMD
asm is disabled (pure C decode). To upgrade FFmpeg: fetch a new tree
anywhere, run `tools/vendor_ffmpeg.cmake <tree>` + the generator, commit.

CI builds run on every push (`.github/workflows/build.yml`) and upload
`minimal-player.exe` as an artifact.

Controls: bottom bar (Play/Pause, ±10s, seek slider, volume slider) and
a right-click context menu (Open File, tracks, fullscreen). Keys: Space
pause · ←/→ ±10s · PgUp/PgDn ±60s · ↑/↓ volume · A audio track ·
S subtitle track · F or double-click fullscreen · O open · Q quit.
Drop a video file onto the window to play it; a same-name `.srt`/`.ass`
next to the file loads automatically.

Generate test files with `tests\gen-samples.ps1` (needs any ffmpeg CLI on
PATH, used only to create inputs).
