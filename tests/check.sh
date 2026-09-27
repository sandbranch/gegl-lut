#!/bin/sh
# Builds the operation and the checks of tests/check.c twice, as usual and
# with AddressSanitizer and UndefinedBehaviorSanitizer, and runs the checks
# with both. With the Flatpak GIMP everything builds and runs inside it
# (gimp-plugin-devtools/gimp-build.sh, found next to this repository or at
# $GIMP_BUILD); with GIMP_FLATPAK=0 it uses the system GEGL. Needs no
# network or display; the LUT files are written into a temporary folder.
#
#   tests/check.sh           both builds
#   tests/check.sh quick     only the usual build
#
# The builds and checks run isolated from your folders (GIMP_RUN_HOME,
# tests/output/gimp-home: see gimp-build.sh), so that nothing lands in
# ~/.var/app/org.gimp.GIMP.
# Before and after, it lists your folders of GIMP and the other apps
# (gimp-plugin-devtools/snapshot.sh, skipped without it) and fails if
# anything there changed.
#
# The build folders are in tests/output. Exits with 1 if anything failed.
set -e

here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$here/output"
mkdir -p "$out"
src=$top
GIMP_RUN_HOME=${GIMP_RUN_HOME:-$here/output/gimp-home}
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$out/snapshot-check-before.txt"

gimp_build=${GIMP_BUILD:-$top/../gimp-plugin-devtools/gimp-build.sh}
if [ "${GIMP_FLATPAK:-1}" = 1 ] && command -v flatpak >/dev/null 2>&1 &&
   flatpak info org.gimp.GIMP >/dev/null 2>&1; then
  [ -x "$gimp_build" ] || { echo "no gimp-build.sh at $gimp_build (set GIMP_BUILD)"; exit 1; }
  in_sdk () { "$gimp_build" "$top" "$*"; }
else
  in_sdk () { (cd "$top" && sh -c "$*"); }
fi

# builds into tests/output/<name> with the given meson options
build () {
  name=$1; shift
  if [ -f "$out/$name/build.ninja" ]; then
    in_sdk ninja -C "tests/output/$name" >/dev/null
  else
    in_sdk meson setup "tests/output/$name" "$@" >/dev/null
    in_sdk ninja -C "tests/output/$name" >/dev/null
  fi
}

status=0

echo "== usual build"
build build-check -Dwarning_level=2
rm -rf "$out/tmp-check"
mkdir -p "$out/tmp-check"
in_sdk "TMPDIR=tests/output/tmp-check tests/output/build-check/check \
  tests/output/build-check/color-lookup.so tests/fixtures" \
  >"$out/check.log" 2>&1 || status=1
rm -rf "$out/tmp-check"
grep -E '^(PASS|FAIL|SKIP)|passed,' "$out/check.log" || true
grep -qE '^[0-9]+ passed, 0 failed' "$out/check.log" || status=1

if [ "$1" = quick ]; then
  snapshot_check "$out/snapshot-check-before.txt" "" || status=1
  exit $status
fi

echo
echo "== AddressSanitizer and UndefinedBehaviorSanitizer build"
build build-asan -Db_sanitize=address,undefined -Db_lundef=false
# GEGL, babl and the libraries under them keep caches until the end on
# purpose (types, conversions): a leak counts only when our code made the
# allocation itself, that is, when the first frame after the allocator is
# in color-lookup.c or check.c. GEGL counts leaked buffers itself (checked
# below).
rm -rf "$out/tmp-asan"
mkdir -p "$out/tmp-asan"
in_sdk "ASAN_OPTIONS=detect_leaks=1:exitcode=0 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  TMPDIR=tests/output/tmp-asan LUT_CHECK_KEEP_MODULE=1 \
  tests/output/build-asan/check tests/output/build-asan/color-lookup.so \
  tests/fixtures" \
  >"$out/check-asan.log" 2>&1 || status=1
rm -rf "$out/tmp-asan"

fails=$(grep -c '^FAIL' "$out/check-asan.log" || true)
passes=$(grep -c '^PASS' "$out/check-asan.log" || true)
echo "checks under the sanitizers: $passes passed, $fails failed"
[ "$fails" = 0 ] && [ "$passes" -gt 0 ] || status=1
grep -qE '^[0-9]+ passed, [0-9]+ failed' "$out/check-asan.log" || {
  echo "FAIL  the checks did not finish under the sanitizers"; status=1; }

if grep -qE 'ERROR: AddressSanitizer|runtime error:' "$out/check-asan.log"; then
  echo "FAIL  sanitizer errors, see tests/output/check-asan.log:"
  grep -E -A3 'ERROR: AddressSanitizer|runtime error:' "$out/check-asan.log" | head -20
  status=1
else
  echo "PASS  no memory or undefined behavior errors"
fi

ours=$(awk '
  function done() { if (block != "" && mine) n++; block = ""; mine = 0 }
  /^(Direct|Indirect) leak/ { done(); block = $0; first = 1; next }
  /^ *#[0-9]+ / && block != "" {
    if (first && $0 !~ / in (malloc|calloc|realloc|g_malloc|g_malloc0|g_realloc|g_try_malloc|g_malloc_n|g_malloc0_n|g_realloc_n|g_strdup|g_strndup|g_strdup_printf|g_strdup_vprintf|g_memdup2|g_array_[a-z_]*|g_slice_[a-z_]*) /) {
      first = 0
      if ($0 ~ /(color-lookup|check)\.c/) mine = 1
    }
  }
  END { done(); print n + 0 }' "$out/check-asan.log")
all=$(grep -cE '^(Direct|Indirect) leak' "$out/check-asan.log" || true)
if [ "$ours" = 0 ]; then
  echo "PASS  no leaks in color-lookup ($all leaks inside GEGL and its libraries)"
else
  echo "FAIL  $ours leaks through color-lookup.c or check.c, see tests/output/check-asan.log"
  status=1
fi

if grep -q 'GeglBuffers leaked' "$out/check.log" "$out/check-asan.log"; then
  echo "FAIL  GEGL reports leaked buffers:"
  grep -h 'GeglBuffers leaked' "$out/check.log" "$out/check-asan.log"
  status=1
else
  echo "PASS  no leaked GeglBuffers"
fi

snapshot_check "$out/snapshot-check-before.txt" "" || status=1

echo
[ $status = 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $status
