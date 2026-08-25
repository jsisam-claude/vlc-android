# Handoff — session_01DiUtHJMjAZbDEazWctdjFp ("VLC Android telemetry removal")

Written 2026-08-25. Repo `jsisam-claude/vlc-android`, branch `main`, HEAD `6f1a163`.

Companion handoff from the concurrent Windows-player session:
`handoff/2026-08-25-session-011XWXFg.md` on `claude/vlc-win64-minimal-player-do4urk`
in `jsisam-claude/vlc-light-win64`. **The two sessions share no files** — see
"Consolidation" at the end.

---

## 1. Repo and branch map

| Repo | Branch | Role | Owned by |
|---|---|---|---|
| `jsisam-claude/vlc-android` | `main` | **Source of truth for the Android fork (VLC-NG).** The app, `libvlcjni/`, `medialibrary/`, the buildsystem. | this session |
| `jsisam-claude/vlc-libs` | `main` | Vendored common sources: the VLC tree at libvlcjni's `VLC_TESTED_HASH` (+20 android patches +12 security backports), libvlcjni/medialibrary/libvlcpp sources, sqlite, and ~49 SHA-512-verified contrib tarballs. Consumed as the sibling `../vlc-libs`. | this session |
| `jsisam-claude/vlc-light-win64` | `claude/vlc-win64-minimal-player-do4urk` | The **Windows player** — a different project. Not touched by this session. | the other session |
| `jsisam-claude/vlc-light-win64` | `stale`, `claude/vlc-android-remove-telemetry-2tt0dm` | Dead Android snapshots predating the repo split. `stale` carries `MOVED.md`; the other is one commit behind it and has no marker. | nobody — awaiting deletion |

**Why this layout.** The Android fork originally lived in `vlc-light-win64`
alongside the Windows player. It was split out earlier in this session:
android → `vlc-android`, win64 → `vlc-light-win64`, shared vendored source →
`vlc-libs`. History was moved with `fast-export`/`fast-import`, tree hash
verified identical. The two leftover branches are what remains of the old
location.

### Warning for anyone working in a `vlc-light-win64` checkout

A container for this session has a **local** branch named
`claude/vlc-android-remove-telemetry-2tt0dm` that points at *win64* code
(`56156cf`), while the **remote** branch of that name holds the *Android*
snapshot. They have diverged (15 local-only / 21 remote-only) and are
different projects. All 15 local commits are already on the remote as
ancestors of `claude/vlc-win64-minimal-player-do4urk`, so nothing is
unsaved — but a stop hook reports them as "unpushed" every turn, and
satisfying it needs `git push --force`, which would destroy the Android
snapshot. **Do not force-push that branch.**

---

## 2. What this fork is

VLC for Android with all non-user-initiated network activity removed, built
from source with no prebuilt binaries anywhere in the supply chain. Interim
name **VLC-NG**; a final non-VLC name is still outstanding (VideoLAN
trademark — public distribution must not use "VLC" branding or the cone, and
the `applicationId` migration is deferred until that name exists).

### Invariants that must not regress

1. **Zero non-user-initiated network traffic.**
2. **Casting is opt-in** behind `KEY_ENABLE_CASTING` — both start *and* stop
   gated, with a re-check after the suspension point in `RendererDelegate.start()`.
3. **Remote artwork is opt-in** behind `KEY_ALLOW_REMOTE_ARTWORK` (default
   false), gated at the single choke point `HttpImageLoader.downloadBitmap`.
4. **Settings export/import blacklist is symmetric** — OpenSubtitles token and
   `safe_mode_pin` filtered at both restore write points.
5. **No binaries in the source repos.** The Apache Ant tarball was removed for
   this reason; host-tool archives are fetched at bootstrap, never committed.
6. **Dependency verification is enforced**: `gradle/verification-metadata.xml`,
   1033 components / 1860 sha256 entries, `verify-metadata=true`, zero trust
   escapes. Add a new task graph to the regeneration or it fails closed.

---

## 3. What this session did (6 commits, 33 files, +2413/−179)

| Commit | What |
|---|---|
| `f6329ec` | Dependencies and toolchain to latest stable compatible with minSdk 17 |
| `2073462` | Cap coroutines and moshi — their newer releases break minSdk 17 / this code |
| `4b01897` | Fix the TV file browser's API guards instead of suppressing them |
| `c2c7615` | Run the unit tests that actually work; close three review findings |
| `dd66572` | Document what `compile.sh` actually builds; fix its help text |
| `6f1a163` | Make `--release` actually reach the native builds, and unblock it |

### Versions now

Gradle 9.7.1 (sha256-pinned), AGP 9.3.1, Kotlin 2.4.10, KSP 2.3.11,
compileSdk 37, targetSdk 36, build-tools 37.0.0, NDK 29.0.14206865 (64-bit),
JDK 25, okhttp 4.12.0, retrofit 2.12.0, desugar 2.1.5, robolectric 4.16.1,
mockk 1.14.11, espresso 3.7.0, and the rest of the test stack at latest.

### Deliberate caps — do not "fix" these without doing the work

- **coroutines 1.8.1**, not 1.11.0. 1.9.0 made `ExecutorCoroutineDispatcher`
  (supertype of `Dispatchers.Default`/`IO`) implement `java.lang.AutoCloseable`,
  which exists only from API 19 and is not core-library-desugared. `dexdump`
  confirmed it reaches the dex verbatim — it would fail to link on the API 17/18 floor.
- **moshi 1.8.0**, not 1.15.2, pinned with a `strictly` constraint because
  converter-moshi 2.12.0 would otherwise drag 1.15.2 in. From 1.9.0 the
  reflective adapter throws for any `@kotlin.Metadata` class; the 14
  OpenSubtitles models are reflective Kotlin data classes using `@field:Json`,
  which moshi's codegen does not read. Moving forward means migrating those
  models and re-verifying OpenSubtitles end to end against the live API.
- **mockito 3.3.3** — the version PowerMock 2.0.9 itself depends on.
- **All androidx majors frozen** (core 1.12, appcompat 1.6.1, lifecycle 2.5.1,
  room 2.6.1, …). Current releases declare minSdk 21 or 23. minSdk 17 is a
  user decision.
- **targetSdk stays 36** while compileSdk is 37: compileSdk is compile-time
  only, targetSdk changes runtime behaviour.

### Real bugs found and fixed along the way

- `:application:app` and `:libvlcjni:libvlc` never received `ndkVersion`, so
  they fell back to an AGP-default NDK the project never installs.
  `stripDebugSymbols` then **failed soft** and the APK shipped unstripped
  libraries — `libc++_shared.so` 9.3 MB instead of 1.3, `libmla.so` 35.7
  instead of 7.7.
- `init_local_props` validated only `sdk.dir` and `android.ndkPath`, so a
  `local.properties` written by Android Studio (which never writes the
  VLC-specific `android.ndkFullVersion`) produced the same silent
  unstripped result via `CXX1100`.
- `compile.sh --release` never reached `compile-libvlc.sh` (`$RELEASE` is
  unexported), so release APKs shipped a `--enable-debug`, `NDK_DEBUG=1`
  libvlc. Masked by the medialibrary's `git describe` release gate, which
  aborted every release build because the vendored drop has no `.git`.
- `compile.sh` built libvlc via `--project-dir`, whose build root has no
  verification metadata. Now `:libvlcjni:libvlc` through the repo root, with
  the old invocation kept for an out-of-tree `VLC_LIBJNI_PATH`. Same fix
  applied to the CI publish jobs.
- CI cached `gradle-9.3.1/` and all of `gradle/` — the latter would restore a
  stale `verification-metadata.xml` over the checkout. Cache is now keyed and
  narrowed to `gradle/wrapper/`.
- `gradle wrapper` preserves an existing `distributionSha256Sum` while
  rewriting the URL, so the Gradle bump would have made `compile.sh` fail
  permanently on any checkout carrying the old sum. Now passed explicitly.

---

## 4. Build gotchas (all documented in BUILDING.md)

- **`./buildsystem/compile.sh -a arm64-v8a`** is the complete build.
  `-l` also sets `NO_ML`, so it builds *only* libvlc and no APK; an app built
  after it is missing `libmla.so`, and that fails silently —
  `MedialibraryImpl` catches the `UnsatisfiedLinkError`, so the app starts
  and simply never sees any media.
- **The native step runs on every invocation**, it is not skipped because the
  `.so` exist. ~17 s when nothing changed (but `libvlc.so`/`libvlcjni.so` are
  relinked every time), ~2.5 min after a reconfigure, ~9 min for contribs.
- **Gradle never compiles native code** — no module declares
  `externalNativeBuild`. `./gradlew` and Android Studio silently reuse
  whatever `compile.sh` last produced.
- **`./gradlew clean` deletes `jni/libs` and `jni/obj`** and no Gradle task can
  regenerate them; the next `assembleDev` then yields an APK with no native
  libraries, because an empty `jniLibs` dir is not an error.
- **Changing `ANDROID_NDK` does not trigger a VLC reconfigure** — delete
  `../vlc-libs/vlc/build-android-<tuple>/config.h` or VLC keeps compiling with
  the old toolchain while contribs use the new one.
- Export `ANDROID_SDK`, `ANDROID_NDK`, `JAVA_HOME` (JDK 25). The JDK is **not**
  pinned by anything in the repo.
- Never run two native builds concurrently — it corrupts the shared contrib
  tree. Concurrent Gradle invocations on the same tree also corrupt
  incremental state.
- A signed build appends local keystore paths to the **tracked**
  `gradle.properties`; do not commit those lines.

---

## 5. Not done

**Nothing here has ever run on a device or emulator.** Every claim is
build-time: compiles, lint, 48 asserting unit tests, APK assembles
(61.5 MB, four native libs). The NDK 27 → 29 jump across the whole native
stack is the largest unverified change — the sibling win64 session hit
exactly this class of failure (VP9 and MPEG-TS/AAC/AC-3 dead on arrival)
after its own vendor refresh.

Also open:

- No full **signed release APK** has been built, though the path is now unblocked.
- **32-bit ABIs unverified** — `armeabi-v7a`/`x86` need NDK 21, not installed here.
- **29 tests `@Ignore`d, 13 files excluded** (pre-existing upstream rot; net
  went from 32 to 48 asserting).
- **Lint is report-only in 7 modules.** No release gate was lost and nothing
  green was switched off, but new lint errors there will not fail a build.
- **B-series concurrency findings** reported but not fixed: `VLCDownloadManager`
  `dlDeferred` cross-talk, receiver re-registration, `FeedbackActivity` IO-thread
  UI, `HttpImageLoader` timeouts, `OpenSubtitleService` `@Volatile`,
  `MediaUtils` AppScope handler, `LiveDataset.size`.
- `BenchActivity` exported (MED); dead extensions permissions (LOW-MED);
  FeedbackUtil still references VideoLAN emails (LOW-MED).
- **Final rebrand name + `applicationId`** — blocking public distribution.
- vlc-libs has 9 untracked host-tool tarballs that are **not gitignored** — a
  stray `git add -A` there would commit ~10 MB of binaries against the
  no-binaries policy.
- vlc-libs keeps ~17 MB of disabled-contrib archives (documented, pruning deferred).
- `deb.debian.org` is blocked by egress policy, so trixie's exact
  `openjdk-25-jdk` could not be installed; verification runs used the same
  Debian source package rebuilt for Ubuntu.
- PR #1 in `vlc-light-win64` (`stale` → win64 default) must **not** be merged;
  it would replace the Windows player with the Android tree.

---

## 6. Consolidation

**These two sessions do not overlap.** This one owns `vlc-android` and
`vlc-libs`; the other owns `vlc-light-win64`. No file is written by both, so
there is no merge to perform and no conflict to resolve. The other session's
conflict warning about `tools/gen_ffmpeg_build.cmake` and the vendored
FFmpeg/HarfBuzz trees applies only inside `vlc-light-win64` — none of those
paths exist in the repos this session touches.

The single shared artifact is the `vlc-light-win64` repo itself, which still
carries the two dead Android branches. Deleting them is safe once you are
satisfied `jsisam-claude/vlc-android` is the source of truth — deletion has
been blocked by policy for both sessions, so it needs to be done by you.
