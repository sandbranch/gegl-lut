#!/usr/bin/env python3
"""Writes the small LUT files in this folder, which tests/check.c reads:
each exercises a part of a format as other programs write it. They are
made here, not taken from anywhere. Run from any folder:

  python3 tests/fixtures/make-fixtures.py
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def write(name, text, newline='\n'):
    with open(os.path.join(HERE, name), 'w', newline=newline) as f:
        f.write(text)


def fmt(v):
    return ('%.6f' % v).rstrip('0').rstrip('.') if v else '0'


# a Resolve .cube with a 1D shaper and a 3D LUT: the shaper inverts, the
# 3D LUT inverts again, so the file is the identity
lines = ['# Resolve style: a 1D shaper, then a 3D LUT',
         'LUT_1D_SIZE 4', 'LUT_1D_INPUT_RANGE 0.0 1.0',
         'LUT_3D_SIZE 3', 'LUT_3D_INPUT_RANGE 0.0 1.0']
for i in range(4):
    v = fmt(1 - i / 3)
    lines.append('%s %s %s' % (v, v, v))
for b in range(3):
    for g in range(3):
        for r in range(3):
            lines.append('%s %s %s' % (fmt(1 - r / 2), fmt(1 - g / 2), fmt(1 - b / 2)))
write('resolve-shaper.cube', '\n'.join(lines) + '\n')

# an Adobe .cube with Windows line ends, comments and a domain: red takes
# green, green blue, blue red
lines = ['# channel swap', 'TITLE "swap, CRLF"', '', 'LUT_3D_SIZE 2',
         'DOMAIN_MIN 0.0 0.0 0.0', 'DOMAIN_MAX 1.0 1.0 1.0', '# the table']
for b in range(2):
    for g in range(2):
        for r in range(2):
            lines.append('%d.0 %d.0 %d.0' % (g, b, r))
write('swap-crlf.cube', '\n'.join(lines) + '\n', newline='\r\n')

# a 1D .cube: x^2 at five points
lines = ['TITLE "square"', 'LUT_1D_SIZE 5']
for i in range(5):
    v = fmt((i / 4) ** 2)
    lines.append('%s %s %s' % (v, v, v))
write('square-1d.cube', '\n'.join(lines) + '\n')

# a Lustre .3dl: 3DMESH, "Mesh <input bits> <output bits>", a 10 bit
# input grid, 12 bit values with blue changing fastest, and the trailer
n = 5
lines = ['#Tokens required by applications - do not edit', '3DMESH',
         'Mesh 2 12', ' '.join(str(int(i * 1023 / (n - 1) + 0.5)) for i in range(n)), '']
for r in range(n):
    for g in range(n):
        for b in range(n):
            lines.append(' '.join(str(int(v / (n - 1) * 4095 + 0.5)) for v in (r, g, b)))
lines += ['', '#Tokens required by applications - do not edit', '', 'LUT8', 'gamma 1.0']
write('lustre-identity-5.3dl', '\n'.join(lines) + '\n')

# a Flame .3dl of three points per axis, whose grid line reads like a
# triplet, with 10 bit values: the channel swap
n = 3
lines = ['# Flame style, 3 points', '0 512 1023', '']
for r in range(n):
    for g in range(n):
        for b in range(n):
            lines.append(' '.join(str(int(v / (n - 1) * 1023 + 0.5)) for v in (g, b, r)))
write('flame-swap-3.3dl', '\n'.join(lines) + '\n')


# a Hald CLUT of level 2 (8 x 8 pixels, 4 points per axis), 16 bit PNG,
# inverting: pixel i holds the point r = i % 4, g = i / 4 % 4, b = i / 16
def png16(path, w, h, rgb):
    raw = b''
    for y in range(h):
        raw += b'\0' + b''.join(struct.pack('>HHH', *rgb[y * w + x]) for x in range(w))

    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 16, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))


n = 4
px = [tuple(65535 - (c * 65535) // (n - 1) for c in (i % n, i // n % n, i // (n * n)))
      for i in range(n ** 3)]
png16(os.path.join(HERE, 'hald-2-invert.png'), 8, 8, px)
