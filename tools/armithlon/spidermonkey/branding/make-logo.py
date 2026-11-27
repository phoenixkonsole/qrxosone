#!/usr/bin/env python3
"""ThunderCat for AROS: the logo (a cat curled around a globe, a lightning bolt on the globe) as SVG,
and the PNGs Gecko's branding and the App Store use. Original artwork for QRX, not Mozilla's.
Usage: make-logo.py <out dir>   (needs cairosvg)"""
import math, os, sys
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
C = (256, 276)            # globe centre
R = 158                   # globe radius

def pt(r, deg):
    a = math.radians(deg)
    return C[0] + r * math.cos(a), C[1] + r * math.sin(a)

def band(r_in, r_out_a, r_out_b, a0, a1, steps=48):
    """a band around the globe from angle a0 to a1 (degrees, clockwise on screen); its outer radius
    goes from r_out_a to r_out_b (a taper)"""
    outer = [pt(r_out_a + (r_out_b - r_out_a) * i / steps, a0 + (a1 - a0) * i / steps) for i in range(steps + 1)]
    inner = [pt(r_in, a0 + (a1 - a0) * i / steps) for i in range(steps, -1, -1)]
    pts = outer + inner
    return "M" + " L".join("%.1f %.1f" % p for p in pts) + " Z"

# one body from the neck (top left) over the back down to the tail's tip (bottom left), thinner
# towards the tail
body = band(R - 8, R + 46, R + 8, -150, 168, steps=96)
tail = ""
hx, hy = pt(R + 30, -150)                             # the head, top left
px1, py1 = pt(R - 2, -112)                            # front paws on the globe
px2, py2 = pt(R - 2, -92)
head = f'''<ellipse cx="{hx:.1f}" cy="{hy:.1f}" rx="66" ry="56"/>
  <path d="M{hx-56:.1f} {hy-14:.1f} Q{hx-70:.1f} {hy-70:.1f} {hx-58:.1f} {hy-96:.1f} Q{hx-30:.1f} {hy-72:.1f} {hx-10:.1f} {hy-50:.1f} Z"/>
  <path d="M{hx+12:.1f} {hy-52:.1f} Q{hx+34:.1f} {hy-80:.1f} {hx+58:.1f} {hy-98:.1f} Q{hx+68:.1f} {hy-60:.1f} {hx+60:.1f} {hy-16:.1f} Z"/>
  <ellipse cx="{px1:.1f}" cy="{py1:.1f}" rx="20" ry="13" transform="rotate(-25 {px1:.1f} {py1:.1f})"/>
  <ellipse cx="{px2:.1f}" cy="{py2:.1f}" rx="20" ry="13" transform="rotate(-5 {px2:.1f} {py2:.1f})"/>'''
tx, ty = pt(R + 8, 168)
tip = f'<circle cx="{tx:.1f}" cy="{ty:.1f}" r="9"/>'
eyes = f'''<path d="M{hx-50:.1f} {hy-22:.1f} Q{hx-58:.1f} {hy-62:.1f} {hx-52:.1f} {hy-78:.1f} Q{hx-34:.1f} {hy-60:.1f} {hx-24:.1f} {hy-46:.1f} Z" fill="#ff8fc8" opacity="0.7"/>
  <path d="M{hx+24:.1f} {hy-48:.1f} Q{hx+38:.1f} {hy-66:.1f} {hx+52:.1f} {hy-80:.1f} Q{hx+56:.1f} {hy-56:.1f} {hx+52:.1f} {hy-24:.1f} Z" fill="#ff8fc8" opacity="0.7"/>
  <ellipse cx="{hx+2:.1f}" cy="{hy+22:.1f}" rx="26" ry="18" fill="#d9b3ff" opacity="0.55"/>
  <g stroke="#f4e8ff" stroke-width="2.5" stroke-linecap="round" opacity="0.85">
    <line x1="{hx-14:.1f}" y1="{hy+22:.1f}" x2="{hx-60:.1f}" y2="{hy+14:.1f}"/><line x1="{hx-14:.1f}" y1="{hy+27:.1f}" x2="{hx-58:.1f}" y2="{hy+32:.1f}"/>
    <line x1="{hx+18:.1f}" y1="{hy+22:.1f}" x2="{hx+64:.1f}" y2="{hy+14:.1f}"/><line x1="{hx+18:.1f}" y1="{hy+27:.1f}" x2="{hx+62:.1f}" y2="{hy+32:.1f}"/>
  </g>
  <path d="M{hx-34:.1f} {hy-2:.1f} q14 -14 28 0 q-14 10 -28 0 Z" fill="#ffe14d"/>
  <path d="M{hx+10:.1f} {hy-4:.1f} q14 -14 28 0 q-14 10 -28 0 Z" fill="#ffe14d"/>
  <ellipse cx="{hx-20:.1f}" cy="{hy-3:.1f}" rx="3" ry="6" fill="#1a0d33"/>
  <ellipse cx="{hx+24:.1f}" cy="{hy-5:.1f}" rx="3" ry="6" fill="#1a0d33"/>
  <path d="M{hx-2:.1f} {hy+14:.1f} l8 0 l-4 6 Z" fill="#ff8fc8"/>'''
# meridians and parallels of the globe
grid = []
for k in (-0.6, -0.2, 0.2, 0.6):
    y = C[1] + k * R
    w = math.sqrt(R * R - (k * R) ** 2)
    grid.append(f'<ellipse cx="{C[0]}" cy="{y:.1f}" rx="{w:.1f}" ry="{w*0.12:.1f}"/>')
for k in (0.35, 0.75):
    grid.append(f'<ellipse cx="{C[0]}" cy="{C[1]}" rx="{R*k:.1f}" ry="{R}"/>')
grid.append(f'<line x1="{C[0]}" y1="{C[1]-R}" x2="{C[0]}" y2="{C[1]+R}"/>')
bolt = "M282 160 L214 292 L262 292 L234 400 L318 252 L268 252 L300 160 Z"
land = ("M170 230 q20 -30 55 -20 q20 10 10 35 q-15 25 -45 20 q-30 -5 -20 -35 Z "
        "M300 320 q30 -15 55 5 q10 30 -20 45 q-35 10 -45 -15 q-5 -20 10 -35 Z "
        "M330 190 q25 -5 35 15 q-5 20 -30 18 q-20 -8 -5 -33 Z")

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">
<defs>
  <radialGradient id="sea" cx="38%" cy="32%" r="75%">
    <stop offset="0" stop-color="#4fb3ff"/><stop offset="0.55" stop-color="#1e5bd8"/><stop offset="1" stop-color="#0b1f63"/>
  </radialGradient>
  <linearGradient id="fur" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#b44dff"/><stop offset="0.5" stop-color="#7a22e0"/><stop offset="1" stop-color="#3d0f8c"/>
  </linearGradient>
  <linearGradient id="bolt" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#fff6a8"/><stop offset="1" stop-color="#ffb300"/>
  </linearGradient>
</defs>
<circle cx="{C[0]}" cy="{C[1]}" r="{R}" fill="url(#sea)"/>
<path d="{land}" fill="#38d6a8" opacity="0.85"/>
<g fill="none" stroke="#9fd4ff" stroke-opacity="0.45" stroke-width="3">{"".join(grid)}</g>
<path d="{bolt}" fill="url(#bolt)" stroke="#7a3d00" stroke-width="4" stroke-linejoin="round"/>
<g fill="url(#fur)">
  <path d="{body}"/>
  {tip}
  {head}
</g>
{eyes}
</svg>
'''
open(os.path.join(out, "thundercat.svg"), "w").write(svg)
wordmark = '''<svg xmlns="http://www.w3.org/2000/svg" width="320" height="56" viewBox="0 0 320 56">
<text x="0" y="42" font-family="Bitstream Vera Sans, sans-serif" font-size="40" font-weight="bold" fill="context-fill">ThunderCat</text>
</svg>
'''
open(os.path.join(out, "wordmark.svg"), "w").write(wordmark)
import cairosvg
for s in (16, 22, 24, 32, 48, 64, 128, 192, 256, 384, 512):
    cairosvg.svg2png(bytestring=svg.encode(), write_to=os.path.join(out, "thundercat-%d.png" % s),
                     output_width=s, output_height=s)
print("written to", out)
