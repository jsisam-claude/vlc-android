# Building from source — no prebuilts

This fork builds the whole VLC stack **from vendored source**: no
`libvlc-all` / `medialibrary-all` / `remote-access` AARs, no VideoLAN-hosted
downloads at build time. Every Gradle variant now uses
`project(':libvlcjni:libvlc')` and `project(':medialibrary')` instead of
prebuilt artifacts.

## One-time bootstrap

**Already executed and committed** — the trees and archives below are in the
repositories. Re-run these steps only to re-pin versions.

```sh
# 1. sibling checkout of the shared source repo
git clone https://github.com/jsisam-claude/vlc-libs ../vlc-libs

# 2. vendor the code.videolan.org trees into this repo (pinned + verified):
#    libvlcjni @ 81bb02ba, medialibrary @ 8c56e26c (libvlcpp patched),
#    plus the sqlite source archive (SHA-512 pinned)
./tools/vendor-videolan.sh
git add -A && git commit -m "Vendor VideoLAN sources"

# 3. stage the committed contrib + host-tool source archives into the vlc tree
#    (they are already vendored in vlc-libs; this only copies them into place)
( cd ../vlc-libs && ./place-build-inputs.sh )
```

`vendor-videolan.sh` is idempotent (`.vendored` markers record the pinned
hashes) and ends by checking that `../vlc-libs/vlc` matches the exact VLC
commit libvlcjni expects (`VLC_TESTED_HASH`) — if not, it prints the
`vendor-vlc.sh` command to align it. It also lists any network-touching lines
left in libvlcjni's own buildsystem so they can be marker-patched the same
way the scripts in `buildsystem/` were.

## Building

```sh
./buildsystem/compile.sh -l -a arm64-v8a   # libvlc + medialibrary + app
```

The buildsystem was patched to respect vendored trees:

- `compile.sh` skips the libvlcjni clone when `libvlcjni/.vendored` exists,
  and auto-exports `VLC_TARBALLS=../vlc-libs/contrib-tarballs` when that
  sibling exists — `compile-libvlc.sh` passes it to make on the command line,
  which is the only override the contrib makefile honors (`TARBALLS` is
  `:=`-assigned). The `extras/tools` archives still need physical staging;
  run `../vlc-libs/place-build-inputs.sh` (bootstrap step 3) to copy the
  committed archives into place.
- `compile-medialibrary.sh` uses the committed sqlite source archive instead
  of downloading, and `--reset` won't touch vendored trees.

## Pruned contrib set

The contrib set is the **dependency-correct** closure for the fork's kept
features — VLC's contrib graph, not a hand-picked minimum, so it lands at
~49 source archives (all committed in `vlc-libs/contrib-tarballs/`,
SHA-512-verified against the upstream sums in `vlc/contrib/src/*/SHA512SUMS`).

The pruning is applied at the contrib bootstrap in
`libvlcjni/buildsystem/compile-libvlc.sh` via `--disable-*` flags: the heavy
and out-of-scope libraries are gone — **dav1d, libvpx, x264/x265,
dvdnav/dvdread, live555, bluray, cddb, mad, aom, openapv, mysofa,
spatialaudio** and the other disc / streaming / spatial-audio libraries.
What is **kept** (and therefore in the vendored set): the ffmpeg decoders,
the libass subtitle stack, the gnutls TLS stack, **lame** (despite a
`--disable-lame` flag, ffmpeg's `BUILD_ENCODERS` dependency pulls it back
in — it provides the mp3 encoder the Chromecast transcode pipeline uses,
and its source archive is vendored), and — deliberately, because the fork
keeps user-initiated network browsing and casting — **smb2, nfs, libdsm,
upnp and microdns**.

## What still comes from outside (toolchain boundary)

| External | Why |
|---|---|
| Android SDK (platform 36, build-tools 36) + **NDK** (21.4.7075529 for 32-bit ABIs; **27–29 for 64-bit ABIs** per `compile-libvlc.sh`) | platform toolchain; Google's terms don't allow republishing it |
| Gradle 9.3.1 | used from `PATH` if present; otherwise `compile.sh` downloads it SHA-256-pinned |
| Google Maven / Maven Central jars (AGP, Kotlin, androidx, …) | the Kotlin/Java app layer; see the supply-chain section — mirrorable into your own repo, not practically source-buildable |
| host build tools (autoconf, cmake, **gperf**, protoc via contribs, …) | from your distro (fontconfig's header generation needs gperf) |

Nothing VideoLAN-hosted is needed after bootstrap, and no prebuilt VLC
binaries exist anywhere in the tree. `local.properties` must exist at the
repo root (`sdk.dir=...`).

## Full supply chain, self-contained

Three layers, three treatments:

1. **Everything VLC / media (what touches your files and the network):
   vendored source.** vlc, libvlcjni, medialibrary(+libvlcpp), the pruned
   contribs from official repos, sqlite as a pinned source archive. No
   binaries anywhere.
2. **The JVM app layer (androidx, material, Kotlin stdlib/coroutines, AGP,
   Room/KSP, desugar): a mirror you own.** These cannot practically be built
   from source outside Google's infrastructure, so the control is possession
   plus pinning: after the first connected build run
   `./tools/mirror-maven.sh` — it harvests every artifact the build resolved
   into `../vlc-mirror/m2` (maven layout). Commit that repo. From then on
   `settings.gradle`/`build.gradle` detect the mirror and resolve
   **exclusively** from it; external repositories are never contacted and
   anything missing fails loudly. Additionally,
   `gradle/verification-metadata.xml` is **committed** (758 components,
   generated with `--write-verification-metadata sha256` against the full
   `assembleDev` graph): Gradle verifies the SHA-256 of every resolved
   artifact on every build, mirror or not.
   Remaining third-party binaries in the APK after the remote-access
   removal: androidx/material/desugar (Google, Apache-2.0/GPL+CE),
   kotlin-stdlib + kotlinx-coroutines (JetBrains, Apache-2.0), and the
   okhttp/retrofit/moshi trio — which exists **only** for the OpenSubtitles
   download dialog. Removing that feature would leave Google + JetBrains as
   the only binary vendors (see REMOVED.md for what that costs).
3. **The platform toolchain (SDK, NDK — see the table above for the
   32-bit/64-bit version split — JDK, Gradle 9.3.1): pinned installs.**
   Gradle is SHA-256-pinned in `compile.sh`; SDK/NDK
   packages are checksummed by `sdkmanager` against Google's signed
   repository manifest. Google's terms do not allow republishing the
   SDK/NDK, so self-contain these as a **private** archive of your SDK
   directory, not in a public repo.

## Status

**The full pipeline has been executed end-to-end from the vendored sources**
(NDK 27.0.12077973, arm64-v8a, Gradle 9.3.1 + AGP 9.1.1 — the committed
pins):

- Bootstrap **executed and committed**: libvlcjni, medialibrary (+libvlcpp,
  patched), the VLC tree at libvlcjni's `VLC_TESTED_HASH` with the 20-patch
  android stack applied, sqlite, and the full dependency-correct contrib set
  (~49 archives, SHA-512-verified) are vendored in vlc-libs.
- **Native stage passes**: all contribs compile (zero network fetches — the
  committed archives are verified and used), libvlc.so, libvlcjni.so and
  libmla.so link, and both AARs assemble. The issues once blamed on
  "toolchain-era friction" turned out to be three concrete, now-fixed
  things: the one-line `CMAKE_POLICY_DEFAULT_CMP0057=NEW` contrib patch
  (NDK 27 + CMake 3.28), `gperf` missing on the build host (fontconfig
  needs it — install it from your distro), and the android patch stack
  having been recorded-but-not-applied in the vendored VLC tree (fixed in
  vlc-libs; `vendor-vlc.sh` now applies patches robustly).
- **The app APK assembles** (`:application:app:assembleDev`) with the real
  pins: `compile.sh` downloaded Gradle 9.3.1 itself and verified its
  SHA-256, AGP 9.1.1 resolved from Google Maven, and the produced APK
  packages the four freshly built native libs. A stripped, re-signed
  arm64 test APK built this way runs ~65 MB.
- **Dependency verification is enforced**: `gradle/verification-metadata.xml`
  (758 components, SHA-256) is committed and the build passes with it
  active.

Remaining outside the sandbox: on-device testing, 32-bit ABIs (build with
NDK 21 per the table above), and populating `../vlc-mirror/m2` if you want
possession of the JVM-layer jars in addition to hash-pinning.
