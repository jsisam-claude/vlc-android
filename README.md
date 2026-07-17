# vlc-light-win64

A lightweight win64-only video player for local files (mp4/mkv/webm/avi
plus ts/m2ts, flv, wmv/asf, ogv, mpg/vob and 3gp) with subtitle support.
Codecs cover H.264, HEVC, MPEG-1/2/4 (DivX/Xvid), VC-1/WMV, VP6/8/9,
Theora, MJPEG, ProRes and AV1 (AV1 needs a GPU with AV1 decode — FFmpeg
has no in-tree software AV1 decoder), with AAC/AC-3/E-AC-3/DTS/TrueHD/
MP2/MP3/FLAC/Vorbis/Opus/ALAC/WMA audio. Music files (mp3, flac, m4a,
ogg, wav, wma, opus...) play too: embedded cover art is shown when
present, otherwise a live spectrum visualization, with title/artist from
the tags. Fully self-contained
source-only repo built entirely with the Visual Studio toolchain — no
mingw, no MSYS, no vcpkg, no package manager, no submodules, no
downloads, no prebuilt binaries of any kind: the needed FFmpeg source
subset (1117 files) is committed under `third_party/ffmpeg-src/` and
compiled by cl.exe using committed config headers (FFmpeg's configure
never runs). Player structure follows ffplay; WASAPI output adapted from
mpv; rendering uses Windows' own D3D11 + ID3D11VideoProcessor.
H.264/HEVC/MPEG-2/VP9/VC-1/AV1 decode in hardware via
D3D11VA when the GPU supports it (zero-copy into the video processor),
falling back to software decode transparently. Phone-rotation metadata
is applied, 10-bit content stays 10-bit through the video processor
(P010), and BT.2020/PQ/HLG colorspaces are signalled to the driver so
HDR files don't wash out. http/https URLs play too (Ctrl+U), with TLS
via Windows' own Schannel - still zero third-party binaries - and HLS
streams (.m3u8, live and VOD, including AES-128-encrypted segments).
Ships as a single statically-linked exe.

See [PLAN.md](PLAN.md) for the full design: reuse strategy and licensing,
the FFmpeg-under-MSVC build solution, renderer decision (staged D3D11, no
GDI/OpenGL), UI decision (raw Win32 v1, WinForms later if needed), and
gaps vs real VLC. [SECURITY.md](SECURITY.md) documents the threat model
and mitigations (CFG/DEP/ASLR builds, per-open protocol whitelists,
forced TLS verification).

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
pause · `.` frame step · ←/→ ±10s · PgUp/PgDn ±60s · Ctrl+PgUp/PgDn
chapters · L set A-B loop points · `[`/`]` speed ±0.25× pitch-corrected
(Backspace resets) · Z/X audio delay ∓50ms · G/H subtitle delay ∓50ms ·
↑/↓ volume (to 200%, boosted in software) · M mute · V aspect
(auto/16:9/4:3/stretch/crop) · Ctrl+D debug HUD · A audio track · S subtitle track · N/P next/prev in
folder/queue · F12 snapshot to Pictures · F or double-click fullscreen ·
O open · Q quit. Seeks are frame-exact; hovering the seek slider
previews the frame at that position; the context menu picks the audio
output device and repeat/shuffle modes. Dropping several files (or an
.m3u/.m3u8 playlist, also openable and saveable) builds a queue;
keyboard media keys and the taskbar thumbnail buttons control playback,
and the taskbar button shows progress. Volume, mute, repeat/shuffle,
window placement, per-file resume positions and per-file track choices
persist across runs.
Drop a video file onto the window to play it; a same-name `.srt`/`.ass`
next to the file loads automatically. The player is single-instance
(opening another file reuses the running window; extra files enqueue),
follows Windows dark mode for its title bar, feeds the taskbar Recent
jump list, remembers fullscreen, and can register per-user "Open with"
file associations from the context menu (no admin).

Generate test files with `tests\gen-samples.ps1` (needs any ffmpeg CLI on
PATH, used only to create inputs).
