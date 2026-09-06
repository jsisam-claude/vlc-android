# Handoff — session_018AUxChjyxpyKLd8WyDKoDT (2026-08-27 → 2026-09-06)

Written on "save state and hold". Everything below is verified against the
tree or the remote at the time of writing; nothing is assumed. Read
`CONTEXT.md` first, then this.

## 1. Where every repo stands

All four default branches were fast-forwarded to the session branch
`claude/new-session-0pkknw` on 2026-09-06 (no merge commits; 0 behind).

| Repo | Default branch | HEAD | CI |
|---|---|---|---|
| `vlc-light-win64` | `claude/vlc-win64-minimal-player-do4urk` | `b70bc05` | green (run #77) |
| `media-gallery` | `main` | `77a7999` | green (run #36) |
| `vlc-android` | `main` | `8cf91af` | no CI exists |
| `vlc-libs` | `main` | `53d86d2f` | no CI exists |

Session branch and default are identical in each repo at those SHAs. This
handoff commit lands on the session branch only (held, not merged).

### Done this session (already merged)
- One canonical `BUILDING.md` per repo with a cross-repo guide table; four
  stale doc claims fixed; libass re-vendoring procedure written down.
- CI: Node-20 deprecation cleared for `actions/checkout@v7`,
  `actions/upload-artifact@v7`, `softprops/action-gh-release@v3`.
  `ilammy/msvc-dev-cmd@v1` still warns — upstream has no node24 release and
  it cannot be dropped (Ninja generator needs the exported MSVC env).
- media-gallery CI: `branches: ['**']`, `if-no-files-found: error`,
  `retention-days: 7`.
- Privacy: no personal email/name existed in any of 192 commits or files;
  the GitHub handle was genericized in 16 places (hygiene only — the repos
  are still served from that account).
- HarfBuzz 14.3.1 → 14.4.0 in the engine, mirrored byte-identically into
  media-gallery; verified compile under g++ and MSVC (CI), API lost nothing,
  closure walker validated against the old tag first.
- A six-dimension adversarially-verified review round found 8 issues, all in
  this session's own work; all fixed (see `vlc-light-win64` commit `b70bc05`).
- `vlc-libs/VERSIONS` Gradle pin corrected 9.3.1 → 9.7.1 (pre-existing).

## 2. Branch cleanup — BLOCKED, needs a human

PR #1 in `vlc-light-win64` is **closed** with tip SHAs recorded:
https://github.com/jsisam-claude/vlc-light-win64/pull/1#issuecomment-5559142050

The four dead branches are **still present**. Every deletion attempt is
refused by the session's git egress proxy with `HTTP 403` on
`git-receive-pack`; the proxy README classifies 403 as an organisation
policy denial to report, not retry. There is no branch-delete tool in the
GitHub MCP set. Both previous sessions hit the same wall. Delete from the
GitHub branches page:

| Repo | Branch | Tip | Why safe |
|---|---|---|---|
| vlc-light-win64 | `stale` | `347281f` | Android snapshot; moved to `vlc-android`, tree hash verified; was PR #1's head |
| vlc-light-win64 | `claude/vlc-android-remove-telemetry-2tt0dm` | `d6bc347` | exactly one commit behind `stale` |
| media-gallery | `claude/embed-video-player` | `fbc50e1` | git-verified ancestor of main |
| media-gallery | `claude/zealous-einstein-y8nak8` | `6396501` | 4 unique fixes ported in `9925cad`; rest already on main |

After deletion, the "Known false positive" section of `CONTEXT.md` is
obsolete and can be removed.

## 3. AGP 9.3.1 → 9.4.0 — IN FLIGHT, held before the bump itself

The user asked for the bump despite a recommendation to wait for a device
test. **No AGP change has been committed.** `build.gradle:3` still reads
`ext.android_plugin_version = '9.3.1'`. What has been done is the
groundwork, which surfaced several facts worth more than the bump.

### 3a. Compatibility (checked against the release notes)
AGP 9.4.0 needs Gradle ≥ 9.6.0 (have 9.7.1), JDK ≥ 17, build-tools ≥ 36.0.0
(have 37.0.0), max API 37 (compileSdk is 37 — **at the ceiling**; 38 would
need AGP 10). Its default NDK is 28.2.13676358 but the project pins 29
explicitly. Its one behavioural change (dynamic-feature variant parity) is
irrelevant: no dynamic-feature modules exist. AGP **9.3.2** also exists on
Google Maven as a lower-risk patch alternative.

### 3b. A working build environment recipe (Linux, this container)
- JDK **21** is accepted — the JDK is unpinned (`BUILDING.md`), AGP's floor
  is 17. Documented reference is JDK 25; the resolved dependency set should
  be identical, APK bytes would not be. Note this in any commit.
- Gradle 9.7.1 from `services.gradle.org`, verified against the SHA-256 in
  `buildsystem/compile.sh:347`. No `gradlew` wrapper exists in the repo.
- SDK via cmdline-tools. **The platform package is `platforms;android-37.0`**
  (Android now minor-versions platforms; `platforms;android-37` does not
  exist and `sdkmanager` skips it silently). Also `build-tools;37.0.0` and
  `platform-tools`. It installs to `platforms/android-37.0/` and AGP matches
  it on `source.properties`, not the directory name.
- `local.properties` with only `sdk.dir=` (gitignored). No NDK was needed
  for a Gradle-only build with no native libs present.
- Proxy reaches `dl.google.com`, `maven.google.com`, Maven Central.
  `docs.gradle.org` and `api.adoptium.net` are egress-blocked.
- Never run two Gradle invocations on this tree concurrently.

### 3c. Finding: the committed verification metadata was already incomplete
`gradle help` under strict verification **fails at AGP 9.3.1, before any
change**, with two artifacts "checksums missing from verification metadata"
on the buildscript `classpath`:
- `org.junit:junit-bom:5.11.0-M2` (`.module`)
- `org.jetbrains.kotlinx:kotlinx-coroutines-bom:1.8.0` (`.pom`)

Neither is in the committed file under any version; neither is declared in
the project — both are transitive from a plugin on the root buildscript
classpath (AGP, kotlin-gradle-plugin 2.4.10, or gradle-maven-publish-plugin
0.37.0). Diagnosis: the previous regeneration ran on a warm cache where
these metadata-only artifacts were never re-fetched, so never recorded; a
cold cache fetches them and strict mode rejects them. Not a mismatch, so no
integrity concern — a coverage gap.

Write-mode runs so far add `com.google.guava:guava-parent:33.3.1-jre` and
`kotlinx-coroutines-bom:1.8.0`. **`junit-bom:5.11.0-M2` has not yet been
recorded by any write-mode run**, even with `--refresh-dependencies`; the
in-flight run adds `help` to the task list to see whether that resolves it.
If it still does not, investigate that artifact specifically before
trusting a strict-mode result.

### 3d. Finding: task names — the `dev` build type has no test variants
`assembleDevAndroidTest` and `testDevUnitTest` do not exist. Both androidTest
and unit tests are wired to `debug` only. Validated by Gradle itself:
`assembleDev`, `lintDev`, `assembleDebugAndroidTest`, `testDebugUnitTest`.

### 3e. Finding: root `assembleDebugAndroidTest` fails on two library modules
`:application:mediadb:mergeExtDexDebugAndroidTest` and
`:application:resources:mergeExtDexDebugAndroidTest` fail with the 64K
method-reference limit. Their androidTest APKs never had multidex; only
`app` and `television` (which set `multiDexEnabled`) assemble. The handoff's
"both androidTest APKs" meant those two. Pre-existing, never exercised by
the documented graph, **out of scope** for an AGP bump. Scope androidTest to
`:application:app:assembleDebugAndroidTest
:application:television:assembleDebugAndroidTest`.

### 3f. Findings about the metadata mechanism
- Gradle writes `verification-metadata.xml` at build finish **even when the
  build fails**.
- Regeneration is **additive**. The previous bump (9.1.1 → 9.3.1, `f6329ec`)
  left the 9.1.1 tree in place: 11 `com.android.tools.*` components at each
  version. Match that precedent; do not prune.
- After a full compile + lint + tests at 9.3.1 the delta was only the two
  cold-cache entries above — the committed file already covered those graphs.
- `<verify-metadata>true`, zero trust escapes: both must survive unchanged.

### 3g. The exact remaining procedure
The in-flight run (log `baseline-931-run4.log` in the container scratchpad,
ephemeral) is step 1. A pristine copy of the committed metadata was saved
as `verification-metadata.PRE-baseline.xml` (also ephemeral; `git show
8cf91af:gradle/verification-metadata.xml` is the durable equivalent).

```sh
export JAVA_HOME=<jdk21> ANDROID_HOME=<sdk> ANDROID_SDK_ROOT=<sdk>; PATH=<gradle-9.7.1>/bin:$PATH
TASKS="help assembleDev lintDev :application:app:assembleDebugAndroidTest \
       :application:television:assembleDebugAndroidTest testDebugUnitTest"
# 1. baseline at 9.3.1 — diff must be ADDITIVE ONLY, then strict mode must pass
gradle --write-verification-metadata sha256 --refresh-dependencies $TASKS
git diff gradle/verification-metadata.xml      # expect only + lines
gradle $TASKS                                  # strict mode: must succeed
git commit -m "Regenerate verification metadata on a cold cache" gradle/verification-metadata.xml
# 2. the bump
sed -i "s/android_plugin_version = '9.3.1'/android_plugin_version = '9.4.0'/" build.gradle
gradle --write-verification-metadata sha256 --refresh-dependencies $TASKS
git diff gradle/verification-metadata.xml      # expect a new 9.4.0 com.android.tools tree, no removals
gradle $TASKS                                  # strict mode: must succeed
git commit -am "Take AGP 9.3.1 -> 9.4.0"
```
Acceptance is build-time only. Nothing in this project has ever run on a
device, and this does not change that.

## 4. Still open, unchanged from before
- `ilammy/msvc-dev-cmd` Node-20 warning (replace with vswhere+vcvars, or live with it).
- Trim `handoff/` docs before making anything public.
- Android has no CI; the Windows repos do — the verification asymmetry runs
  opposite to the build difficulty.
- `vlc-light-win64`'s default branch is a session-scoped name; `main` was
  proposed and not acted on.
- Final non-VLC name for the Android fork; the `vlc-*` repo names.
