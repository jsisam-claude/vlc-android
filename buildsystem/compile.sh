#! /bin/sh
set -e


#############
# FUNCTIONS #
#############

diagnostic()
{
    echo "$@" 1>&2;
}

fail()
{
    diagnostic "$1"
    exit 1
}

# Read the Android Wiki http://wiki.videolan.org/AndroidCompile
# Setup all that stuff correctly.
# Get the latest Android SDK Platform or modify numbers in configure.sh and libvlc/default.properties.

RELEASE=0
RESET=0
# Indicates the license of contribs
AVLC_CONTRIB_LICENSE=g
while [ $# -gt 0 ]; do
    case $1 in
        help|--help|-h)
            echo "Use -a to set the ARCH:"
            echo "  ARM:     (armeabi-v7a|arm)"
            echo "  ARM64:   (arm64-v8a|arm64)"
            echo "  X86:     x86, x86_64"
            echo "Use --release to build in release mode"
            echo "Use --signrelease to build in release mode and sign apk, see vlc-android/build.gradle"
            echo "Use --reset to reset code from git"
            echo "Use -s to set your keystore file and -p for the password"
            echo "Use -l to build only LibVLC (implies --no-ml)"
            echo "Use -ml to build only the medialibrary"
            echo "Use --no-ml to skip the medialibrary"
            echo "Use -vlc4 to build against VLC 4"
            echo "Use run to install and start the app, test to build the test APK"
            echo "Use -b to bypass libvlc source checks (vlc custom sources)"
            echo "Use -t to use prebuilt contribs for LibVLC"
            echo "Use -m2 to set the maven local repository path to use"
            echo "Use --static-cpp to use the static C++ runtime"
            echo "Use --license <l> to build contribs with license l"
            echo "   g: GPLv3 (default)"
            echo "   l: LGPLv3 + ad-clauses"
            echo "   a: LGPLv2 + ad-clauses"
            exit 0
            ;;
        a|-a)
            ANDROID_ABI=$2
            shift
            ;;
        -r|release|--release)
            RELEASE=1
            ;;
        signrelease|--signrelease)
            SIGNED_RELEASE=1
            RELEASE=1
            ;;
        -s|--signature)
            KEYSTORE_FILE=$2
            shift
            ;;
        -p|--password)
            PASSWORD_KEYSTORE=$2
            shift
            ;;
        -m2|--local-maven)
            M2_REPO=$2
            shift
            ;;
        --license)
            AVLC_CONTRIB_LICENSE=$2
            shift
            ;;
        -l)
            BUILD_LIBVLC=1
            NO_ML=1
            ;;
        -t)
            PREBUILT_CONTRIBS=1
            ;;
        -ml)
            BUILD_MEDIALIB=1
            ;;
        run)
            RUN=1
            ;;
        test)
            TEST=1
            ;;
        stub)
            STUB=1
            ;;
        --reset)
            RESET=1
            ;;
        --no-ml)
            NO_ML=1
            ;;
        --init)
            GRADLE_SETUP=1
            ;;
        -b)
            BYPASS_VLC_SRC_CHECKS=1
            ;;
        -vlc4)
            FORCE_VLC_4=1
            ;;
        --static-cpp)
            AVLC_STATIC_CXX=1
            ;;
        *)
            diagnostic "$0: Invalid option '$1'."
            diagnostic "$0: Try --help for more information."
            exit 1
            ;;
    esac
    shift
done

# Debian / Android Studio layout defaults. Anything already exported wins;
# these only fill in what is unset.
#  - JAVA_HOME: Debian's openjdk-25-jdk package. Gradle/AGP run on whatever
#    JAVA_HOME supplies (nothing in the repo pins the JDK), so default it to
#    the reference JDK when it is installed.
#  - ANDROID_SDK: Android Studio and the sdkmanager docs export ANDROID_HOME;
#    the VLC scripts have always wanted ANDROID_SDK. The value flows into
#    local.properties as sdk.dir via init_local_props below, alongside the
#    android.ndkPath / android.ndkFullVersion keys Gradle also needs, which
#    is why sdk.dir is not written with a bare "echo >" here.
#  - ANDROID_NDK: sdkmanager installs NDKs under $ANDROID_SDK/ndk/<version>.
#    With exactly one installed, use it; with several, the choice is not
#    guessable (32-bit ABIs need 21.x, 64-bit need 27-29) so stay unset and
#    hit the check below.
if [ -z "$JAVA_HOME" ] && [ -d /usr/lib/jvm/java-25-openjdk-amd64 ]; then
    export JAVA_HOME=/usr/lib/jvm/java-25-openjdk-amd64
fi
if [ -z "$ANDROID_SDK" ] && [ -n "$ANDROID_HOME" ]; then
    export ANDROID_SDK="$ANDROID_HOME"
fi
if [ -z "$ANDROID_NDK" ] && [ -n "$ANDROID_SDK" ] && [ -d "$ANDROID_SDK/ndk" ]; then
    ndk_candidates=$(ls -d "$ANDROID_SDK"/ndk/*/ 2>/dev/null | wc -l)
    if [ "$ndk_candidates" -eq 1 ]; then
        export ANDROID_NDK="$(ls -d "$ANDROID_SDK"/ndk/*/ | sed 's:/$::')"
        diagnostic "*** ANDROID_NDK not set: using the only installed NDK, $ANDROID_NDK"
    fi
fi

if [ -z "$ANDROID_NDK" ] || [ -z "$ANDROID_SDK" ]; then
   diagnostic "You must define ANDROID_NDK, ANDROID_SDK before starting."
   diagnostic "They must point to your NDK and SDK directories."
   diagnostic "(ANDROID_HOME is accepted for the SDK; the NDK is picked up"
   diagnostic " automatically only when exactly one is installed under it.)"
   exit 1
fi

if [ -z "$ANDROID_ABI" ]; then
   diagnostic "*** No ANDROID_ABI defined architecture: using arm64-v8a"
   ANDROID_ABI="arm64-v8a"
elif [ "$ANDROID_ABI" = "arm64" ]; then
    ANDROID_ABI="arm64-v8a"
elif [ "$ANDROID_ABI" = "arm" ]; then
    ANDROID_ABI="armeabi-v7a"
fi

if [ "$ANDROID_ABI" = "armeabi-v7a" ]; then
    GRADLE_ABI="ARMv7"
    ARCH="arm"
    TRIPLET="arm-linux-androideabi"
elif [ "$ANDROID_ABI" = "arm64-v8a" ]; then
    GRADLE_ABI="ARMv8"
    ARCH="arm64"
    TRIPLET="aarch64-linux-android"
elif [ "$ANDROID_ABI" = "x86" ]; then
    GRADLE_ABI="x86"
    ARCH="x86"
    TRIPLET="i686-linux-android"
elif [ "$ANDROID_ABI" = "x86_64" ]; then
    GRADLE_ABI="x86_64"
    ARCH="x86_64"
    TRIPLET="x86_64-linux-android"
else
    diagnostic "Invalid arch specified: '$ANDROID_ABI' (arm64-v8a|armeabi-v7a|x86_64|x86)."
    diagnostic "Try --help for more information"
    exit 1
fi

if [ -n "$M2_REPO" ]; then
  if test -d "$M2_REPO"; then
    echo "Custom local maven repository found"
  else
    diagnostic "Invalid local maven repository path: $M2_REPO"
    exit 1
  fi
fi
####################
# Configure gradle #
####################

if [ -z "$KEYSTORE_FILE" ]; then
    KEYSTORE_FILE="$HOME/.android/debug.keystore"
    STOREALIAS="androiddebugkey"
else
    if [ -z "$PASSWORD_KEYSTORE" ]; then
        diagnostic "No password"
        exit 1
    fi
    STOREALIAS="vlc"
fi

if [ ! -f gradle.properties ]; then
    echo android.enableJetifier=false > gradle.properties
    echo android.useAndroidX=true >> gradle.properties
    echo kapt.incremental.apt=true >> gradle.properties
    echo kapt.use.worker.api=true >> gradle.properties
    echo kapt.include.compile.classpath=false >> gradle.properties
fi

# Append only the signing keys, never rewrite the file: the committed gradle.properties
# carries org.gradle.jvmargs (-Xmx4g), android.newDsl/builtInKotlin/nonTransitiveRClass
# and enableJetifier=false. Deleting it silently dropped all of those and re-enabled
# Jetifier for exactly the release builds that matter.
grep -q '^keyStoreFile=' gradle.properties || echo keyStoreFile=$KEYSTORE_FILE >> gradle.properties
grep -q '^storealias=' gradle.properties || echo storealias=$STOREALIAS >> gradle.properties
if [ -z "$PASSWORD_KEYSTORE" ]; then
    grep -q '^storepwd=' gradle.properties || echo storepwd=android >> gradle.properties
fi

init_local_props() {
    (
    # initialize the local.properties file,
    # or fix it if it was modified (by Android Studio, for example).
    echo_props() {
        echo "sdk.dir=$ANDROID_SDK"
        echo "android.ndkPath=$ANDROID_NDK"
        NDK_FULL_VERSION=$(grep -o '^Pkg.Revision.*[0-9]*.*' $ANDROID_NDK/source.properties |cut -d " " -f 3)
        echo "android.ndkFullVersion=$NDK_FULL_VERSION"
        if [ $(command -v cmake) >/dev/null 2>&1 ]; then
            # prefix of the cmake installation, not the cmake path or the dir that contains the cmake executable
            echo "cmake.dir=$(dirname $(dirname $(command -v cmake)))"
        fi
    }
    # first check if the file just needs to be created for the first time
    if [ ! -f "$1" ]; then
        echo_props > "$1"
        return 0
    fi
    # escape special chars to get regex that matches string
    make_regex() {
        echo "$1" | sed -e 's/\([[\^$.*]\)/\\\1/g' -
    }
    android_sdk_regex=`make_regex "${ANDROID_SDK}"`
    android_ndk_regex=`make_regex "${ANDROID_NDK}"`
    # check for lines setting the SDK directory
    sdk_line_start="^sdk\.dir="
    total_sdk_count=`grep -c "${sdk_line_start}" "$1"`
    good_sdk_count=`grep -c "${sdk_line_start}${android_sdk_regex}\$" "$1"`
    # check for lines setting the NDK directory
    ndk_line_start="^android\.ndkPath="
    total_ndk_count=`grep -c "${ndk_line_start}" "$1"`
    good_ndk_count=`grep -c "${ndk_line_start}${android_ndk_regex}\$" "$1"`
    # check for the line setting the NDK version. Gradle needs it to agree with
    # android.ndkPath: if it is missing (Android Studio writes sdk.dir and
    # android.ndkPath but never this VLC-specific key) the build.gradle fallback
    # applies instead, AGP reports CXX1100 and resolves no NDK at all -- which
    # only degrades stripDebugSymbols to a warning, so the APK silently ships
    # unstripped .so files.
    ndk_version_line_start="^android\.ndkFullVersion="
    ndk_full_version=$(grep -o '^Pkg.Revision.*[0-9]*.*' $ANDROID_NDK/source.properties |cut -d " " -f 3)
    ndk_version_regex=`make_regex "${ndk_full_version}"`
    total_ndk_version_count=`grep -c "${ndk_version_line_start}" "$1" || true`
    good_ndk_version_count=`grep -c "${ndk_version_line_start}${ndk_version_regex}\$" "$1" || true`
    # if one of each is found and all match the environment vars, no action needed
    if [ "$total_sdk_count" -eq "1" ] && [ "$good_sdk_count" -eq "1" ] \
    && [ "$total_ndk_count" -eq "1" ] && [ "$good_ndk_count" -eq "1" ] \
    && [ "$total_ndk_version_count" -eq "1" ] && [ "$good_ndk_version_count" -eq "1" ]
    then
        return 0
    fi
    # if neither property is set they can simply be appended to the file
    if [ "$total_sdk_count" -eq "0" ] && [ "$total_ndk_count" -eq "0" ] \
    && [ "$total_ndk_version_count" -eq "0" ]; then
        echo_props >> "$1"
        return 0
    fi
    # if a property is set incorrectly or too many times,
    # remove all instances of both properties and append correct ones.
    replace_props() {
        temp_props="$1.tmp"
        while IFS= read -r LINE || [ -n "$LINE" ]; do
            line_sdk_dir="${LINE#sdk.dir=}"
            line_ndk_dir="${LINE#android.ndkPath=}"
            line_ndk_version="${LINE#android.ndkFullVersion=}"
            line_cmake_dir="${LINE#cmake.dir=}"
            if [ "x$line_sdk_dir" = "x$LINE" ] && [ "x$line_ndk_dir" = "x$LINE" ] && [ "x$line_ndk_version" = "x$LINE" ] && [ "x$line_cmake_dir" = "x$LINE" ]; then
                echo "$LINE"
            fi
        done <"$1" >"$temp_props"
        echo_props >> "$temp_props"
        mv -f -- "$temp_props" "$1"
    }
    echo "local.properties: Contains incompatible sdk.dir and/or android.ndkPath properties. Replacing..."
    replace_props "$1"
    echo "local.properties: Finished replacing sdk.dir and/or android.ndkPath with current environment variables."
    )
}
init_local_props local.properties || { echo "Error initializing local.properties"; exit $?; }

if [ ! -d "$ANDROID_SDK/licenses" ]; then
    mkdir "$ANDROID_SDK/licenses"
    echo "24333f8a63b6825ea9c5514f83c2829b004d1fee" > "$ANDROID_SDK/licenses/android-sdk-license"
    echo "d56f5187479451eabf01fb78af6dfcb131a6481e" >> "$ANDROID_SDK/licenses/android-sdk-license"
    echo "24333f8a63b6825ea9c5514f83c2829b004d1fee" >> "$ANDROID_SDK/licenses/android-sdk-license"
fi

if [ "$FORCE_VLC_4" = 1 ]; then
    gradle_prop="-PforceVlc4=true"
fi

####################
# Fetch libVLCjni source #
####################


if [ "$FORCE_VLC_4" = 1 ]; then
    LIBVLCJNI_TESTED_HASH=a8d53a9151d7e4a9a5dfd0a5eb1cd92669afdc21
    LIBVLCJNI_BRANCH="master"
else
    LIBVLCJNI_TESTED_HASH=81bb02ba48dcad32550e0626139a387b3c30af04
    LIBVLCJNI_BRANCH="libvlcjni-3.x"
fi
LIBVLCJNI_REPOSITORY=https://code.videolan.org/videolan/libvlcjni.git

: ${VLC_LIBJNI_PATH:="$(pwd -P)/libvlcjni"}

if [ -f "$VLC_LIBJNI_PATH/.vendored" ]; then
    diagnostic "libvlcjni sources: vendored at $(cat "$VLC_LIBJNI_PATH/.vendored")"
    (cd "$VLC_LIBJNI_PATH" && init_local_props local.properties) || { echo "Error initializing local.properties"; exit $?; }
elif [ ! -d "$VLC_LIBJNI_PATH" ] || [ ! -d "$VLC_LIBJNI_PATH/.git" ]; then
    diagnostic "libvlcjni sources: not found, cloning"
    if [ ! -d "$VLC_LIBJNI_PATH" ]; then
        git clone --single-branch --branch ${LIBVLCJNI_BRANCH} "${LIBVLCJNI_REPOSITORY}"
        cd libvlcjni
    else # folder exist with only the artifacts
        cd libvlcjni
        git init
        git remote add origin "${LIBVLCJNI_REPOSITORY}"
        git pull origin ${LIBVLCJNI_BRANCH}
    fi
    git reset --hard ${LIBVLCJNI_TESTED_HASH} || fail "libvlcjni sources: LIBVLCJNI_TESTED_HASH ${LIBVLCJNI_TESTED_HASH} not found"
    init_local_props local.properties || { echo "Error initializing local.properties"; exit $?; }
    cd ..
fi

##########
# GRADLE #
##########

# Locate the vlc-libs checkout that supplies the VLC core and the contrib
# archives. The documented layout is a sibling directory, but a nested one is
# common in practice, so accept both rather than silently falling back to
# network downloads. VLC_LIBS_DIR may also be set explicitly.
if [ -z "$VLC_LIBS_DIR" ]; then
    for candidate in "$(pwd -P)/../vlc-libs" "$(pwd -P)/vlc-libs"; do
        if [ -d "$candidate" ]; then VLC_LIBS_DIR="$candidate"; break; fi
    done
fi
[ -n "$VLC_LIBS_DIR" ] && diagnostic "vlc-libs: using $VLC_LIBS_DIR"

# Prefer the vendored contrib source-archive cache from that checkout so contrib
# builds run without downloads. The variable the contrib build actually consumes
# is VLC_TARBALLS (compile-libvlc.sh passes it to make as TARBALLS=... on the
# command line — the makefile :=-assigns TARBALLS, so a plain environment
# variable would be ignored).
if [ -z "$VLC_TARBALLS" ] && [ -n "$VLC_LIBS_DIR" ] && [ -d "$VLC_LIBS_DIR/contrib-tarballs" ]; then
    export VLC_TARBALLS="$VLC_LIBS_DIR/contrib-tarballs"
    diagnostic "contrib tarballs: using vendored cache $VLC_TARBALLS"
fi

GRADLE_VERSION=9.7.1
# the SHA256 is found in https://gradle.org/release-checksums/
GRADLE_SHA256=acd53f1edaf02f1a8ff99879f8a34b302661a057d9b063ae9e35b552f804d20a
GRADLE_URL=https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip
GRADLE_DOWNLOADED_ZIP=gradle-${GRADLE_VERSION}-bin.zip

if [ -e "./gradlew" ] && [ -x "./gradlew" ]; then
    GRADLE_CACHED_VERSION=$(./gradlew -q 2>/dev/null | grep gradle_version= | cut -b 16-)
    if [ "$GRADLE_CACHED_VERSION" != "$GRADLE_VERSION" ]; then
        diagnostic "gradlew version $GRADLE_CACHED_VERSION not matching $GRADLE_VERSION"
        rm -rf "./gradlew"
    fi
fi
if [ ! -e "./gradlew" ] || [ ! -x "./gradlew" ]; then
    diagnostic "gradlew not found"
    export PATH="$(pwd -P)/gradle-${GRADLE_VERSION}/bin:$PATH"
    GRADLE_PATH_VERSION=$(cd buildsystem/gradle_version; gradle -q 2>/dev/null | grep gradle_version= | cut -b 16-)
    if [ "$GRADLE_PATH_VERSION" != "$GRADLE_VERSION" ]; then
        diagnostic "gradle could not be found in PATH, downloading"
        wget ${GRADLE_URL} -O ${GRADLE_DOWNLOADED_ZIP}  2>/dev/null || curl -LO ${GRADLE_URL} || fail "gradle: download failed"
        echo $GRADLE_SHA256 ${GRADLE_DOWNLOADED_ZIP} | sha256sum -c || fail "gradle: hash mismatch"

        unzip -o ${GRADLE_DOWNLOADED_ZIP} || fail "gradle: unzip failed"
        rm -rf ${GRADLE_DOWNLOADED_ZIP}
    fi

    # Pass the checksum explicitly: `gradle wrapper` rewrites distributionUrl
    # but PRESERVES any distributionSha256Sum already in
    # gradle/wrapper/gradle-wrapper.properties, so after a GRADLE_VERSION bump
    # the old sum would be checked against the new distribution and every run
    # would die with "Verification of Gradle distribution failed" -- without
    # self-healing, since each run regenerates and re-preserves it.
    gradle wrapper --gradle-distribution-sha256-sum ${GRADLE_SHA256} ${gradle_prop} || fail "gradle: wrapper failed"

    chmod a+x gradlew
fi
./gradlew -version || fail "gradle: wrapper failed"

####################
# Fetch VLC source #
####################

# libvlcjni reads the VLC core from $VLC_LIBJNI_PATH/vlc. In this fork that path
# is a symlink into the vlc-libs checkout, created by tools/vendor-videolan.sh --
# and it is gitignored (.gitignore:3), so it does NOT exist in a fresh clone.
# When it is missing, libvlcjni's get-vlc.sh silently CLONES VLC from
# code.videolan.org, which breaks this fork's no-downloads contract; the Gradle
# lua asset copies in libvlcjni/libvlc/build.gradle also become NO-SOURCE, so the
# AAR is built with no assets and the build still reports success. Link it here,
# and refuse to fall through to the clone.
if [ -z "$VLC_SRC_DIR" ] && [ ! -e "$VLC_LIBJNI_PATH/vlc" ]; then
    if [ -n "$VLC_LIBS_DIR" ] && [ -d "$VLC_LIBS_DIR/vlc" ]; then
        # Relative first so the checkout stays relocatable; fall back to absolute
        # if the relative form does not resolve (non-default VLC_LIBJNI_PATH, or
        # a nested vlc-libs layout).
        ln -s ../../vlc-libs/vlc "$VLC_LIBJNI_PATH/vlc" 2>/dev/null || true
        if [ ! -d "$VLC_LIBJNI_PATH/vlc" ]; then
            rm -f "$VLC_LIBJNI_PATH/vlc"
            ln -s "$VLC_LIBS_DIR/vlc" "$VLC_LIBJNI_PATH/vlc" || fail "VLC sources: could not link $VLC_LIBJNI_PATH/vlc"
        fi
        diagnostic "VLC sources: linked $VLC_LIBJNI_PATH/vlc -> $(readlink "$VLC_LIBJNI_PATH/vlc")"
    else
        diagnostic "VLC sources: $VLC_LIBJNI_PATH/vlc is missing and no vlc-libs checkout was found."
        diagnostic "  This fork takes the VLC core from vlc-libs and never downloads it."
        diagnostic "  Clone it beside this repo:  git clone <your-account>/vlc-libs ../vlc-libs"
        diagnostic "  then run ./tools/vendor-videolan.sh, or set VLC_LIBS_DIR to an existing checkout."
        diagnostic "  To let libvlcjni clone VLC from the network anyway, set ALLOW_VLC_CLONE=1."
        [ "$ALLOW_VLC_CLONE" = 1 ] || fail "VLC sources: refusing to download VLC (see above)"
        diagnostic "VLC sources: ALLOW_VLC_CLONE=1 set, falling through to the upstream network clone"
    fi
fi

# If you want to use an existing vlc dir add its path to an VLC_SRC_DIR env var
if [ -z "$VLC_SRC_DIR" ]; then
    get_vlc_args=
    if [ "$BYPASS_VLC_SRC_CHECKS" = 1 ]; then
        get_vlc_args="${get_vlc_args} -b"
    fi
    if [ $RESET -eq 1 ]; then
        get_vlc_args="${get_vlc_args} --reset"
    fi

    (cd ${VLC_LIBJNI_PATH} && ./buildsystem/get-vlc.sh ${get_vlc_args})
fi

# Always clone VLC when using --init since we'll need to package some files
# during the final assembly (lua/hrtfs/..)
if [ "$GRADLE_SETUP" = 1 ]; then
    exit 0
fi

############
# Make VLC #
############
diagnostic "Configuring"

if [ "$AVLC_STATIC_CXX" = 1 ]; then
    CONFIG_ARGS="$CONFIG_ARGS --static-cpp"
fi

# Build LibVLC if asked for it, or needed by medialibrary
OUT_DBG_DIR="$(pwd -P)/.dbg/${ANDROID_ABI}"
mkdir -p $OUT_DBG_DIR

if [ "$BUILD_MEDIALIB" != 1 ] || [ ! -d "${VLC_LIBJNI_PATH}/libvlc/jni/libs/" ]; then
    if [ "$PREBUILT_CONTRIBS" = 1 ];then
        VLC_CONTRIB_SHA="$(cd ${VLC_LIBJNI_PATH}/vlc && extras/ci/get-contrib-sha.sh android-${ARCH})"
        if [ "$FORCE_VLC_4" = 1 ]; then
            export VLC_PREBUILT_CONTRIBS_URL="https://artifacts.videolan.org/vlc/android-${ARCH}/vlc-contrib-${TRIPLET}-${VLC_CONTRIB_SHA}.tar.bz2"
        else
            export VLC_PREBUILT_CONTRIBS_URL="https://artifacts.videolan.org/vlc-3.0/android-${ARCH}/vlc-contrib-${TRIPLET}-${VLC_CONTRIB_SHA}.tar.bz2"
        fi
        if ${VLC_LIBJNI_PATH}/vlc/extras/ci/check-url.sh "$VLC_PREBUILT_CONTRIBS_URL"; then CONTRIB_FLAGS="--with-prebuilt-contribs"; fi
    fi
    # --release has to be passed through explicitly: compile-libvlc.sh reads it
    # from $RELEASE, which is a plain (unexported) shell variable here, so a
    # child process never sees it. Without this, `compile.sh --release` built
    # libvlc.so and libvlcjni.so with --enable-debug and NDK_DEBUG=1 and
    # packaged them into a release APK.
    libvlc_args="-a ${ARCH} ${CONTRIB_FLAGS} ${CONFIG_ARGS} --license $AVLC_CONTRIB_LICENSE"
    if [ "$RELEASE" = 1 ]; then
        libvlc_args="$libvlc_args --release"
    fi
    ${VLC_LIBJNI_PATH}/buildsystem/compile-libvlc.sh ${libvlc_args}

    cp -a ${VLC_LIBJNI_PATH}/libvlc/jni/obj/local/${ANDROID_ABI}/*.so "${OUT_DBG_DIR}"

    # The native build has just generated the static module list, so this is the
    # first moment we can tell which libvlc modules the APK will actually
    # contain. Contrib pruning removes modules, removing the options they
    # define, while the app layer keeps passing them -- and libvlc treats an
    # option no loaded module defines as a FATAL libvlc_new() failure whose
    # diagnostic Android discards. That shipped twice (--hrtf-file, --soundfont)
    # and is invisible to compiling and to unit tests. Catch it here instead.
    if command -v python3 >/dev/null 2>&1; then
        python3 "$(pwd -P)/tools/check-libvlc-options.py" \
            --app "$(pwd -P)" --vlc "${VLC_LIBJNI_PATH}/vlc" \
            || fail "libvlc options: the app passes options no built module defines (see above)"
    else
        diagnostic "*** python3 not found: skipping the libvlc option/module check"
    fi
fi

if [ "$NO_ML" != 1 ]; then
    medialig_args="-a $ANDROID_ABI $CONFIG_ARGS"
    if [ "$RELEASE" = 1 ]; then
        medialig_args="$medialig_args --release"
    fi
    if [ "$RESET" = 1 ]; then
        medialig_args="$medialig_args --reset"
    fi
    buildsystem/compile-medialibrary.sh ${medialig_args}
    cp -a medialibrary/jni/libs/${ANDROID_ABI}/*.so "${OUT_DBG_DIR}"
fi

##################
# Compile the UI #
##################
BUILDTYPE="Dev"
if [ "$TEST" = 1 ]; then
    BUILDTYPE="Debug"
elif [ "$SIGNED_RELEASE" = 1 ]; then
    BUILDTYPE="signedRelease"
elif [ "$RELEASE" = 1 ]; then
    BUILDTYPE="Release"
fi
if [ "$TEST" = 1 ] || [ "$RUN" = 1 ]; then
    ACTION="install"
else
    ACTION="assemble"
fi
GRADLE_TASK="${ACTION}${BUILDTYPE}"

if [ -n "$M2_REPO" ]; then
    gradle_prop="$gradle_prop -Dmaven.repo.local=$M2_REPO"
fi

if [ "$BUILD_LIBVLC" = 1 ];then
    # Build libvlc through the root build so dependency verification and the
    # local-mirror repository setup apply (the standalone libvlcjni root has no
    # verification metadata). Only the in-tree libvlcjni is a subproject of
    # this root, so an out-of-tree VLC_LIBJNI_PATH keeps the old invocation.
    if [ "$VLC_LIBJNI_PATH" = "$(pwd -P)/libvlcjni" ]; then
        GRADLE_ABI=$GRADLE_ABI ./gradlew ${gradle_prop} ":libvlcjni:libvlc:$GRADLE_TASK"
    else
        diagnostic "libvlcjni is out of tree ($VLC_LIBJNI_PATH): building from its own root, without dependency verification"
        GRADLE_ABI=$GRADLE_ABI ./gradlew ${gradle_prop} --project-dir ${VLC_LIBJNI_PATH}/libvlc $GRADLE_TASK
    fi
    RUN=0
elif [ "$BUILD_MEDIALIB" = 1 ]; then
    gradle_prop="$gradle_prop -PvlcLibVariant=$GRADLE_ABI"
    ./gradlew ${gradle_prop} --project-dir medialibrary $GRADLE_TASK
    RUN=0
else
    ./gradlew ${gradle_prop} $GRADLE_TASK
    if [ "$BUILDTYPE" = "Release" ] && [ "$ACTION" = "assemble" ]; then
        ./gradlew ${gradle_prop} "bundle${BUILDTYPE}"
    fi
    if [ "$TEST" = 1 ]; then
        ./gradlew ${gradle_prop} "application:vlc-android:install${BUILDTYPE}AndroidTest"

        echo -e "\n===================================\nRun following for UI tests:"
        echo "adb shell am instrument -w -m -e clearPackageData true   -e package org.videolan.vlc -e debug false org.videolan.vlc.debug.test/org.videolan.vlc.MultidexTestRunner 1> result_UI_test.txt"
    fi
fi

#######
# RUN #
#######
if [ "$RUN" = 1 ]; then
    export PATH="${ANDROID_SDK}/platform-tools/:$PATH"
    if [ "$STUB" = 1 ]; then
        EXTRA="--ez 'extra_test_stubs' true"
    fi
    adb wait-for-device
    if [ "$RELEASE" = 1 ]; then
        adb shell am start -n org.videolan.vlc/org.videolan.vlc.StartActivity $EXTRA
    else
        adb shell am start -n org.videolan.vlc.debug/org.videolan.vlc.StartActivity $EXTRA
    fi
fi
