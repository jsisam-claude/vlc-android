# Handoff — 2026-09-11 — first run on real hardware

The fork ran on a physical device for the first time in the project's history.
Three defects surfaced and are fixed in this commit. One symptom is still open.

Read this with [BUILDING.md](../BUILDING.md) (how to build) and
[CONTEXT.md](../CONTEXT.md) (invariants). The 2026-09-06 handoff covers the AGP
bump and the branch-cleanup that is still blocked.

---

## 1. What actually happened

The build ran on a Debian host against a network mount. In order:

1. **Meson aborted on clock skew.** `fribidi` was the first contrib to use
   meson, and meson makes a future-dated `coredata.dat` fatal with no
   threshold and no override. The mount's timestamps came from a machine
   ~0.5 s ahead of the builder. Not a project bug. Eight contribs use meson
   (bluray, dav1d, freetype2, fribidi, harfbuzz, libdsm, librist, microdns),
   so skipping one only moves the failure. Autotools contribs only *warn*,
   which is why everything before fribidi passed on the same mount.
2. **The APK built, installed and launched.**
3. **It crashed immediately** with `IllegalStateException: can't create LibVLC
   instance`. Root cause in §2.1.
4. Past that, **the media list populated** — so the medialibrary native lib
   loaded, scanned storage and produced real entries; storage permission and
   the URIs are fine.
5. **Tapping an item plays nothing.** No video, no audio, the loading cone
   spins indefinitely, no error dialog. Still open — §3.
6. **Stock VLC plays the same file on the same device.** So the device
   delivers surfaces normally, mediacodec works, and the media is playable.

## 2. Fixed in this commit

### 2.1 Options for modules this fork does not build (the crash)

`libvlc`'s second `config_LoadCmdLine` pass (`src/libvlc.c:161`) treats a
command-line option that **no loaded module defines** as fatal: `libvlc_new()`
returns NULL and the JNI layer throws. The diagnostic is written with `fputs`
to stderr, which Android discards, and the early log buffer is dropped by
`vlc_LogDeinit` before the Android logger is ever attached. **The failure is
completely silent — the absence of any `VLC`-tagged logcat line is itself the
fingerprint.**

Contrib pruning removes modules, which removes the options they define, while
the app layer is upstream code that keeps passing them. Two had shipped:

| Option | Only definer | Disabled by |
|---|---|---|
| `--hrtf-file` | spatialaudio (`modules/audio_filter/channel_mixer/spatialaudio.cpp:89`) | `aa07a05` (`--disable-spatialaudio`, `--disable-mysofa`) |
| `--soundfont` | fluidsynth (`modules/codec/fluidsynth.c:76`; `audiotoolbox_midi.c` is Apple-only) | `--disable-fluidsynth`, `--disable-fluidlite` |

`--hrtf-file` was emitted unconditionally on the phone UI, so it crashed every
launch. `--soundfont` only fires once a user picks a MIDI soundfont, so it was
latent. Both emissions are now removed from `VLCOptions.kt`, each replaced by a
comment naming the module and the reason, so re-enabling is a deliberate act.

**This class is now a build failure, not an app failure.**
`tools/check-libvlc-options.py` cross-references every `--option` literal the
app can emit against the options defined by the core plus the modules actually
linked into the generated `libvlcjni-modules.c`. `compile.sh` runs it right
after `compile-libvlc.sh`, which is the first moment the module list exists.

Its failure mode is deliberately narrow: an option is an **error** only when it
resolves to definers that are all unbuilt — the exact signature of this bug. An
option it cannot resolve, or one whose defining file it cannot attribute to a
plugin, is a **warning** that never fails the build, so a new `add_*` macro
spelling degrades to noise rather than a broken build.

Verified both ways against the current tree: 44 options emitted, 496 modules in
the fixture, 1784 options defined; **0 errors and 0 warnings** on the fixed
source, and on the pre-fix source exactly `--hrtf-file` → spatialaudio and
`--soundfont` → fluidsynth, with no false positives.

One parsing subtlety worth keeping: VLC's per-directory `Makefile.am` files are
`include`d from `modules/Makefile.am`, so paths inside them are relative to
`modules/`, not to the fragment's own directory — `modules/codec/Makefile.am`
says `codec/subsdec.c`. Resolving against the fragment's directory maps nothing
and makes every such module look unowned (24 false positives on the first pass).
The script now tries each ancestor and keeps the one that exists on disk.

### 2.2 `libvlcjni/vlc` is a per-clone step, not a committed one

`libvlcjni/vlc` is a symlink to the vlc-libs checkout, created by
`tools/vendor-videolan.sh` — and it is gitignored (`.gitignore:3`), so **it does
not exist in a fresh clone**. BUILDING.md listed it under "already executed and
committed", which cannot be true of a gitignored symlink. When it is absent,
two things go wrong silently:

- libvlcjni's `get-vlc.sh` **clones VLC from code.videolan.org**, breaking the
  no-downloads contract.
- The Gradle lua asset copies in `libvlcjni/libvlc/build.gradle` become
  **NO-SOURCE**: Gradle skips them and the build still succeeds, so the AAR
  ships with no assets at all.

`compile.sh` now links it before the fetch step and **refuses to fall through to
the network clone** (`ALLOW_VLC_CLONE=1` overrides). It accepts either the
documented sibling layout or a nested `vlc-libs/`, since both occur in
practice, exported as `VLC_LIBS_DIR`; the contrib-tarball auto-detect uses the
same variable. Exercised under `dash` against sibling, nested, missing and
opt-in layouts, plus the already-linked no-op.

The three lua copy tasks now resolve their sources through a `requireVlcShare`
helper that throws a `GradleException` naming the fix, so a missing symlink is
a loud failure instead of an assets-free AAR. They are wired to every `merge*`
and `lint*` task in that module (`build.gradle:78-83`), so they do run on any
real build.

### 2.3 `compile.sh` environment defaults

From the previous commit, recorded here for completeness: `JAVA_HOME`,
`ANDROID_SDK` (from `ANDROID_HOME`) and `ANDROID_NDK` (when exactly one is
installed under `$ANDROID_SDK/ndk/`) are now defaulted, so an Android Studio
layout builds with only `ANDROID_HOME` exported.

## 3. Open: playback never starts

**Symptom.** Tap an item, the player opens, nothing plays, the cone spins
forever, no error dialog.

**What the cone means.** `startLoading()` is posted 1 s after the player opens
and is cleared **only** by `MediaPlayer.Event.Playing` (→ `onPlaying()` →
`stopLoading()`) or `Event.Buffering` at `100f`.

**Eliminated by evidence, not by guesswork:**

- **Any libvlc error.** A tap on a video row builds a *single-item* queue. On
  `EncounteredError`, `PlaylistManager.kt:1259` runs `next()`, which falls off
  the end (`:374-378`) into `stop()`, which broadcasts `EXIT_PLAYER` (`:416`),
  which `VideoPlayerActivity.kt:517` turns into `exitOK()`. **A cone still
  spinning after ten seconds proves no `EncounteredError` fired.** That
  eliminates the entire missing-module / missing-codec family as the cause of
  *this* symptom — those fail in well under a second and close the activity.
- **A dead Kotlin event bridge.** Video is blitted natively onto the Surface
  and audio written natively by `android_audiotrack`; no Kotlin sits in either
  loop. Had libvlc reached Playing, there would be picture and sound *under* a
  spinning cone.
- **A vout-module hole.** Decoders are built in `InitPrograms()`
  (`input.c:1398`) and `input_ChangeState(PLAYING_S)` runs unconditionally
  after (`:1437`); a vout that cannot be created is non-fatal and falls back.
  That signature is audio-with-black-screen, cone already cleared.
- **Patch integrity.** All 20 android patches verified present in the vendored
  tree, no `.rej` or `.orig` files. (`vendor-vlc.sh` uses `patch --force`, so
  this was worth checking.)
- **The platform.** Stock VLC plays the same file on the same device.

**The two surviving shapes** — either `nativePlay()` was never issued, or the
input thread is blocked before `input.c:1437`:

1. **The two-surface gate.** `MediaPlayer.java:769` — `play()` sets
   `mPlayRequested` and returns early while `mWindow.areSurfacesWaiting()`.
   The only re-arm (`:458`) needs **both** the video and the 1dp×1dp subtitles
   `SurfaceView` ready (`AWindow.java:380`). If either never arrives, or
   arrives and is destroyed once, `onSurfaceDestroyed()` → `detachViews()`
   resets to INIT and releases the holder callback (`:344-354`), and
   `VideoPlayerActivity.kt:1038` (`if (playbackStarted) return`) blocks any
   re-attach. libvlc is then never told to play: no input thread, no ES, no
   error. This is the only mechanism producing *zero* libvlc activity.
2. **The mediacodec direct-rendering handshake.** `mediacodec-dr` defaults true
   (`mediacodec.c:187`), so `mediacodec.c:770` calls `UpdateVout()` *inside*
   decoder open — i.e. inside `InitPrograms()`, strictly before `PLAYING_S`.
   A block in that handshake freezes the input thread with no event and no
   error. Note this ordering also excludes the ordinary "hardware decoder
   hangs" story, which would happen *after* `PLAYING_S` with the cone gone.

### Next steps, cheapest first

Two in-app experiments need no rebuild and no adb, and together they pin it:

1. **Settings → Video → Hardware acceleration → Disabled.** That takes
   `VLCOptions.kt:302` down `setHWDecoderEnabled(false, false)`, removing
   mediacodec and the opaque vout entirely, so candidate 2's handshake never
   runs. If it then plays, it is candidate 2.
2. **Play an mp3.** Audio never attaches a video surface, so it skips the
   `areSurfacesWaiting()` gate and the whole `AWindow` handshake. If the mp3
   plays, the fault is confined to the video path (candidate 1). If the mp3
   also hangs, both candidates are wrong and the problem is upstream of video
   entirely.

The decisive artifact is still one logcat capture. `-vv` is on by default
(`VLCOptions.kt:111`, `KEY_ENABLE_VERBOSE_MODE` defaults **true**, emitted at
`:204`) and the android logger plugin is built and **not** blacklisted — the
`logger` entry in `VLC_MODULE_BLACKLIST` only matches `liblogger_plugin.a`, not
`libandroid_logger_plugin.a`. So the useful tag is **`VLC`**
(`modules/logger/android.c:67`); `VLC-std` only carries raw stdout/stderr.

```sh
adb logcat -c
# tap ONE video, let the cone spin ~15 s, leave it open:
adb shell dumpsys SurfaceFlinger --list | grep -i videolan
adb logcat -d -v threadtime > vlc.log
grep -nE "VLC Options:|creating input thread|using demux module|looking for (vout|audio output|video decoder)|Opaque Vout|no .* module matched|cannot " vlc.log | tail -40
```

Read the last grep **bottom-up: the last `VLC` line names the stage that never
completed.**

| Log shows | Cause | Next |
|---|---|---|
| `VLC Options:` but no `creating input thread` | `nativePlay()` never issued | SurfaceFlinger line: two layers → attach was skipped; no layers → surfaces never arrived (candidate 1) |
| `using demux module`, then the `VLC` tag stops on a `vout`/`mediacodec`/`Opaque Vout` line | input thread blocked in the handshake (candidate 2) | confirm with the software-decoding toggle |
| `EncounteredError` **and the activity closed** | ordinary open failure, not a stall | fix the named module or URI |
| reaches `PLAYING_S` but the cone stays | only then is the event-bridge family live | `PlayerController.kt:320`, `PlaylistManager.kt:1204` |

## 4. Repo state

All four repos are on `claude/new-session-0pkknw`, which equals `main` (or the
win64 default branch), trees clean.

| Repo | Role | Health |
|---|---|---|
| `vlc-light-win64` | Windows player, vendored FFmpeg n9.0.1 + libass stack | good: CI green, single build guide, reproducible |
| `media-gallery` | Windows gallery, byte-identical mirror of the engine | good: CI green |
| `vlc-android` | this fork | builds and launches; playback open |
| `vlc-libs` | passive source supplier for vlc-android | good |

**Honest cost note.** The two Windows repos are self-contained, CI-covered and
documented. The Android side is a VLC fork with ~49 vendored contribs, **no
CI**, one hand-driven build host, and nothing had ever run on a device until
today. Every defect found today was found within minutes of having hardware.
The absence of a feedback loop, not the defects, is the structural problem.

## 5. Open items

- **Playback stall** — §3. The blocker.
- **No CI for Android.** The other three repos have it.
- **Branch deletion still blocked.** The git proxy returns HTTP 403 on
  `receive-pack` for ref deletion. Four dead branches remain: win64 `stale`
  (`347281f`) and `claude/vlc-android-remove-telemetry-2tt0dm` (`d6bc347`);
  gallery `claude/embed-video-player` (`fbc50e1`) and
  `claude/zealous-einstein-y8nak8` (`6396501`). After they go, drop the
  "Known false positive" section from CONTEXT.md.
- **Kotlin 2.4.20** is out; recommended hold (KSP must move with it).
- **`handoff/` should be trimmed** before these repos are made public.
- Win64 default-branch naming, and the final non-VLC naming pass.

## 6. Conventions that still hold

- Develop on `claude/new-session-0pkknw`; fast-forward the default branch.
- Never `git add -A` in `vlc-libs` (untracked host-tool tarballs; no-binaries
  policy). Never force-push. Never route around the 403 on ref deletion.
- Commits are authored as `Claude <noreply@anthropic.com>`. **The repository
  owner's personal email must not appear in git** — see §7.

## 7. Email in git history

The owner asked that their personal email not appear in git. Going forward it
does not: this and all future commits use `Claude <noreply@anthropic.com>`,
which is the configured default.

**One existing commit still carries it** in its author field:
`9924835` in `vlc-android` (`compile.sh: default JAVA_HOME, ANDROID_SDK and
ANDROID_NDK…`), already pushed to both `claude/new-session-0pkknw` and `main`.
No tracked file content in any of the four repos contains the address, and no
commit message does.

Removing it from that commit means **rewriting history and force-pushing**,
which has not been done and should not be done casually: it changes every
subsequent SHA on both branches and breaks anyone's existing checkout. It is a
deliberate decision for the owner to make. The `jsisam-claude` GitHub account
name does appear in repository URLs; that is the account that owns the repos,
not the email, and removing it would break the links.
