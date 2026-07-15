# Plan: Lightweight win64 video player — MSVC only, embedded deps, no external libs

## Constraints (agreed)

- **win64 only** (x64, Windows 10+)
- **Visual Studio Enterprise tooling only** — anything that ships in the
  VS installer is allowed: MSVC, clang-cl/LLVM toolset, CMake, Ninja,
  vcpkg, MFC, WPF/WinForms. **No mingw, no MSYS, no WSL.**
- **No external library dependencies** — media code is *embedded in this
  repo* (vendored FFmpeg source, statically linked) and reuses code from
  libvlc/mpv/Microsoft samples where applicable; the shipped player is a
  single exe with no third-party DLLs
- Plays local **.mp4, .mkv, .webm, .avi** with common codecs + **subtitles**
  (embedded and external .srt/.ass)
- Minimal code we write ourselves — prefer battle-tested open-source code
  and OS-provided facilities over novel code
- No streaming, no transcoding, no DVD/Blu-ray; **no Qt ever**

## Where the code comes from (reuse strategy)

| Layer | Source | How |
|---|---|---|
| Demux + decode | **FFmpeg** (latest release branch), vendored as a git submodule under `third_party/ffmpeg` | Compiled by MSVC/clang-cl, statically linked. Same engine libvlc uses internally — embedding FFmpeg *is* embedding the part of VLC that does the heavy lifting. |
| Player structure (sync/seek/queues) | **`fftools/ffplay.c`** in the same FFmpeg tree | The canonical ~3,700-line reference player; we mirror its clock, queue, and seek logic while replacing its SDL frontend with Win32/WASAPI/D3D11. License-free reuse — it's in the tree we already vendor. |
| Video render | **Windows itself: `ID3D11VideoProcessor`** + Microsoft's MIT-licensed `DX11VideoRenderer` sample as setup reference | The GPU driver does NV12/P010→RGB, colorspace, scaling, **and deinterlacing**. Zero-copy from D3D11VA decode textures. Almost no novel GPU code. |
| Audio output (WASAPI) | **mpv** `audio/out/ao_wasapi.c` (primary) and **libvlc** `modules/audio_output/mmdevice.c` (cross-reference) | Adapted into `src/audio_out.c`, module scaffolding stripped. Both LGPL. |
| Subtitle handling | **libvlc** `modules/demux/subtitle.c` heuristics (encoding detection, sidecar discovery) + FFmpeg's srt/ass/mov_text decoders | Text rendering via Windows DirectWrite; full ASS styling later only if DirectWrite proves insufficient. |
| Player logic we actually write | **ours** (~1–2k lines C/C++) | Window/hotkeys shell, wiring between the adapted pieces, track selection. |

**Licensing:** FFmpeg, libvlc, and mpv code are LGPL-2.1+, so this repo is
LGPL-2.1+; static linking is fine because the vendored sources live in the
repo. Microsoft samples are MIT (compatible). No GPL-only FFmpeg features
(`--enable-gpl` stays off), and **no code from GPL players** (MPC-HC/BE,
MPC Video Renderer) — reference reading only, or the whole project flips
to GPL.

## The one hard build problem, solved up front

FFmpeg's build uses a `configure` shell script; MSVC/clang-cl are fully
supported *compilers* for it, but the script needs a POSIX shell. With
MSYS banned, in order of preference:

1. **vcpkg in manifest mode, static triplet** (`x64-windows-static`) —
   vcpkg *ships inside the VS 2022 installer*, so this stays within
   "tooling that comes with Visual Studio". `cmake --preset x64-release`
   has vcpkg build our *vendored* FFmpeg checkout (overlay port pointing
   at `third_party/ffmpeg`) and link it statically. vcpkg internally
   fetches a private msys2 subprocess to run configure and nasm for the
   SIMD asm — automatic, never installed system-wide, never touched by
   you. Output is `.lib` files in the build tree.
2. If a hidden shell is still unacceptable: a one-time
   `tools/build-ffmpeg.ps1` that runs configure via the `busybox-w32`
   single-binary shell (one ~700 KB exe checked into `tools/`, no
   installation, no environment). Same compilers, same static output.
3. Never: system MSYS2, mingw toolchains, or prebuilt FFmpeg/libvlc DLLs.

**Compiler choice:** prefer **clang-cl** (LLVM toolset in the VS
installer) for the FFmpeg objects — it handles FFmpeg's hot paths better —
while player code stays plain MSVC; they link together freely. Escape
hatch for asm/tooling friction: `--disable-x86asm` (slower, functional).

FFmpeg is built **once per FFmpeg version bump**, `.lib` outputs cached;
day-to-day development is pure Visual Studio: open folder, F5.

**Bring-up sequencing note:** phase 1 uses the *official* vcpkg ffmpeg
port (features: avcodec/avformat/swresample/swscale, full decoder set
within those libs) instead of the trimmed custom configure below — it is
the battle-tested MSVC build path and maximizes the chance the first
build succeeds. The `--disable-everything` trim moves to the size-trim
phase as an overlay port; until then the exe is larger (~30–60 MB) but
functionally identical.

FFmpeg configure line (trimmed, static, LGPL):

```
--toolchain=msvc --arch=x86_64 --disable-everything --disable-programs
--disable-doc --disable-network --disable-avdevice --disable-avfilter
--enable-static --disable-shared
--enable-demuxer=mov,matroska,avi --enable-protocol=file
--enable-decoder=h264,hevc,mpeg2video,mpeg4,msmpeg4v3,vp8,vp9,
  aac,mp3,ac3,eac3,flac,vorbis,opus,pcm_s16le,pcm_s24le,
  subrip,ass,ssa,mov_text,pgssub,dvdsub
--enable-parser=h264,hevc,mpegvideo,mpeg4video,mpegaudio,aac,ac3,flac,vp8,vp9
--enable-d3d11va --enable-dxva2
```

Notes: `avfilter` stays disabled — deinterlacing is done by the driver via
`ID3D11VideoProcessor`, not yadif. `pgssub`/`dvdsub` included so bitmap
subtitles (Blu-ray remuxes, old AVIs) work. Final exe target: **~8–15 MB**.

## Video pipeline: D3D11 from day one, no GDI, no OpenGL

The renderer choice decides the decode pipeline: D3D11VA outputs GPU
textures, and only a D3D11 renderer consumes them zero-copy. GDI would
force CPU readback (killing hw decode); OpenGL on Windows means fragile
`WGL_NV_DX_interop` hacks, worse compositor/vsync behavior, bad Intel
drivers, no HDR. D3D9 is legacy; D3D12/Vulkan are boilerplate for no
player-visible gain at this scale.

**Primary path (mostly OS-provided):**

```
D3D11VA decode → ID3D11VideoProcessor blit (NV12→RGB, scale, colorspace,
deinterlace — all in driver) → DXGI flip-model swapchain
```

**Staged bring-up** (each stage shippable, earlier stages remain as
permanent fallbacks, not throwaway):

1. *CPU path:* software decode → swscale→BGRA → upload one texture → draw
   quad. ~Days of work; stays forever as the fallback for non-hw-decodable
   codecs and WARP/VM environments, and as the known-good baseline for
   bisecting render bugs. Only the ~30 swscale lines are disposable.
2. *VideoProcessor path:* hw decode textures through
   `ID3D11VideoProcessor`. The main path.
3. *Custom NV12 shader:* only if a driver misbehaves or quality control is
   ever needed (mpv-style). Not scheduled; contingency.

Window/swapchain/present code is identical across stages — only the
texture format and blit mechanism change.

## UI: raw Win32 now; WinForms if ever needed; not WPF/MFC/Qt

- **Raw Win32 (v1):** one window, drag-and-drop, ~8 hotkeys ≈ 150 lines in
  the same codebase. No interop, no .NET dependency, exe stays standalone.
- **WinForms (if a richer UI is wanted later):** every control has a real
  HWND for the engine to render into; engine exposed as a C API DLL,
  P/Invoked. Fast for playlists/settings/menus. Costs .NET runtime +
  two-language repo.
- **WPF (avoid for video):** controls have no HWND; `HwndHost` airspace
  restrictions or laggy `D3DImage` copies. Only wins for skinned UIs —
  out of scope.
- **MFC (allowed, not worth it):** framework ceremony over raw Win32 for
  this scope. **Qt:** the build-pain source we're escaping.

Engine is designed behind a C API (`player_open/pause/seek/tracks/...`)
from day one so a WinForms shell can be bolted on without touching it.

Hotkeys: Space=pause, ←/→=±10s, ↑/↓=volume, `S`=cycle subtitle track,
`A`=cycle audio track, `F`=fullscreen, `Esc`/`Q`=quit. Sidecar `.srt`/`.ass`
next to the opened file loads automatically.

## Source layout

```
/vcpkg.json               manifest + pinned baseline
/CMakeLists.txt           VS opens the folder natively; vcpkg toolchain
/CMakePresets.json        x64-windows-static preset so F5 just works
/src/
  main.c         args, WM_DROPFILES, hotkey map, message loop
  demux.c        avformat: open, track enumeration/selection, reader thread
  decode.c       avcodec threads, packet/frame queues   [ffplay-informed]
  clock.c        audio-master A/V sync, pause, seek+flush [ffplay-informed]
  video_out.c    D3D11 window/swapchain; CPU + VideoProcessor paths
  audio_out.c    WASAPI shared mode                     [from mpv ao_wasapi]
  subs.c         text subs via avcodec + DirectWrite; bitmap subs blit;
                 sidecar discovery                      [libvlc heuristics]
  queue.c        small thread-safe packet/frame queues
/third_party/ffmpeg/         git submodule (latest release branch)
/third_party/vlc-excerpts/   adapted libvlc/mpv files, provenance headers
/tools/                      build-ffmpeg.ps1 fallback, busybox-w32
/tests/                      test-matrix scripts + sample file manifest
```

## Phases & schedule (≈2–2.5 weeks to daily-usable)

1. **Build bring-up** *(1–2 days, the risk item — do first)* — FFmpeg
   submodule + overlay port builds static `.lib`s on a clean VS 2022
   machine; CMake preset; F5 runs a stub exe.
2. **Video on screen** *(2–3 days)* — window, demux→decode→CPU-path
   present. Playable-without-audio build inside week one.
3. **Audio + sync + controls** *(3–5 days, the hard part)* — WASAPI port,
   audio-master clock, pause/seek/volume/track cycling, drag-and-drop.
   ffplay's structure keeps this from ballooning.
4. **VideoProcessor + hw decode** *(2–3 days)* — D3D11VA →
   `ID3D11VideoProcessor` zero-copy path; driver deinterlacing.
5. **Subtitles** *(2–4 days)* — embedded text + sidecar .srt/.ass via
   DirectWrite; PGS/VobSub bitmap blit; evaluate whether ASS styling needs
   more than DirectWrite.
6. **Optional later** — chapters, resume-position, playback speed
   (scaletempo port), HDR tone mapping, AV1 (dav1d), WinForms shell.

## Gaps vs real VLC (accepted, with mitigations)

Inside our scope, where VLC's 20 years show:

- **Format breadth:** VLC plays anything (.ts, .flv, .wmv, damaged files);
  we know exactly mp4/mkv/webm/avi + listed codecs. Each new format is one
  configure line away, but "any file ever" is gone by design. **AV1**
  absent until dav1d/FFmpeg decoder is enabled.
- **Broken-file resilience:** libavformat recovers a lot; VLC adds extra
  heuristics on top. Some half-downloaded files VLC limps through will
  fail here.
- **ASS subtitle styling:** DirectWrite renders text but not full ASS
  positioning/karaoke/animation. Gap until/unless libass-level rendering
  is added (FFmpeg's hookup or DirectWrite improvements).
- **HDR:** plays but tone mapping is driver-dependent via VideoProcessor;
  VLC does explicit tone mapping. Revisit in phase 6.
- **A/V sync edge cases:** broken timestamps, VFR, long-session drift —
  VLC handles these maturely; we fix reactively as real files expose them.
- **Audio features:** no equalizer, no AC-3/DTS passthrough, simpler
  downmix, no pitch-preserving speed until scaletempo is ported.
- **Conveniences:** no playlists, chapters, resume-position, frame step,
  snapshots, A-B loop, settings UI, file associations (phase 6 picks the
  valuable ones; chapters + resume first).

By deliberate design (not coming back): network streaming, discs,
transcode/stream-out, casting, capture, lua/extensions/skins,
visualizations, translations.

## Rejected alternatives (for the record)

- **Building VLC/libvlc with MSVC** — impossible; upstream is GCC/mingw-only
  and its contrib deps don't compile under MSVC. We reuse its *source*.
- **Prebuilt libvlc or FFmpeg DLLs** — violates "no external libs".
- **mingw/MSYS/WSL cross-compile** — ruled out by constraints.
- **OpenGL renderer** — interop hacks for hw decode, worse vsync/drivers/
  HDR on Windows; only wins portability we don't need. **GDI-first** —
  forces CPU readback, defers hw decode, guarantees a rewrite.
- **Big-bang final pipeline** (no staged bring-up) — 2–3 weeks dark, first
  black screen has five unproven suspects; only rational when porting a
  renderer wholesale, and extraction cost beats rewrite cost here.
- **Vendor SDL + adapt ffplay wholesale** — fastest to first playback but
  CPU-upload only (no zero-copy hw decode), weaker subtitles, and rebuilds
  the temporary-renderer problem one level up. We take ffplay's *logic*,
  not its SDL frontend.
- **GPL renderers (MPC-HC/BE, MPC Video Renderer)** — best open-source
  D3D11 renderers on Windows, but GPL (would relicense the project) and
  DirectShow-entangled. Reference reading only. madVR: closed.
- **libplacebo** — external library; mpv-quality rendering but against
  constraints, and `ID3D11VideoProcessor` covers our needs.
- **Media Foundation end-to-end** — patchy MKV/subtitle support, HEVC
  needs the paid Store codec.

## Test matrix

| File | Verifies |
|---|---|
| H.264 + AAC in .mp4 | the 90% case |
| H.265 + AC-3 in .mkv | HEVC, MKV demux, hw decode |
| VP9 + Opus in .webm | WebM path |
| Xvid + MP3 in .avi | legacy AVI |
| .mkv with embedded ASS subs | subtitle rendering limits (documented) |
| .mkv with PGS bitmap subs | bitmap subtitle blit |
| .mp4 + sidecar .srt (UTF-8 and CP1250) | external subs, encoding detection |
| 1080i MPEG-2 sample | VideoProcessor driver deinterlacing |
| 4K HEVC 10-bit | zero-copy hw decode path, P010 |
| Seek/pause/track-switch on each | controls, flush correctness |

## Order of work

1. Commit this plan. *(done)*
2. Phase 1 build bring-up — prove the vendored FFmpeg static build on a
   clean machine before writing player code.
3. Phases 2–5 in order; each phase ends with the test matrix rerun.
4. Pick phase-6 items by feel on real files.
