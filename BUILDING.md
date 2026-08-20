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
- `compile-medialibrary.sh` prefers the committed sqlite source archive; if
  it is missing it falls back to downloading, then enforces the pinned
  SHA-512 either way (mismatch is a hard `exit 1`). `--reset` won't touch
  vendored trees.

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

## Host packages (Debian trixie / Ubuntu 24.04)

The build compiles several host tools from the vendored `host-tools/` sources,
so this list is only what must come from the distro. Package names are
identical on Debian trixie and Ubuntu 24.04.

```sh
sudo apt-get install -y \
    openjdk-25-jdk \
    build-essential git curl wget unzip zip tar xz-utils bzip2 patch file \
    autoconf automake m4 pkg-config libtool-bin \
    cmake ninja-build meson python3 python3-setuptools \
    bison flex gperf nasm gawk \
    gettext autopoint texinfo help2man protobuf-compiler ant
```

Why the less obvious ones:

| Package | Needed for |
|---|---|
| `openjdk-25-jdk` | Gradle/AGP. Reference build environment is Debian trixie's **`25.0.4+7-1~deb13u1`** (intended target, supplied by the user; not verifiable from this sandbox — Debian archives are blocked by its egress policy, so check which trixie suite provides it). (The verification runs recorded under Status were executed on the same Debian `openjdk-25` source package rebuilt for Ubuntu 24.04, `25.0.3+9-2~24.04.2` — one upstream patch release behind, because Debian's archives are not reachable from that sandbox.) |
| `gperf` | fontconfig generates a perfect-hash header with it; without it the contrib build fails late |
| `nasm` | ffmpeg x86 assembly — required only for the `x86`/`x86_64` ABIs, not for arm |
| `ant` | VLC's `extras/tools` bootstrap checks for it. It is **not** vendored: upstream publishes Ant only as a prebuilt jar distribution, which this project's source-only policy forbids |
| `meson` | build system for several contribs (harfbuzz, libass deps). Not vendored |
| `autopoint` | `autoreconf` runs it for gettext-using contribs; libgpg-error fails with "Can't exec autopoint" without it. Separate package from `gettext` on Debian/Ubuntu |
| `texinfo` (`makeinfo`), `gawk` | required by several autotools contribs during `autoreconf`/doc generation |
| `bison`, `flex` | generated parsers in the contrib chain |
| `libtool-bin`, `gettext`, `help2man`, `protobuf-compiler`, `ninja-build` | checked by `extras/tools/bootstrap`. Vendored sources exist for all five, so they are optional — installing them just skips building them |

Minimum versions the bootstrap enforces (all satisfied by trixie/24.04):
autoconf 2.71, automake 1.15, m4 1.4.16, libtool 2.4, cmake 3.18, meson 0.60,
bison 3.0, protoc 3.4, nasm 2.15.

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
   `gradle/verification-metadata.xml` is **committed** (767 components,
   1370 SHA-256 artifact entries, `verify-metadata=true`, and no
   trusted-artifact/regex/PGP escape hatches): Gradle verifies every
   artifact resolved **through the repo-root build** — the app, television,
   resources, tools, mediadb and `:medialibrary` — mirror or not. It was
   generated across the `assembleDev` **and** `lintDev` graphs, so lint runs
   under strict verification too; add new task graphs to the regeneration or
   they fail closed.
   **Known scope gap:** `compile.sh -l` builds libvlc with
   `--project-dir libvlcjni/libvlc`, and because `libvlcjni/settings.gradle`
   exists that invocation's build root is `libvlcjni/`, which has no
   verification metadata — so its plugin classpath (AGP, kotlin-gradle-plugin,
   the publish plugins) resolves unverified. Close it either by building that
   module through the root (`./gradlew :libvlcjni:libvlc:<task>`, already
   declared in `settings.gradle`) or by generating
   `libvlcjni/gradle/verification-metadata.xml` (note `.gitignore`'s
   `gradle*/` rule means it must be `git add -f`'d, as the root one was).
   Both need a build to validate, so neither is applied here.
   Remaining third-party binaries in the APK after the remote-access
   removal: androidx/material/desugar (Google, Apache-2.0/GPL+CE),
   kotlin-stdlib + kotlinx-coroutines (JetBrains, Apache-2.0), okhttp +
   retrofit (OpenSubtitles networking), moshi (OpenSubtitles **and** general
   JSON use — settings import/export, locale/equalizer/library metadata),
   plus zxing (QR fallback for browserless TVs), colorpicker (subtitle and
   widget preferences), konfetti (the About easter egg) and ok2curl.
   Dropping OpenSubtitles would remove okhttp/retrofit but **not** moshi, and
   would not by itself reduce the vendor list to Google + JetBrains.
3. **The platform toolchain (SDK, NDK — see the table above for the
   32-bit/64-bit version split — and Gradle 9.3.1): pinned installs. The JDK
   is NOT pinned by anything in the repo** — there is no
   `java { toolchain }`, no `org.gradle.java.home`, so the build uses whatever
   `JAVA_HOME`/PATH supplies. Export `JAVA_HOME` to your JDK 25 before
   building. This matters beyond tidiness: AGP decides warn-vs-error on
   `source/target 8` from the *running* javac version, and javac 21 and 25
   emit byte-different (semantically identical) class files, so an APK is not
   bit-reproducible across JDKs.
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
  (767 components, SHA-256) is committed and both `assembleDev` and `lintDev`
  pass with it active.

Remaining outside the sandbox: on-device testing, 32-bit ABIs (build with
NDK 21 per the table above), and populating `../vlc-mirror/m2` if you want
possession of the JVM-layer jars in addition to hash-pinning.
