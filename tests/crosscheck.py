#!/usr/bin/env python3
"""Compares lut:color-lookup with an independent implementation, FFmpeg's
lut3d, haldclut and lut1d filters (libavfilter/vf_lut3d.c), on a generated
image and generated LUTs: .cube files of 17, 33 and 65 points, a Hald CLUT
of level 8, and a 1D .cube; tetrahedral and trilinear. Both run in 32 bit
float (FFmpeg's gbrpf32le). Prints the mean and largest difference per
case, PASS within the tolerance; run by tests/crosscheck.sh, which sets
LUT_APPLY to the command that runs tests/lut-apply.c.

Needs numpy and ffmpeg on the PATH; no network.
"""
import math
import os
import shlex
import struct
import subprocess
import sys
import zlib

import numpy as np

OUT = sys.argv[1]
APPLY = shlex.split(os.environ['LUT_APPLY'])
W, H = 512, 384
TOL = 2e-6          # both in float, the same interpolation
failed = 0


def grade(rgb):
    """a smooth, colorful grade within 0 to 1 (as in tests/check.c)"""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.3 * r + 0.55 * g + 0.15 * b
    out = np.empty_like(rgb)
    for c, nxt in ((0, g), (1, b), (2, r)):
        v = y + 1.3 * (rgb[..., c] - y) + 0.08 * np.sin(3.0 * nxt)
        out[..., c] = 0.5 + 0.5 * np.tanh(2.2 * (v - 0.5)) / math.tanh(1.1)
    return np.clip(out, 0.0, 1.0)


def grid(n):
    """the points of an n^3 LUT, red fastest"""
    i = np.arange(n) / (n - 1)
    b, g, r = np.meshgrid(i, i, i, indexing='ij')
    return np.stack([r, g, b], axis=-1).reshape(-1, 3)


def write_cube(path, n):
    with open(path, 'w') as f:
        f.write('TITLE "crosscheck"\nLUT_3D_SIZE %d\n' % n)
        np.savetxt(f, grade(grid(n)), fmt='%.9g')


def write_cube_1d(path, n):
    x = np.linspace(0, 1, n)
    with open(path, 'w') as f:
        f.write('LUT_1D_SIZE %d\n' % n)
        np.savetxt(f, np.stack([x ** 2.2, np.sqrt(x), 1 - x * x], axis=-1), fmt='%.9g')


def write_hald(path, level):
    """a 16 bit PNG, and the same values as float planes for FFmpeg"""
    n = level * level
    w = level ** 3
    codes = np.round(grade(grid(n)) * 65535)
    exact = (codes / 65535).reshape(w, w, 3)
    np.stack([exact[..., 1], exact[..., 2], exact[..., 0]]).astype('<f4').tofile(path + '.raw')
    px = codes.astype('>u2').reshape(w, w * 3)
    raw = b''.join(b'\0' + row.tobytes() for row in px)

    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, w, 16, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def image():
    """smooth ramps, a gray axis and random colors, all within 0 to 1"""
    rnd = np.random.default_rng(3)
    y, x = np.mgrid[0:H, 0:W]
    img = np.stack([x / (W - 1), y / (H - 1), 0.5 + 0.5 * np.sin(x * 0.05 + y * 0.03)], axis=-1)
    img[H // 2:H // 2 + 16] = (np.arange(W) / (W - 1))[None, :, None]
    img[-96:] = rnd.random((96, W, 3))
    return img.astype(np.float32)


def ffmpeg(args, out_raw):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y'] + args +
                   ['-f', 'rawvideo', '-pix_fmt', 'gbrpf32le', out_raw], check=True)
    p = np.fromfile(out_raw, dtype='<f4').reshape(3, H, W)
    return np.stack([p[2], p[0], p[1]], axis=-1)      # planes G, B, R


def ours(props):
    out = os.path.join(OUT, 'ours.raw')
    subprocess.run(APPLY + [os.path.join(OUT, 'in-rgb.raw'), out, str(W), str(H)] + props,
                   check=True)
    return np.fromfile(out, dtype=np.float32).reshape(H, W, 3)


def compare(name, a, b):
    global failed
    d = np.abs(a.astype(np.float64) - b.astype(np.float64))
    ok = d.max() <= TOL
    failed += not ok
    print('%s  crosscheck_%s: mean difference %.2g, largest %.2g'
          % ('PASS' if ok else 'FAIL', name, d.mean(), d.max()))


img = image()
img.tofile(os.path.join(OUT, 'in-rgb.raw'))
planar = np.stack([img[..., 1], img[..., 2], img[..., 0]])
planar.astype('<f4').tofile(os.path.join(OUT, 'in-gbrp.raw'))
src = ['-f', 'rawvideo', '-pix_fmt', 'gbrpf32le', '-s', '%dx%d' % (W, H),
       '-i', os.path.join(OUT, 'in-gbrp.raw')]
ffout = os.path.join(OUT, 'ffmpeg.raw')

for n in (17, 33, 65):
    cube = os.path.join(OUT, 'grade-%d.cube' % n)
    write_cube(cube, n)
    for interp in ('tetrahedral', 'trilinear'):
        ref = ffmpeg(src + ['-vf', 'lut3d=file=%s:interp=%s' % (cube, interp)], ffout)
        compare('cube_%d_%s' % (n, interp), ours(['path=' + cube, 'interpolation=' + interp]), ref)

# the Hald CLUT goes to FFmpeg as float planes: its own conversion of a
# 16 bit PNG to float is off by up to about 5e-4 (printed below)
hald = os.path.join(OUT, 'grade-hald-8.png')
write_hald(hald, 8)
exact = np.fromfile(hald + '.raw', dtype='<f4').reshape(3, 512, 512)
subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', hald,
                '-f', 'rawvideo', '-pix_fmt', 'gbrpf32le', os.path.join(OUT, 'hald-ff.raw')],
               check=True)
conv = np.abs(np.fromfile(os.path.join(OUT, 'hald-ff.raw'), dtype='<f4').reshape(3, 512, 512)
              .astype(np.float64) - exact)
print('info  ffmpeg reads the 16 bit PNG with differences of mean %.2g, largest %.2g'
      % (conv.mean(), conv.max()))
clut = ['-f', 'rawvideo', '-pix_fmt', 'gbrpf32le', '-s', '512x512', '-i', hald + '.raw']
for interp in ('tetrahedral', 'trilinear'):
    ref = ffmpeg(src + clut + ['-filter_complex', '[0][1]haldclut=interp=%s' % interp], ffout)
    compare('hald_8_%s' % interp, ours(['path=' + hald, 'interpolation=' + interp]), ref)

cube1 = os.path.join(OUT, 'curves-1d.cube')
write_cube_1d(cube1, 1024)
ref = ffmpeg(src + ['-vf', 'lut1d=file=%s:interp=linear' % cube1], ffout)
compare('cube_1d_1024_linear', ours(['path=' + cube1]), ref)

print('%d failed' % failed)
sys.exit(1 if failed else 0)
