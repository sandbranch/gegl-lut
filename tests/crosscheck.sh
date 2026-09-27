#!/bin/sh
# Compares the operation with FFmpeg's lut3d, haldclut and lut1d filters on
# a generated image and generated LUTs (tests/crosscheck.py). FFmpeg and
# numpy run on this machine; the operation runs through tests/lut-apply.c,
# built in build/ (or $BUILD), inside the Flatpak SDK with the Flatpak GIMP
# or natively with GIMP_FLATPAK=0. Writes to tests/output/crosscheck.
# The operation runs isolated from your folders (GIMP_RUN_HOME,
# tests/output/gimp-home: see gimp-build.sh).
# Before and after, it lists your folders of GIMP and the other apps
# (gimp-plugin-devtools/snapshot.sh, skipped without it) and fails if
# anything there changed.
# Exits non-zero if a case fails, 2 if ffmpeg or numpy are missing.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
build=${BUILD:-build}
out="$here/output/crosscheck"

command -v ffmpeg >/dev/null 2>&1 && ffmpeg -hide_banner -filters 2>/dev/null |
  grep -q ' lut3d ' || { echo "SKIP  no ffmpeg with the lut3d filter"; exit 2; }
python3 -c 'import numpy' 2>/dev/null || { echo "SKIP  no numpy"; exit 2; }
[ -x "$top/$build/lut-apply" ] || { echo "no $build/lut-apply: build first (README)" >&2; exit 2; }
rm -rf "$out"
mkdir -p "$out"
src=$top
GIMP_RUN_HOME=${GIMP_RUN_HOME:-$here/output/gimp-home}
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$here/output/snapshot-cross-before.txt"

gimp_build=${GIMP_BUILD:-$top/../gimp-plugin-devtools/gimp-build.sh}
if [ "${GIMP_FLATPAK:-1}" = 1 ] && command -v flatpak >/dev/null 2>&1 &&
   flatpak info org.gimp.GIMP >/dev/null 2>&1; then
  apply="$gimp_build $top $build/lut-apply $build/color-lookup.so"
else
  apply="$top/$build/lut-apply $top/$build/color-lookup.so"
fi
rc=0
LUT_APPLY="$apply" python3 "$here/crosscheck.py" "$out" || rc=$?
snapshot_check "$here/output/snapshot-cross-before.txt" "" || rc=1
exit $rc
