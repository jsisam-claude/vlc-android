# Security model

What this player trusts, what it doesn't, and the mitigations in place.
Scope covers the engine (`src/` behind `player.h`), the bundled Win32
shell, and the vendored FFmpeg subset; the photo-gallery embedding
inherits everything engine-side.

## Threat model

Untrusted inputs, in decreasing order of exposure:

1. **Media files** (local or received from anyone): parsed in-process by
   the vendored FFmpeg demuxers/decoders. This is the dominant attack
   surface of any media player.
2. **Network streams** (http/https/HLS, user-initiated via Open URL):
   everything in (1) plus a hostile server or on-path attacker, plus
   playlist indirection (an HLS playlist names further URLs/files).
3. **Subtitle files** (sidecars, or tracks inside containers): parsed by
   FFmpeg; rendered as text/bitmaps only — no scripting of any kind.
4. **Local IPC**: the single-instance handoff (`WM_COPYDATA`) receives
   paths from other processes in the same session.

Out of scope: attackers with code execution in the same user session
(they already own everything this process owns), and availability
attacks (a file crafted to decode slowly plays slowly).

## Mitigations

### Styled subtitles (libass stack)
- Styled ASS/SSA rendering vendors libass + FreeType + FriBidi + HarfBuzz
  as pinned-release source (tags in
  `third_party/libass-src/PROVENANCE.txt`); patch story is re-vendoring,
  same as FFmpeg. Font glyph parsing (FreeType) and subtitle script
  parsing (libass) are untrusted-input parsers and compile with the same
  `/guard:cf` hardening.
- No fontconfig, no iconv: subtitle input is UTF-8 only (FFmpeg's decoder
  output already is), and system fonts come from DirectWrite. Embedded
  fonts from a container are handed to FreeType — a font-parser exposure
  that CFG/DEP/ASLR and the release-tracking patch story cover.

### Parsing surface (FFmpeg)
- The subset is vendored from a current upstream release (n8.1.2) as
  unmodified source; the patch story is re-vendoring a newer tree
  (`tools/vendor_ffmpeg.cmake` + the generator), one command each.
- Compiled with **Control Flow Guard** (`/guard:cf`) — FFmpeg dispatches
  through large function-pointer tables, exactly what CFG protects —
  plus DEP/ASLR (`/NXCOMPAT /DYNAMICBASE`) and `/GS` stack cookies on
  every target. The engine and shell additionally build with `/sdl`.
- Heap corruption terminates the process
  (`HeapEnableTerminationOnCorruption`).
- Demux queues are byte-bounded (16 MB) and the frame queues
  entry-bounded, so a hostile file cannot balloon memory through the
  pipeline.
- Muxers, encoders, filters and all unused protocols are not compiled
  in at all — the attack surface is the ~60 components the player
  actually uses, not FFmpeg's full set.

### Network
- **Per-open protocol whitelists** (engine-enforced): local files open
  with `file,crypto,data` — a hostile local playlist can never trigger
  network I/O; URLs open with `http,https,tcp,tls,udp,crypto,data` — a
  hostile stream can never read local files.
- **TLS certificate verification is forced on** for https. This matters:
  libavformat 62 still ships with verification off by default
  (`FF_API_NO_DEFAULT_TLS_VERIFY`); the engine overrides it per open.
  TLS itself is Windows Schannel — cipher policy and certificate roots
  are OS-maintained.
- HLS keeps FFmpeg's built-in guard: playlist entries pointing at local
  files are rejected unless they carry a common media extension.
- Network I/O only ever happens for a URL the user explicitly entered;
  nothing in a local file can initiate a connection.
- No telemetry, no update checks, no connections other than the stream
  the user asked for.

### Shell & persistence
- `WM_COPYDATA` handoff: size-bounded (8 KB), length-derived string (no
  NUL trust), and the payload is treated exactly like a user-opened
  path — same-session senders already run as the user.
- File associations are written under **HKCU only** (never elevates,
  `asInvoker` manifest); state lives in `%APPDATA%\minimal-player\`
  and is parsed with fixed-size bounded reads.
- Snapshot filenames derived from media names/URLs are sanitized before
  use.

## Reporting

Open an issue in this repository. Include the file or URL that triggers
the problem if it can be shared.
