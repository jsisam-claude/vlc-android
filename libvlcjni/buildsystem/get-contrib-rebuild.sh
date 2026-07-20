#!/usr/bin/env bash
# Returns 0 if the prebuilt contrib can be used
set -e

# Print error message and terminate script with status 1
# Arguments:
#   Message to print
abort_err()
{
    echo "ERROR: $1" >&2
    exit 1
}

command -v "git" >/dev/null 2>&1 || abort_err "Git was not found!"

# VLC source root directory
LIBVLCJNI_SRC_ROOT_DIR=$(git rev-parse --show-toplevel)

[ -n "${LIBVLCJNI_SRC_ROOT_DIR}" ] || abort_err "This script must be run in the libvlcjni Git repo and git must be available"
[ -f "${LIBVLCJNI_SRC_ROOT_DIR}/libvlc/jni/libvlcjni.c" ] || abort_err "This script must be run in the libvlcjni Git repository"

REFERENCE_BRANCH="$1"
[ -n "${REFERENCE_BRANCH}" ] || abort_err "Missing reference branch argument (origin/master ?)"

# Check if files were changed in the buildsystem
ALL_CHANGES=$(git diff --name-only "${REFERENCE_BRANCH}") || abort_err "Unknown branch ${REFERENCE_BRANCH}"
if [ -z "${ALL_CHANGES}" ]; then
    BUILDSYSTEM_CHANGED=""
else
    BUILDSYSTEM_CHANGED=$(echo "${ALL_CHANGES}" | grep ^buildsystem/)
fi

if [ -n "${BUILDSYSTEM_CHANGED}" ]; then
    # The buildsystem has been modified, we need to rebuild contribs
    echo "buildsystem changed compared to $1"
    exit 1
fi

exit 0
