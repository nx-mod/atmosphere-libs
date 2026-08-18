#!/bin/bash
# Builds libstratosphere -- the flat atmosphere-libs shared by every
# sysmodule and overlay project in switch-cfw (mission-control, ldn-mitm,
# sys-patch, sysmodules-overlay, reverse-nx, ryu-ldn-nx, ...).
# Invoked by build.bat in this same folder (and usable standalone from msys2):
#   bash build.sh [target] [jobs] [--dryrun]
#     target  nx_release (default) | nx_debug | nx_audit | clean

source /etc/profile.d/devkit-env.sh

PYTHON_WIN="/c/Users/nik/AppData/Local/Programs/Python/Python312"
export PATH="$PYTHON_WIN:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/libstratosphere" || exit 1

TARGET="${1:-nx_release}"
JOBS="${2:-2}"
DRYRUN=""
[ "${3}" = "--dryrun" ] && DRYRUN="-n"

export PYTHON="$(command -v python)"
echo "PYTHON=$PYTHON"
echo "DEVKITPRO=$DEVKITPRO DEVKITARM=$DEVKITARM"
echo "Target=$TARGET Jobs=$JOBS DryRun=${DRYRUN:-no} CWD=$(pwd)"

if [ "$TARGET" = "clean" ]; then
    make clean
    exit $?
fi

make $DRYRUN -j"$JOBS" "$TARGET"
STATUS=$?

if [ $STATUS -eq 0 ] && [ -z "$DRYRUN" ]; then
    echo "== Build ok. libstratosphere.a in lib/nintendo_nx_arm64_armv8a/release/ =="
    ls -la lib/nintendo_nx_arm64_armv8a/release/ 2>/dev/null

    # Package a drag-and-drop release zip: the zip's own top level exactly
    # mirrors this project's own layout (no wrapper folder), so extracting
    # it directly onto a fresh atmosphere-libs checkout drops the binary in
    # the exact spot the build would have put it. Only for nx_release --
    # see PACKAGING.md at the switch-cfw root for the convention this
    # follows across every project.
    if [ "$TARGET" = "nx_release" ]; then
        ZIPS_DIR="$SCRIPT_DIR/../_ZIPS_"
        PKG="$SCRIPT_DIR/.release-pkg"
        rm -rf "$PKG"
        mkdir -p "$PKG/libstratosphere/lib/nintendo_nx_arm64_armv8a/release"
        cp lib/nintendo_nx_arm64_armv8a/release/libstratosphere.a \
           "$PKG/libstratosphere/lib/nintendo_nx_arm64_armv8a/release/"
        mkdir -p "$ZIPS_DIR"
        ( cd "$PKG" && zip -r -X "$ZIPS_DIR/atmosphere-libs-release.zip" libstratosphere/ >/dev/null )
        rm -rf "$PKG"
        echo "== Packaged: $ZIPS_DIR/atmosphere-libs-release.zip =="
    fi
fi

exit $STATUS
