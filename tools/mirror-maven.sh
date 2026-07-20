#!/bin/sh -e
# Make the jar/aar supply chain self-contained: harvest every artifact the
# build actually resolved out of the local Gradle cache into a maven-layout
# mirror repository that you commit and own.
#
# Usage:
#   1. run one full connected build (e.g. ./buildsystem/compile.sh -l -a arm64-v8a)
#   2. ./tools/mirror-maven.sh            # harvests into ../vlc-mirror/m2
#   3. commit the vlc-mirror repository
#
# From then on settings.gradle / build.gradle detect ../vlc-mirror/m2 and
# resolve EXCLUSIVELY from it — google()/mavenCentral() are never contacted,
# and any artifact missing from the mirror fails the build loudly instead of
# being fetched silently.
#
# Licensing note: the mirrored artifacts (androidx, AGP, Kotlin, material,
# desugar) are Apache-2.0/GPL+CE and fine to host in your own repo. The
# Android SDK/NDK are NOT redistributable — keep those out of public repos
# (a private tarball of your SDK dir is the equivalent control).

MIRROR=${1:-$(cd "$(dirname "$0")/.." && pwd -P)/../vlc-mirror/m2}
CACHE="${GRADLE_USER_HOME:-$HOME/.gradle}/caches/modules-2/files-2.1"
[ -d "$CACHE" ] || { echo "no gradle cache at $CACHE — run a full build first"; exit 1; }

mkdir -p "$MIRROR"
count=0
find "$CACHE" -type f | while read -r f; do
    rel=${f#"$CACHE"/}                      # group/artifact/version/<sha1>/file
    group=$(printf '%s' "$rel" | cut -d/ -f1 | tr . /)
    artifact=$(printf '%s' "$rel" | cut -d/ -f2)
    version=$(printf '%s' "$rel" | cut -d/ -f3)
    dest="$MIRROR/$group/$artifact/$version"
    mkdir -p "$dest"
    cp -n "$f" "$dest/$(basename "$f")" 2>/dev/null || true
done

echo "Mirror populated at $MIRROR"
find "$MIRROR" -type f | wc -l
echo "Commit the mirror repo; builds now resolve exclusively from it."
echo "Also pin hashes once: gradle --write-verification-metadata sha256 help"
echo "and commit gradle/verification-metadata.xml in this repo."
