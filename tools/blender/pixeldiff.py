#!/usr/bin/env python3
"""Lists the pixels that differ between two 8-bit PNGs, with their values."""
import importlib.util, sys
spec = importlib.util.spec_from_file_location('cmp', __file__.replace('pixeldiff.py', 'compare_png.py'))
src = open(spec.origin).read().split("w, h, bpp, a = load")[0]
ns = {}
exec(src, ns)
w, h, bpp, a = ns['load'](sys.argv[1])
_, _, bpp2, b = ns['load'](sys.argv[2])
for y in range(h):
    for x in range(w):
        if a[y][x * bpp:x * bpp + 3] != b[y][x * bpp2:x * bpp2 + 3]:
            print(x, y, tuple(a[y][x * bpp:x * bpp + bpp]), tuple(b[y][x * bpp2:x * bpp2 + bpp2]))
