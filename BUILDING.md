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
dvdnav/dvdread, live555, bluray, cddb, mad, aom, lame, openapv, mysofa** and
the other encoders / disc / streaming / spatial-audio libraries. What is
**kept** (and therefore in the vendored set): the ffmpeg decoders, the
libass subtitle stack, the gnutls TLS stack, and — deliberately, because the
fork keeps user-initiated network browsing and casting — **smb2, nfs,
libdsm, upnp and microdns**.

## What still comes from outside (toolchain boundary)

| External | Why |
|---|---|
| Android SDK (platform 36, build-tools 36) + **NDK** (21.4.7075529 for 32-bit ABIs; **27–29 for 64-bit ABIs** per `compile-libvlc.sh`) | platform toolchain; Google's terms don't allow republishing it |
| Gradle 9.3.1 | used from `PATH` if present; otherwise `compile.sh` downloads it SHA-256-pinned |
| Google Maven / Maven Central jars (AGP, Kotlin, androidx, …) | the Kotlin/Java app layer; see the supply-chain section — mirrorable into your own repo, not practically source-buildable |
| host build tools (autoconf, cmake, protoc via contribs, …) | from your distro |

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
   anything missing fails loudly. Additionally run
   `gradle --write-verification-metadata sha256 help` once and commit
   `gradle/verification-metadata.xml` so every artifact is SHA-256-pinned
   even when building without the mirror.
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

Verified so far:

- The bootstrap has been **executed and committed**: libvlcjni, medialibrary
  (+libvlcpp, patched), the VLC tree at libvlcjni's `VLC_TESTED_HASH` with
  the 20-patch android stack applied, sqlite, and the full dependency-correct
  contrib set (~49 archives, SHA-512-verified) are vendored in vlc-libs.
- `:application:app:compileDebugKotlin` **builds successfully** against the
  vendored sources as project dependencies — every module's Kotlin/Java,
  data binding, KSP/Room and resource/manifest merging pass. (Validation ran
  on Gradle 8.14.3 / AGP 8.13.2 because the pinned Gradle 9.3.1
  distribution is hosted as a GitHub release asset the sandbox proxy blocks;
  the repo's 9.x pins are untouched — exercise them on first local build.)
- Native stage (`compile.sh` NDK build) — driven far in-sandbox (NDK 27,
  arm64): the vendored VLC source + 20-patch stack configure, every
  clone/download is marker-guarded (zero network fetches for VLC sources),
  the host-tool bootstrap builds libtool/protobuf/ant/help2man from the
  vendored `host-tools/` sources, and the CMake-based contribs build after a
  one-line compat patch (`CMAKE_POLICY_DEFAULT_CMP0057=NEW`, for NDK 27 +
  CMake 3.28). The remaining contrib failures (gnutls "cannot compile and
  link", harfbuzz depfile races) are incompatibilities between VLC 3.0.x-era
  contribs and this newer NDK/host-tool combination — the reason VLC's own
  buildbot pins an exact toolchain. Build on the VLC-tested toolchain
  versions to close these; the vendored sources themselves are intact.
  All ~48 contrib tarballs + host-tool sources are committed in vlc-libs;
  run `../vlc-libs/place-build-inputs.sh` before building to stage them.
