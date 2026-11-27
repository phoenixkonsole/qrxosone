#!/usr/bin/env python3
"""ThunderCat for AROS: the logo (thundercat-logo-source.png, a cat curled around a globe with a
lightning bolt, on white) with a transparent background, in the sizes Gecko's branding and the App
Store use: thundercat-<size>.png, thundercat.svg (the 512 one inside an SVG, for about-logo.svg) and
wordmark.svg. The white around the logo becomes transparent: the white area that touches the image's
border, with a soft edge.
Usage: make-logo.py <out dir>   (needs Pillow, numpy, scipy)"""
import base64, io, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage

here = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
src = Image.open(os.path.join(here, "thundercat-logo-source.png")).convert("RGB")
rgb = np.asarray(src).astype(np.float32)
lo, hi = rgb.min(axis=2), rgb.max(axis=2)
# white or very light grey, not coloured
whiteish = (lo > 228) & (hi - lo < 24)
labels, _ = ndimage.label(whiteish)
border = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
bg = np.isin(labels, border[border > 0])
# the edge: a few pixels around the background, faded by how light they are
edge = ndimage.binary_dilation(bg, iterations=3) & ~bg
alpha = np.ones(lo.shape, np.float32)
alpha[bg] = 0.0
a_edge = np.clip((255.0 - lo) / 60.0, 0.0, 1.0)
alpha[edge] = a_edge[edge]
# take the white out of the edge's colour (un-blend from white)
a3 = np.maximum(alpha, 1e-3)[..., None]
col = np.where(edge[..., None], np.clip((rgb - 255.0 * (1.0 - a3)) / a3, 0, 255), rgb)
rgba = np.dstack([col, alpha * 255.0]).astype(np.uint8)
img = Image.fromarray(rgba, "RGBA")
# square around what is left, with a small margin
x0, y0, x1, y1 = img.getbbox()
side = max(x1 - x0, y1 - y0)
side = int(side * 1.04)
sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
sq.paste(img.crop((x0, y0, x1, y1)), ((side - (x1 - x0)) // 2, (side - (y1 - y0)) // 2))
for s in (16, 22, 24, 32, 48, 64, 128, 192, 256, 384, 512):
    sq.resize((s, s), Image.LANCZOS).save(os.path.join(out, "thundercat-%d.png" % s), optimize=True)
buf = io.BytesIO()
sq.resize((512, 512), Image.LANCZOS).save(buf, "PNG", optimize=True)
svg = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="512" '
       'height="512" viewBox="0 0 512 512"><image width="512" height="512" href="data:image/png;base64,'
       + base64.b64encode(buf.getvalue()).decode() + '"/></svg>\n')
open(os.path.join(out, "thundercat.svg"), "w").write(svg)
open(os.path.join(out, "wordmark.svg"), "w").write(
    '<svg xmlns="http://www.w3.org/2000/svg" width="320" height="56" viewBox="0 0 320 56">\n'
    '<text x="0" y="42" font-family="Bitstream Vera Sans, sans-serif" font-size="40" font-weight="bold" '
    'fill="context-fill">ThunderCat</text>\n</svg>\n')
print("written to", out)
