# Changelog

All notable changes to this project. Versions are milestone tags on the
development branch; each groups several development cycles.

## [1.0.0] — release candidate

The first tagged release: a self-contained, source-only Windows video and
music player, plus the embeddable engine behind `player.h`.

### Playback
- Formats: mp4/mkv/webm/avi, ts/m2ts, flv, wmv/asf, ogv, mpg/vob, 3gp;
  audio mp3/flac/m4a/ogg/wav/wma/opus/aac/ac3/mka.
- Codecs: H.264, HEVC, MPEG-1/2/4 (DivX/Xvid), VC-1/WMV, VP6/8/9, Theora,
  MJPEG, ProRes, AV1 (hardware-decode only); AAC, AC-3, E-AC-3, DTS,
  TrueHD, MP2/MP3, FLAC, Vorbis, Opus, ALAC, WMA.
- Hardware decode via D3D11VA (H.264/HEVC/MPEG-2/VP9/VC-1/AV1) with
  transparent software fallback; a menu toggle forces software.
- Rendering: D3D11 + ID3D11VideoProcessor. Display-matrix rotation,
  10-bit P010, BT.2020/PQ/HLG HDR signalling, driver deinterlacing,
  ProcAmp picture controls, aspect/zoom modes, a Ctrl+D debug HUD.
- Audio: WASAPI shared-mode with device-loss recovery and a runtime
  device picker; pitch-corrected speed (WSOLA) 0.25–4×; volume to 200%
  with a soft limiter; audio/subtitle delay correction.

### Subtitles
- srt/mov_text (text), PGS/VobSub (bitmap), and full styled ASS/SSA via
  vendored libass + FreeType + FriBidi + HarfBuzz (positioning, karaoke,
  embedded fonts). Sidecar auto-load with encoding detection; size
  setting; per-file track memory.

### Streaming
- http/https and HLS (`.m3u8`, incl. AES-128 segments) via Open URL, TLS
  through Windows Schannel with certificate verification forced on;
  buffering indicator.

### Music
- Cover-art display, a live spectrum visualization, and title/artist
  from tags for audio-only files.

### Shell & integration
- Playlists (multi-drop / .m3u load-save, repeat, shuffle), exact seek
  with hover frame previews, A-B loop, frame step (both directions),
  J/K/L shuttle, chapters, snapshots (PNG via WIC), single-instance
  handoff, per-user file associations, taskbar progress + thumb buttons +
  jump list, media keys, dark title bar, fullscreen monitor picker,
  persisted state (resume, tracks, volume, window, fullscreen).

### Build & security
- Fully self-contained: FFmpeg (1117 files) and the libass stack vendored
  as committed source, compiled by MSVC alone — no package manager, no
  submodules, no downloads, no prebuilt binaries; FFmpeg's configure and
  the libraries' build systems never run (config derived/hand-authored as
  committed text). One CMake build produces both the player and the
  gallery embedding.
- Hardened: CFG/DEP/ASLR, `/sdl`, per-open protocol whitelists, forced
  TLS verification. See [SECURITY.md](SECURITY.md).
