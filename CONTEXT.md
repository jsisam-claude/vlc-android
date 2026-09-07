# VLC-NG (Android) — context and working instructions

A privacy fork of VLC for Android: every non-user-initiated network path
removed, built entirely from vendored source with no prebuilt binaries in the
supply chain. `VLC-NG` is an interim name — a final non-VLC name is still
outstanding, because VideoLAN's trademark means public distribution must not
use "VLC" branding or the cone. The `applicationId` migration is deferred
until that name exists.

## Repo layout

| Repo | Role |
|---|---|
| `vlc-android` (this one, `main`) | The app, `libvlcjni/`, `medialibrary/`, the buildsystem. Source of truth. |
| `vlc-libs` (`main`) | Vendored common source, consumed as the sibling `../vlc-libs`. Passive supplier — the dependency is strictly one-way. |
| `vlc-light-win64` | A *different project* (a Windows player). It still holds two dead Android branches, `stale` and `claude/vlc-android-remove-telemetry-2tt0dm`, left over from before the repo split. Do not push to them. |

## Invariants — do not regress these

1. **Zero non-user-initiated network traffic.** This is the entire point of the fork.
2. **Casting is opt-in** behind `KEY_ENABLE_CASTING` — both start *and* stop
   gated, with a re-check after the suspension point in `RendererDelegate.start()`.
3. **Remote artwork is opt-in** behind `KEY_ALLOW_REMOTE_ARTWORK` (default
   false), gated at the single choke point `HttpImageLoader.downloadBitmap`.
4. **Settings export/import blacklist is symmetric** — the OpenSubtitles token
   and `safe_mode_pin` are filtered at *both* restore write points.
5. **No binaries in the source repos.** An Apache Ant tarball was removed for
   this reason. Host-tool archives are fetched at bootstrap and never committed.
6. **Dependency verification is enforced.** `gradle/verification-metadata.xml`
   has `verify-metadata=true` and zero trust escapes. It is generated across
   the `assembleDev` *and* `lintDev` graphs plus the androidTest and
   leakcanary classpaths — **add a new task graph to the regeneration or it
   fails closed.**

When touching anything in 1-4, prove the invariant still holds rather than
assuming; each has been broken at least once before.

## Building

```sh
export ANDROID_SDK=... ANDROID_NDK=... JAVA_HOME=...   # JDK 25; or just
                                                        # ANDROID_HOME with one
                                                        # NDK installed under it
./buildsystem/compile.sh -a arm64-v8a                   # complete build
```

Read `BUILDING.md` before running anything else. The traps that cost real time:

- **`-l` also sets `NO_ML`** — it builds only libvlc and produces no APK. An
  app built after it is missing `libmla.so`, which fails *silently*:
  `MedialibraryImpl` catches the `UnsatisfiedLinkError`, so the app starts and
  simply never sees any media.
- **The native step runs on every invocation.** It is not skipped because the
  `.so` files exist (~17 s when nothing changed).
- **Gradle never compiles native code** — no module declares
  `externalNativeBuild`. `./gradlew` and Android Studio silently reuse
  whatever `compile.sh` last produced.
- **`./gradlew clean` deletes `jni/libs` and `jni/obj`**, and no Gradle task
  can regenerate them. The next `assembleDev` then yields an APK with no
  native libraries, because an empty `jniLibs` directory is not an error.
- **Changing `ANDROID_NDK` does not trigger a VLC reconfigure** — delete
  `../vlc-libs/vlc/build-android-<tuple>/config.h` first, or VLC keeps
  compiling with the old toolchain while contribs use the new one.
- **Never run two native builds concurrently** — it corrupts the shared
  contrib tree. Concurrent Gradle invocations on the same tree corrupt
  incremental state too.
- Only `arm64-v8a` and `x86_64` build with NDK 29; 32-bit ABIs need exactly
  NDK 21, per the gate in `compile-libvlc.sh`.

## Version policy

`minSdk` is **17** by explicit decision. That caps a great deal, and the caps
are deliberate — verify against the artifact before raising any of them:

- **coroutines 1.8.1**, not newer: 1.9.0 makes `ExecutorCoroutineDispatcher`
  (supertype of `Dispatchers.Default`/`IO`) implement `java.lang.AutoCloseable`,
  API 19+, not core-library-desugared. It reaches the dex verbatim and fails
  to link on API 17/18.
- **moshi 1.8.0**, pinned `strictly`: from 1.9.0 the reflective adapter throws
  for any `@kotlin.Metadata` class, and the OpenSubtitles models are
  reflective Kotlin data classes using `@field:Json`, which moshi's codegen
  does not read.
- **mockito 3.3.3** — the version PowerMock 2.0.9 depends on.
- **androidx majors frozen** — current releases declare minSdk 21 or 23.
- **targetSdk stays 36** while compileSdk is 37: compileSdk is compile-time
  only, targetSdk changes runtime behaviour.

To bump a dependency, probe the real artifact's `minSdkVersion` (AAR manifest)
and its transitives before trusting a version number.

## Conventions

- Never `git add -A` in `vlc-libs` — untracked host-tool tarballs there are
  binaries that must not be committed.
- A signed build appends local keystore paths to the **tracked**
  `gradle.properties`. Never commit those lines.
- Lint is report-only in seven modules (upstream findings that predate the
  fork). Reports are still generated; fix rather than suppress new findings.
- 29 unit tests are `@Ignore`d and 13 files excluded as pre-existing upstream
  rot. Do not widen the exclusion to whole directories — that buries working
  tests.

## Further reading

- `BUILDING.md` — the full build, supply chain and toolchain story.
- `REMOVED.md` — what was taken out of upstream VLC and why.
- `handoff/` — dated, session-scoped records of what was done and what is not
  done. Read the most recent one before starting substantial work.

## Known false positive

A container checked out from `vlc-light-win64` may carry a local branch named
`claude/vlc-android-remove-telemetry-2tt0dm` that holds *win64* code, while
the remote branch of that name holds the old *Android* snapshot. Tooling that
compares `origin/<branch-name>..HEAD` then reports ~15 "unpushed" commits that
are in fact already published on `claude/vlc-win64-minimal-player-do4urk`.
Nothing is unsaved and **the fix is never `git push --force`** — that would
destroy the Android snapshot. Rename the local branch, or delete the stale
remote one.
