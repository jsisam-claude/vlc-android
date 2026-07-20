# Building from source — no prebuilts

This fork builds the whole VLC stack **from vendored source**: no
`libvlc-all` / `medialibrary-all` / `remote-access` AARs, no VideoLAN-hosted
downloads at build time. Every Gradle variant now uses
`project(':libvlcjni:libvlc')` and `project(':medialibrary')` instead of
prebuilt artifacts.

## One-time bootstrap (needs network once)

```sh
# 1. sibling checkout of the shared source repo
git clone https://github.com/jsisam-claude/vlc-libs ../vlc-libs

# 2. vendor the code.videolan.org trees into this repo (pinned + verified):
#    libvlcjni @ 81bb02ba, medialibrary @ 8c56e26c (libvlcpp patched),
#    plus the sqlite source archive (SHA-512 pinned)
./tools/vendor-videolan.sh
git add -A && git commit -m "Vendor VideoLAN sources"

# 3. generate the contrib archives from the official-source trees vendored in
#    vlc-libs/contrib-src (needs autotools + xz), then fetch the supplement set
#    (gnutls/nettle/gmp/libiconv/libdvbpsi; SHA-512 verified)
( cd ../vlc-libs && ./make-contrib-tarballs.sh && ./fetch-contribs.sh && git add -A && git commit -m "Vendor contrib archives" )
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

- `compile.sh` skips the libvlcjni clone when `libvlcjni/.vendored` exists and
  exports `TARBALLS=../vlc-libs/contrib-tarballs` automatically so contribs
  build from the committed archives.
- `compile-medialibrary.sh` uses the committed sqlite source archive instead
  of downloading, and `--reset` won't touch vendored trees.

## Pruned contrib set

The third-party set is cut to the minimal-player list in
`../vlc-libs/contrib-pruned.list`: eight packages vendored as **source
trees from their official repositories** at the exact pinned production
versions (ffmpeg 8.1.2, libass, freetype, fribidi, harfbuzz, libogg,
libebml, libmatroska) plus five supplemented as official release archives
(gnutls, nettle, gmp, libiconv, libdvbpsi — no official GitHub exists).
Everything else in the default android contrib set (dav1d, libvpx, live555,
smb2, upnp, dvd/bluray, …) is dropped; pass the matching `--disable-*`
flags to libvlcjni's contrib bootstrap once it is vendored, per the notes
in that list.

## What still comes from outside (toolchain boundary)

| External | Why |
|---|---|
| Android SDK (platform 36, build-tools 36) + **NDK 21.4.7075529** | platform toolchain; Google's terms don't allow republishing it |
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
3. **The platform toolchain (SDK, NDK 21.4.7075529, JDK, Gradle 9.3.1):
   pinned installs.** Gradle is SHA-256-pinned in `compile.sh`; SDK/NDK
   packages are checksummed by `sdkmanager` against Google's signed
   repository manifest. Google's terms do not allow republishing the
   SDK/NDK, so self-contain these as a **private** archive of your SDK
   directory, not in a public repo.

## Status

The Gradle/buildsystem rewiring was done in a sandbox without an Android SDK
or access to code.videolan.org, so the vendor script and a full
`compile.sh` run have not been executed end-to-end here. All hashes are
pinned from upstream's own buildsystem; if libvlcjni's internal scripts still
try to fetch VLC despite the symlinked tree, patch their guard exactly like
`buildsystem/compile.sh` (a `.vendored` marker check) and please report back.
