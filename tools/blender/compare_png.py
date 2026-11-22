#!/usr/bin/env python3
"""Compares two 8-bit PNGs (RGB or RGBA) pixel by pixel, without any image library.
usage: compare_png.py a.png b.png [diff.ppm]"""
import struct, sys, zlib

def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat = 8, b''
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        c = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT':
            idat += c
        pos += 12 + n
    assert depth == 8 and ctype in (2, 6), (depth, ctype)
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    rows, prev, p = [], bytearray(w * bpp), 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + w * bpp]); p += 1 + w * bpp
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line); prev = line
    return w, h, bpp, rows

w, h, bpp, a = load(sys.argv[1])
w2, h2, bpp2, b = load(sys.argv[2])
if (w, h) != (w2, h2):
    sys.exit(f'sizes differ: {w}x{h} and {w2}x{h2}')
hist, worst, sq, npx = {}, (0, 0, 0), 0, 0
out = bytearray()
for y in range(h):
    for x in range(w):
        m = 0
        for c in range(3):
            e = abs(a[y][x * bpp + c] - b[y][x * bpp2 + c])
            sq += e * e
            m = max(m, e)
        hist[m] = hist.get(m, 0) + 1
        if m > worst[0]:
            worst = (m, x, y)
        npx += m > 0
        out += bytes((min(255, m * 32),)) * 3
print(f'{w}x{h}: {npx} of {w * h} pixels differ, largest difference {worst[0]}/255 at {worst[1:]}, '
      f'RMS {(sq / (w * h * 3)) ** .5:.4f}')
print('differences by size:', ' '.join(f'{k}:{v}' for k, v in sorted(hist.items())))
if len(sys.argv) > 3:
    with open(sys.argv[3], 'wb') as f:
        f.write(b'P6 %d %d 255\n' % (w, h) + out)
