#!/usr/bin/env python3
"""Offscreen GPU 3D smoke validation (not a performance benchmark)."""
from pathlib import Path
import sys
if len(sys.argv)!=2:raise SystemExit('Usage: check-vulkan3d-result.py screenshot.ppm')
p=Path(sys.argv[1]);f=p.open('rb')
with f:
    magic=f.readline().strip();wh=f.readline().split();maxval=f.readline().strip()
    if magic!=b'P6' or wh!=[b'800',b'450'] or maxval!=b'255':
        raise SystemExit('Unexpected screenshot dimensions or PPM header')
    data=f.read()
if len(data)!=800*450*3:raise SystemExit('Truncated/oversized image')
def at(x,y):return tuple(data[(y*800+x)*3:(y*800+x)*3+3])
corner=at(4,4)
centre=at(400,220)
if max(corner)-min(corner)<5:raise SystemExit(f'Background not initialized: {corner}')
if sum(abs(centre[i]-corner[i]) for i in range(3))<45:
    raise SystemExit(f'Center triangle missing: {centre} vs {corner}')
print(f'PASS: GPU triangle screenshot 800x450, background={corner} center={centre}')
