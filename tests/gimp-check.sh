#!/bin/sh
# The operation as a non-destructive filter in the Flatpak GIMP, without a
# window (tests/gimp-check.py): on float and 8 bit images, through an XCF
# save and load, against plain GEGL; then GIMP's result against the gegl
# command line. Uses the module from build/ (or $BUILD) and a throwaway
# GIMP profile in tests/output/gimp-profile (GIMP3_DIRECTORY), so the
# installed filters and the user's GIMP settings are not touched. The
# first run takes about a minute (GIMP sets up the new profile). Exits
# non-zero if a check fails.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
build=${BUILD:-$top/build}
case $build in /*) ;; *) build=$top/$build ;; esac
out="$here/output/gimp-check"
mod="$here/output/gimp-check-modules"
[ -f "$build/color-lookup.so" ] ||
  { echo "no $build/color-lookup.so: build first (README)" >&2; exit 2; }
rm -rf "$mod" "$out"
mkdir -p "$mod" "$out" "$here/output/gimp-profile"
# GEGL loads every file in a module folder: only the module
cp "$build/color-lookup.so" "$mod/"

run () {
  flatpak run --filesystem="$top" \
    --env=GIMP3_DIRECTORY="$here/output/gimp-profile" \
    --env=GEGL_PATH="$mod:/app/lib/gegl-0.4" \
    --env=LUT_CHECK_OUT="$out" "$@"
}

run --command=gimp-console-3.2 org.gimp.GIMP --no-interface --no-data --no-fonts \
  --batch-interpreter python-fu-eval \
  -b "exec(open('$here/gimp-check.py').read())" --quit 2>&1 |
  grep -E "^(PASS|FAIL)|failed$|Error|Traceback|^  File" || true
[ "$(cat "$out/gimp-check.status" 2>/dev/null)" = 0 ] || status=1

# the same settings on the gegl command line
lut="$out/grade-17.cube"
run --command=gegl org.gimp.GIMP "$out/scene.tif" -o "$out/cli-float_defaults.tif" -- \
  lut:color-lookup path="$lut"
run --command=gegl org.gimp.GIMP "$out/scene.tif" -o "$out/cli-float_options.tif" -- \
  lut:color-lookup path="$lut" strength=0.6 interpolation=trilinear \
  encoding=linear out-of-range=extrapolate

run --command=python3 org.gimp.GIMP - <<EOF || status=1
import array
import gi
gi.require_version('Gegl', '0.4')
from gi.repository import Gegl

Gegl.init(None)
bad = 0

def load(path):
    g = Gegl.Node()
    n = g.create_child('gegl:tiff-load')
    n.set_property('path', path)
    r = n.get_bounding_box()
    buf = Gegl.Buffer.new("R'G'B'A float", r.x, r.y, r.width, r.height)
    w = g.create_child('gegl:write-buffer')
    w.set_property('buffer', buf)
    n.link(w)
    w.process()
    return array.array('f', buf.get(r, 1.0, "R'G'B'A float", Gegl.AbyssPolicy.NONE))

for label in ('float_defaults', 'float_options'):
    a = load('$out/gimp-' + label + '.tif')
    b = load('$out/cli-' + label + '.tif')
    d = max(abs(p - q) for p, q in zip(a, b)) if len(a) == len(b) and a else float('inf')
    ok = d <= 1e-5
    bad += not ok
    print('%s  gimp_%s_same_as_gegl_command_line: max difference %.2g'
          % ('PASS' if ok else 'FAIL', label, d))
raise SystemExit(bad)
EOF

exit ${status:-0}
