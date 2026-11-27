#!/usr/bin/env python3
"""Reads the screenshots of a qrx_ec64_run --capture-at series (cap-TTTT.ppm, T in seconds) and says
when a browser window's content appeared and when it stopped changing.
Usage: ec64bench.py DIR [x0 y0 x1 y1]   the content area on the screen (default: ThunderCat's first
       window at 60,40: 62,128-1058,738)
For each picture: the share of the area that is not the empty page (near white or near the window's
grey) and the share that changed since the picture before. Then:
  first content   the first picture with more than 5% of the area drawn
  settled         the first picture after which the area changes by less than 0.5% until the end
The pictures are taken every few seconds, so both times are up to one interval late."""
import glob, os, re, sys
from PIL import Image, ImageChops

d = sys.argv[1]
box = tuple(int(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else (62, 128, 1058, 738)
rows = []
prev = None
for f in sorted(glob.glob(os.path.join(d, 'cap-*.p*'))):
    t = int(re.search(r'cap-(\d+)', f).group(1))
    full = Image.open(f).convert('RGB')
    # the window is there when its toolbar (the strip above the content) is light
    bar = list(full.crop((box[0] + 10, box[1] - 30, box[2] - 10, box[1] - 20)).getdata())
    window = sum(1 for r, g, b in bar if r > 200 and g > 200 and b > 200) / len(bar) > 0.5
    im = full.crop(box).resize(((box[2] - box[0]) // 4, (box[3] - box[1]) // 4))
    px = list(im.getdata())
    drawn = 0.0 if not window else sum(1 for r, g, b in px if not (r > 235 and g > 235 and b > 235)) / len(px)
    if prev is None:
        changed = 0.0
    else:
        diff = ImageChops.difference(im, prev).convert('L')
        changed = sum(1 for v in diff.getdata() if v > 24) / len(px)
    rows.append((t, drawn, changed, window))
    prev = im
win = next((t for t, dr, ch, w in rows if w), None)
first = next((t for t, dr, ch, w in rows if dr > 0.05), None)
settled = None
for i, (t, dr, ch, w) in enumerate(rows):
    if dr > 0.05 and all(c < 0.005 for _, _, c, _ in rows[i + 1:]):
        settled = t
        break
for t, dr, ch, w in rows:
    print('%5d s  %s  drawn %5.1f%%  changed %5.1f%%' % (t, 'window' if w else '      ', 100 * dr, 100 * ch))
print('window: %s s, first content: %s s, settled: %s s' % (win, first, settled))
