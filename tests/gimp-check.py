# Runs inside GIMP (tests/gimp-check.sh): applies lut:color-lookup to
# synthetic images as a non-destructive filter, checks that it is one,
# saves the image as XCF with the filter and loads it again, merges the
# filters and compares the pixels with the same operation run in plain
# GEGL in this process. Writes the scene and GIMP's results as float TIFF
# for the comparison with the gegl command line in gimp-check.sh. Prints
# PASS or FAIL per check.
import array
import math
import os
import random

import gi
gi.require_version('Gimp', '3.0')
gi.require_version('Gegl', '0.4')
from gi.repository import Gimp, Gegl, Gio

OUT = os.environ['LUT_CHECK_OUT']
W, H = 256, 160
failed = []


def result(name, ok, msg=''):
    print('%s  gimp_%s%s' % ('PASS' if ok else 'FAIL', name, (': ' + msg) if msg else ''))
    if not ok:
        failed.append(name)


def grade(r, g, b):
    y = 0.3 * r + 0.55 * g + 0.15 * b
    out = []
    for c, v in enumerate((r, g, b)):
        w = y + 1.3 * (v - y) + 0.08 * math.sin(3.0 * (g, b, r)[c])
        out.append(0.5 + 0.5 * math.tanh(2.2 * (w - 0.5)) / math.tanh(1.1))
    return out


def write_cube(path, n):
    with open(path, 'w') as f:
        f.write('TITLE "gimp check"\nLUT_3D_SIZE %d\n' % n)
        for b in range(n):
            for g in range(n):
                for r in range(n):
                    f.write('%.9g %.9g %.9g\n' % tuple(grade(r / (n - 1), g / (n - 1), b / (n - 1))))


def scene():
    rnd = random.Random(1)
    a = array.array('f')
    for y in range(H):
        for x in range(W):
            a.extend((x / (W - 1), y / (H - 1), rnd.random(), 1.0))
    return a


def gegl_result(source, props):
    """lut:color-lookup on the buffer source in plain GEGL, as R'G'B'A float"""
    rect = Gegl.Rectangle.new(0, 0, W, H)
    fmt = 'R\'G\'B\'A float'
    g = Gegl.Node()
    src = g.create_child('gegl:buffer-source')
    src.set_property('buffer', source)
    node = g.create_child('lut:color-lookup')
    for k, v in props.items():
        node.set_property(k, v)
    out = Gegl.Buffer.new(fmt, 0, 0, W, H)
    sink = g.create_child('gegl:write-buffer')
    sink.set_property('buffer', out)
    src.link(node)
    node.link(sink)
    sink.process()
    return array.array('f', out.get(rect, 1.0, fmt, Gegl.AbyssPolicy.NONE))


def save_tiff(path, data):
    rect = Gegl.Rectangle.new(0, 0, W, H)
    buf = Gegl.Buffer.new('R\'G\'B\'A float', 0, 0, W, H)
    buf.set(rect, 'R\'G\'B\'A float', data.tobytes())
    g = Gegl.Node()
    src = g.create_child('gegl:buffer-source')
    src.set_property('buffer', buf)
    save = g.create_child('gegl:tiff-save')
    save.set_property('path', path)
    src.link(save)
    save.process()


def layer_pixels(layer):
    rect = Gegl.Rectangle.new(0, 0, W, H)
    return array.array('f', layer.get_buffer().get(rect, 1.0, 'R\'G\'B\'A float',
                                                   Gegl.AbyssPolicy.NONE))


def max_diff(a, b):
    return max(abs(p - q) for i, (p, q) in enumerate(zip(a, b)))


def new_image(precision, data):
    image = Gimp.Image.new_with_precision(W, H, Gimp.ImageBaseType.RGB, precision)
    layer = Gimp.Layer.new(image, 'scene', W, H, Gimp.ImageType.RGBA_IMAGE, 100,
                           Gimp.LayerMode.NORMAL)
    image.insert_layer(layer, None, 0)
    rect = Gegl.Rectangle.new(0, 0, W, H)
    buf = layer.get_buffer()
    buf.set(rect, 'R\'G\'B\'A float', data.tobytes())
    buf.flush()
    layer.update(0, 0, W, H)
    return image, layer


# GIMP's config takes enum values by their nick, GEGL by their number
ENUMS = {'interpolation': ['tetrahedral', 'trilinear'],
         'encoding': ['perceptual', 'linear'],
         'lut-space': ['image', 'srgb'],
         'out-of-range': ['clamp', 'extrapolate']}


def gegl_props(props):
    return {k: ENUMS[k].index(v) if k in ENUMS else v for k, v in props.items()}


def add_filter(layer, props):
    f = Gimp.DrawableFilter.new(layer, 'lut:color-lookup', 'Color Lookup (LUT)')
    cfg = f.get_config()
    for k, v in props.items():
        cfg.set_property(k, v)
    f.update()
    layer.append_filter(f)
    return f


# GIMP runs the filters in its own process; plain GEGL here, in the
# plug-in's, needs its own start
Gegl.init(None)
lut = os.path.join(OUT, 'grade-17.cube')
write_cube(lut, 17)
data = scene()
save_tiff(os.path.join(OUT, 'scene.tif'), data)

cases = [
    ('float_defaults', Gimp.Precision.FLOAT_NON_LINEAR, {'path': lut}, 1e-5),
    ('float_linear_image', Gimp.Precision.FLOAT_LINEAR, {'path': lut}, 1e-5),
    ('8_bit', Gimp.Precision.U8_NON_LINEAR, {'path': lut}, 0.5 / 255 + 1e-6),
    ('float_options', Gimp.Precision.FLOAT_NON_LINEAR,
     {'path': lut, 'strength': 0.6, 'interpolation': 'trilinear',
      'encoding': 'linear', 'out-of-range': 'extrapolate'}, 1e-5),
]

for label, precision, props, tol in cases:
    image, layer = new_image(precision, data)
    # plain GEGL on the layer's pixels, in the layer's own format
    want = gegl_result(layer.get_buffer(), gegl_props(props))
    if precision == Gimp.Precision.U8_NON_LINEAR:
        # the 8 bit layer holds 0 to 1; the LUT gives a little more
        want = array.array('f', (min(max(v, 0.0), 1.0) for v in want))
    f = add_filter(layer, props)
    names = [x.get_operation_name() for x in layer.get_filters()]
    result(label + '_is_a_filter', names == ['lut:color-lookup'], str(names))

    # the filter survives an XCF save and load, with its settings
    xcf = os.path.join(OUT, label + '.xcf')
    Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, image, Gio.File.new_for_path(xcf), None)
    loaded = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(xcf))
    ll = loaded.get_layers()[0]
    lf = ll.get_filters()
    same = len(lf) == 1 and lf[0].get_operation_name() == 'lut:color-lookup'
    kept = {}
    if same:
        cfg = lf[0].get_config()
        for k, v in props.items():
            got = cfg.get_property(k)
            kept[k] = got
            if isinstance(v, float):
                # XCF keeps the filter's numbers as 32 bit floats
                same = same and abs(got - v) < 1e-6
            else:
                same = same and got == v
    result(label + '_survives_xcf', same, 'settings after loading: %s' % kept)

    # the merged result is the same as plain GEGL, before and after the XCF
    layer.merge_filters()
    got = layer_pixels(layer)
    d = max_diff(got, want)
    result(label + '_same_as_gegl', d <= tol, 'max difference %.2g' % d)
    ll.merge_filters()
    got2 = layer_pixels(ll)
    d2 = max_diff(got2, want)
    result(label + '_same_as_gegl_after_xcf', d2 <= tol, 'max difference %.2g' % d2)
    if label == 'float_defaults' or label == 'float_options':
        save_tiff(os.path.join(OUT, 'gimp-' + label + '.tif'), got)
    image.delete()
    loaded.delete()

# a file that is not there: the image is left as it is, and the reason
# reaches the filter's settings, which the dialog shows
image, layer = new_image(Gimp.Precision.FLOAT_NON_LINEAR, data)
f = add_filter(layer, {'path': os.path.join(OUT, 'missing.cube')})
# render it (the image's projection); GIMP then has the reason in the
# filter's settings. This plug-in's copy of them is cached: they are read
# from the image saved and loaded again
image.pick_color([layer], 1, 1, True, False, 0)
xcf = os.path.join(OUT, 'missing.xcf')
Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, image, Gio.File.new_for_path(xcf), None)
again = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(xcf))
msg = again.get_layers()[0].get_filters()[0].get_config().get_property('error')
again.delete()
result('missing_file_error_in_the_settings', 'missing.cube: the file is not there' in (msg or ''),
       repr(msg))
layer.merge_filters()
d = max_diff(layer_pixels(layer), data)
result('missing_file_leaves_the_image', d == 0, 'max difference %.2g' % d)
image.delete()

print('%d failed' % len(failed))
with open(os.path.join(OUT, 'gimp-check.status'), 'w') as fh:
    fh.write('%d\n' % len(failed))
