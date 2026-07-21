#!/bin/sh -e
# One-time vendoring of the code.videolan.org sources this fork builds from.
# Run on a machine with normal network access, then review and COMMIT the
# results. Once committed (together with vlc-libs' contrib tarball cache),
# native builds perform no downloads: everything is compiled from vendored
# source. The remaining externals are toolchain only (Android SDK/NDK,
# Gradle, and the Google/Maven-Central jars for the Kotlin app layer).
#
# Pins mirror buildsystem/compile*.sh at the fork's import commit
# (upstream vlc-android c3b20bc9a1070e3e8f43aee284d2a2b6b621296b).

LIBVLCJNI_HASH=81bb02ba48dcad32550e0626139a387b3c30af04
MEDIALIBRARY_HASH=8c56e26c625d757994cffeea84d2a0a2e6033dee
SQLITE_RELEASE=sqlite-autoconf-3460100
SQLITE_SHA512SUM=a5ba5af9c8d6440d39ba67e3d5903c165df3f1d111e299efbe7c1cca4876d4d5aecd722e0133670daa6eb5cbf8a85c6a3d9852ab507a393615fb5245a3e1a743

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
GIT="git -c user.name=vendor -c user.email=vendor@localhost"

note() { printf '\033[1;32m%s\033[0m\n' "$*"; }

is_vendored() { # hash dest
    [ -f "$2/.vendored" ] && [ "$(cat "$2/.vendored")" = "$1" ]
}

fetch_tree() { # url hash dest
    url=$1; hash=$2; dest=$3
    if is_vendored "$hash" "$dest"; then
        note "$dest already vendored at $hash"
        return 0
    fi
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    $GIT -C "$tmp" init -q
    $GIT -C "$tmp" remote add origin "$url"
    $GIT -C "$tmp" fetch --depth 1 origin "$hash"
    $GIT -C "$tmp" checkout -q FETCH_HEAD
    rm -rf "$tmp/.git"
    rm -rf "$dest"
    mkdir -p "$(dirname "$dest")"
    mv "$tmp" "$dest"
    trap - EXIT
    echo "$hash" > "$dest/.vendored"
    note "vendored $dest @ $hash"
}

# --- libvlcjni (JNI glue + libvlc gradle module + native buildsystem) ---
fetch_tree https://code.videolan.org/videolan/libvlcjni.git \
    "$LIBVLCJNI_HASH" "$ROOT/libvlcjni"

# --- medialibrary C++ core (+ libvlcpp submodule with local patches) ---
ML_DEST="$ROOT/medialibrary/medialibrary"
if is_vendored "$MEDIALIBRARY_HASH" "$ML_DEST"; then
    note "$ML_DEST already vendored at $MEDIALIBRARY_HASH"
else
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    $GIT -C "$tmp" init -q
    $GIT -C "$tmp" remote add origin https://code.videolan.org/videolan/medialibrary.git
    $GIT -C "$tmp" fetch --depth 1 origin "$MEDIALIBRARY_HASH"
    $GIT -C "$tmp" checkout -q FETCH_HEAD
    $GIT -C "$tmp" submodule update --init libvlcpp
    # same patches buildsystem/compile-medialibrary.sh applies on a fresh clone
    (cd "$tmp/libvlcpp" && $GIT am "$ROOT"/buildsystem/patches/libvlcpp/*)
    rm -rf "$tmp/.git" "$tmp/libvlcpp/.git" "$tmp/.gitmodules"
    rm -rf "$ML_DEST"
    mv "$tmp" "$ML_DEST"
    trap - EXIT
    echo "$MEDIALIBRARY_HASH" > "$ML_DEST/.vendored"
    note "vendored $ML_DEST @ $MEDIALIBRARY_HASH (libvlcpp patched)"
fi

# --- sqlite source archive (SHA-512 pinned, extracted at build time) ---
SQLITE_TGZ="$ROOT/medialibrary/$SQLITE_RELEASE.tar.gz"
if [ ! -f "$SQLITE_TGZ" ]; then
    for u in "https://download.videolan.org/pub/contrib/sqlite/$SQLITE_RELEASE.tar.gz" \
             "https://sqlite.org/2024/$SQLITE_RELEASE.tar.gz"; do
        curl -fL -o "$SQLITE_TGZ" "$u" && break || rm -f "$SQLITE_TGZ"
    done
fi
echo "$SQLITE_SHA512SUM  $SQLITE_TGZ" | sha512sum -c - >/dev/null \
    || { echo "sqlite tarball checksum mismatch"; exit 1; }
note "sqlite source archive OK: $SQLITE_TGZ"

# --- libvlc (VLC core) source comes from the sibling vlc-libs checkout ---
if [ ! -e "$ROOT/libvlcjni/vlc" ]; then
    if [ -d "$ROOT/../vlc-libs/vlc" ]; then
        ln -s ../../vlc-libs/vlc "$ROOT/libvlcjni/vlc"
        note "linked libvlcjni/vlc -> ../../vlc-libs/vlc"
    else
        echo "WARNING: ../vlc-libs not found. Clone jsisam-claude/vlc-libs next to"
        echo "         this repository, then re-run this script."
    fi
fi

# libvlcjni pins the exact VLC revision it was tested with; make sure the
# vendored vlc-libs tree matches it.
TESTED=$(grep -rh "VLC_TESTED_HASH=" "$ROOT/libvlcjni"/buildsystem/*.sh "$ROOT/libvlcjni"/*.sh 2>/dev/null | head -1 | sed 's/.*VLC_TESTED_HASH=//' | tr -d '"')
VENDORED=$(cat "$ROOT/../vlc-libs/vlc/.vendored" 2>/dev/null || true)
if [ -n "$TESTED" ] && [ "$TESTED" != "$VENDORED" ]; then
    echo "NOTE: libvlcjni expects VLC commit $TESTED"
    echo "      but ../vlc-libs/vlc is at ${VENDORED:-<missing>}."
    echo "      Run: ../vlc-libs/vendor-vlc.sh $TESTED   and commit vlc-libs."
fi

# libvlcjni's own buildsystem may guard its vlc checkout with .git checks the
# way this repo's scripts did. Surface anything that could still try the
# network so it can be patched the same way (.vendored markers).
note "network-touching lines left in libvlcjni's buildsystem (patch if the build tries to fetch):"
grep -rn "git clone\|git pull\|wget \|curl " "$ROOT/libvlcjni"/buildsystem/*.sh 2>/dev/null | grep -v "^Binary" || echo "  (none found)"

note "Done. Review the trees, then: git add -A && git commit"
