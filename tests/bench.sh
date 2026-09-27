#!/bin/sh
# How long the operation takes on a 24 megapixel image (tests/bench.c),
# with the build in build/ (or $BUILD), inside the Flatpak SDK with the
# Flatpak GIMP or natively with GIMP_FLATPAK=0.
#   tests/bench.sh                 6000 x 4000
#   tests/bench.sh 3000 2000       another size
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
build=${BUILD:-build}
[ -x "$top/$build/bench" ] || { echo "no $build/bench: build first (README)" >&2; exit 2; }
gimp_build=${GIMP_BUILD:-$top/../gimp-plugin-devtools/gimp-build.sh}
if [ "${GIMP_FLATPAK:-1}" = 1 ] && command -v flatpak >/dev/null 2>&1 &&
   flatpak info org.gimp.GIMP >/dev/null 2>&1; then
  exec "$gimp_build" "$top" "$build/bench" "$build/color-lookup.so" "$@"
fi
cd "$top" && exec "$build/bench" "$build/color-lookup.so" "$@"
