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

# 3. cache the contrib source archives (ffmpeg, dav1d, …; SHA-512 verified)
( cd ../vlc-libs && ./fetch-contribs.sh && git add contrib-tarballs && git commit -m "Vendor contrib tarballs" )
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

## What still comes from outside (toolchain boundary)

| External | Why |
|---|---|
| Android SDK (platform 36, build-tools 36) + **NDK 21.4.7075529** | platform toolchain; Google's terms don't allow republishing it |
| Gradle 9.3.1 | used from `PATH` if present; otherwise `compile.sh` downloads it SHA-256-pinned |
| Google Maven / Maven Central jars (AGP, Kotlin, androidx, ktor, …) | the Kotlin/Java app layer; building androidx from source is not practical |
| host build tools (autoconf, cmake, protoc via contribs, …) | from your distro |

Nothing VideoLAN-hosted is needed after bootstrap, and no prebuilt VLC
binaries exist anywhere in the tree. `local.properties` must exist at the
repo root (`sdk.dir=...`).

## Status

The Gradle/buildsystem rewiring was done in a sandbox without an Android SDK
or access to code.videolan.org, so the vendor script and a full
`compile.sh` run have not been executed end-to-end here. All hashes are
pinned from upstream's own buildsystem; if libvlcjni's internal scripts still
try to fetch VLC despite the symlinked tree, patch their guard exactly like
`buildsystem/compile.sh` (a `.vendored` marker check) and please report back.
