#!/usr/bin/env python3
"""Check exact pixels from actual Vulkan 0.8 image readback (never synthesized)."""
from pathlib import Path
import sys
if len(sys.argv)!=2:raise SystemExit('Usage: check-vulkan-result.py /path/to/qrx-vulkan-0.8.ppm')
p=Path(sys.argv[1]); b=p.read_bytes()
header=b'P6\n800 450\n255\n'
if not b.startswith(header) or len(b)!=len(header)+800*450*3:
    raise SystemExit('FAIL: wrong PPM header or length')
def pixel(x,y):
    o=len(header)+(y*800+x)*3
    return tuple(b[o:o+3])
for name,x,y,expected in (
    ('clear background',0,0,(19,35,73)),
    ('clear after rect',799,449,(19,35,73)),
    ('GPU rectangle',100,80,(255,136,0)),
    ('GPU rectangle interior',200,100,(255,136,0)),
    ('GPU image red',10,10,(255,0,0)),
    ('GPU image green',11,10,(0,255,0)),
    ('GPU image blue',10,11,(0,0,255)),
    ('GPU image white',11,11,(255,255,255)),
):
    actual=pixel(x,y)
    if actual!=expected:
        raise SystemExit(f'FAIL {name}: {actual} != {expected}')
    print('PASS',name,actual)
print('Vulkan GPU offscreen/readback pixel validation PASS')
