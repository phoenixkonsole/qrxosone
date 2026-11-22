#!/usr/bin/env python3
"""The QRX wallpapers and the icons of the four desktop designs, on Linux.

A port of gen-assets.swift (CoreGraphics, macOS only) to Python with Pillow and numpy, drawing
the same designs from the same numbers: Convergence, Horizon and Aurora, and the seven icon kinds
for the qrx, windows, mac and amiga themes. Avenir Next is a macOS font; the Inter family
(SIL Open Font License, /usr/share/fonts/opentype/inter, Ubuntu: fonts-inter) stands in.

  gen-assets.py OUTDIR [png]
OUTDIR/Wallpapers/<design>-<w>x<h>.qrxwp: "QRXW", width, height (big endian), then RGB bytes.
OUTDIR/Icons/<theme>/<kind>.argb: "QRXI", width, height, then A R G B bytes, not premultiplied.
With "png", a PNG beside each wallpaper and an icon sheet, for looking at.

Coordinates are CoreGraphics' (origin bottom left, y up), as in the Swift original; a canvas
draws at twice the size and is reduced at the end for smooth edges.
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTDIR = os.environ.get('QRX_FONT_DIR', '/usr/share/fonts/opentype/inter')
FONTS = {  # Avenir Next weights -> Inter
    'AvenirNext-Heavy': 'InterDisplay-ExtraBold.otf', 'AvenirNext-Bold': 'Inter-Bold.otf',
    'AvenirNext-DemiBold': 'Inter-SemiBold.otf', 'AvenirNext-Medium': 'Inter-Medium.otf',
}
WORLDS = [('PPC', 0xFF9A2E), ('x86', 0x2E9BFF), ('ARM', 0x2EFFB4), ('68K', 0xE24BFF)]


def rgb(v):
    return ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0


class RNG:
    """The xorshift of the Swift original, 64-bit, same sequence"""
    def __init__(self, s):
        self.s = s & 0xFFFFFFFFFFFFFFFF

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFFFFFFFFFF
        s ^= s >> 7
        s ^= (s << 17) & 0xFFFFFFFFFFFFFFFF
        self.s = s
        return (s % 1000000) / 1000000.0


class Canvas:
    """An RGB float picture with CoreGraphics coordinates, drawn at scale S"""
    def __init__(self, w, h, S=2):
        self.w, self.h, self.S = w, h, S
        self.W, self.H = int(w * S), int(h * S)
        self.px = np.zeros((self.H, self.W, 3), np.float32)

    # coordinates
    def X(self, x): return x * self.S
    def Y(self, y): return (self.h - y) * self.S

    # filling
    def linear_v(self, colors, y_from, y_to):
        """vertical gradient: colors evenly from y_from to y_to (CG y), extended beyond"""
        ys = self.h - (np.arange(self.H, dtype=np.float32) + 0.5) / self.S
        t = np.clip((ys - y_from) / (y_to - y_from), 0, 1)
        self.px[:] = self._ramp(colors, t)[:, None, :]

    @staticmethod
    def _ramp(colors, t):
        cols = np.array([rgb(c) for c in colors], np.float32)
        n = len(colors) - 1
        f = t * n
        i = np.minimum(f.astype(int), n - 1)
        k = (f - i)[..., None]
        return cols[i] * (1 - k) + cols[i + 1] * k

    def radial_screen(self, stops, cx, cy, r):
        """CG radial gradient (stops: (rgb, alpha) evenly to radius r) drawn with screen blending"""
        x0, x1 = max(0, int(self.X(cx - r))), min(self.W, int(self.X(cx + r)) + 1)
        y0, y1 = max(0, int(self.Y(cy + r))), min(self.H, int(self.Y(cy - r)) + 1)
        if x0 >= x1 or y0 >= y1:
            return
        xs = (np.arange(x0, x1, dtype=np.float32) + 0.5) / self.S
        ys = self.h - (np.arange(y0, y1, dtype=np.float32) + 0.5) / self.S
        d = np.sqrt((xs[None, :] - cx) ** 2 + (ys[:, None] - cy) ** 2) / r
        inside = d <= 1
        n = len(stops) - 1
        f = np.clip(d, 0, 1) * n
        i = np.minimum(f.astype(int), n - 1)
        k = f - i
        cols = np.array([rgb(c) for c, a in stops], np.float32)
        alph = np.array([a for c, a in stops], np.float32)
        col = cols[i] * (1 - k[..., None]) + cols[i + 1] * k[..., None]
        a = (alph[i] * (1 - k) + alph[i + 1] * k) * inside
        sub = self.px[y0:y1, x0:x1]
        sub += (a[..., None] * col) * (1 - sub)       # screen: d + a*s*(1-d)

    def layer(self):
        return Image.new('RGBA', (self.W, self.H), (0, 0, 0, 0))

    def over(self, layer, shadow=None):
        """layer onto the picture; shadow = (color, alpha, blur, dx, dy) as CG setShadow"""
        if shadow:
            col, alpha, blur, dx, dy = shadow
            a = layer.getchannel('A')
            if blur > 0:
                a = a.filter(ImageFilter.GaussianBlur(blur * self.S / 2))
            sh = np.asarray(a, np.float32) / 255.0 * alpha
            if dx or dy:
                sh = np.roll(sh, (int(-dy * self.S), int(dx * self.S)), (0, 1))
            c = np.array(rgb(col), np.float32)
            self.px[:] = self.px * (1 - sh[..., None]) + c * sh[..., None]
        arr = np.asarray(layer, np.float32) / 255.0
        a = arr[..., 3:4]
        self.px[:] = self.px * (1 - a) + arr[..., :3] * a

    def image(self):
        img = Image.fromarray((np.clip(self.px, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')
        return img.resize((self.w, self.h), Image.LANCZOS) if self.S != 1 else img


def rgba(v, a=1.0):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, int(round(a * 255)))


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTDIR, FONTS[name]), max(1, int(round(size))))


def text(cv, s, fname, size, x, y, col, alpha=1.0, tracking=0.0, glow=None, blur=0.0):
    """centered at x, baseline y (CG), tracking after every glyph; glow = (color, alpha)"""
    f = font(fname, size * cv.S)
    adv = [f.getlength(ch) + tracking * cv.S for ch in s]
    width = sum(adv)
    lay = cv.layer()
    d = ImageDraw.Draw(lay)
    px = cv.X(x) - width / 2
    for ch, a in zip(s, adv):
        d.text((px, cv.Y(y)), ch, font=f, fill=rgba(col, alpha), anchor='ls')
        px += a
    cv.over(lay, (glow[0], glow[1], blur, 0, 0) if glow else None)


def starfield(cv, w, h, seed, count):
    r = RNG(seed)
    lay = cv.layer()
    d = ImageDraw.Draw(lay)
    for _ in range(count):
        x, y = r.next() * w, r.next() * h
        s = 0.4 + r.next() * r.next() * 1.8
        a = 0.15 + r.next() * 0.6
        d.ellipse([cv.X(x), cv.Y(y + s), cv.X(x + s), cv.Y(y)], fill=(204, 224, 255, int(a * 255)))
    cv.over(lay)


def traces(cv, frm, to, col, u, seed, count):
    """the board traces from a corner towards the middle, right angles and steps"""
    r = RNG(seed)
    for i in range(count):
        spread = (i - count / 2) * u * 2.6
        dx, dy = to[0] - frm[0], to[1] - frm[1]
        ln = math.hypot(dx, dy)
        nx, ny = -dy / ln, dx / ln
        p = (frm[0] + nx * spread, frm[1] + ny * spread)
        pts = [p]
        end = 0.45 + r.next() * 0.42
        t = 0.0
        while t < end:
            t += 0.05 + r.next() * 0.09
            tx = frm[0] + dx * t + nx * spread * (1 - t)
            ty = frm[1] + dy * t + ny * spread * (1 - t)
            pts.append((tx, p[1]) if r.next() < 0.5 else (p[0], ty))
            pts.append((tx, ty))
            p = (tx, ty)
        a = 0.25 + r.next() * 0.55
        lw = max(1.0, u * (0.10 + 0.12 * r.next()))
        lay = cv.layer()
        d = ImageDraw.Draw(lay)
        d.line([(cv.X(x), cv.Y(y)) for x, y in pts], fill=rgba(col, a), width=max(1, int(round(lw * cv.S))), joint='curve')
        n = u * 0.32
        d.ellipse([cv.X(p[0] - n), cv.Y(p[1] + n), cv.X(p[0] + n), cv.Y(p[1] - n)], fill=rgba(col, min(1.0, a + 0.3)))
        cv.over(lay, (col, 0.9, u * 1.4, 0, 0))


def rounded(d, cv, x, y, w, h, rad, **kw):
    d.rounded_rectangle([cv.X(x), cv.Y(y + h), cv.X(x + w), cv.Y(y)], radius=rad * cv.S, **kw)


def chip_badge(cv, p, name, col, u):
    s = u * 6.2
    lay = cv.layer()
    d = ImageDraw.Draw(lay)
    rounded(d, cv, p[0] - s / 2, p[1] - s / 2, s, s, u * 0.8, fill=rgba(0x0A0E18, 0.92),
            outline=rgba(col), width=max(1, int(round(max(1, u * 0.2) * cv.S))))
    cv.over(lay, (col, 0.9, u * 2.4, 0, 0))
    lay = cv.layer()
    d = ImageDraw.Draw(lay)
    lw = max(1, int(round(max(1, u * 0.14) * cv.S)))
    for i in range(5):
        o = -s * 0.32 + i * s * 0.16
        for a, b in (((p[0] + o, p[1] + s / 2), (p[0] + o, p[1] + s / 2 + u * 0.8)),
                     ((p[0] + o, p[1] - s / 2), (p[0] + o, p[1] - s / 2 - u * 0.8)),
                     ((p[0] + s / 2, p[1] + o), (p[0] + s / 2 + u * 0.8, p[1] + o)),
                     ((p[0] - s / 2, p[1] + o), (p[0] - s / 2 - u * 0.8, p[1] + o))):
            d.line([(cv.X(a[0]), cv.Y(a[1])), (cv.X(b[0]), cv.Y(b[1]))], fill=rgba(col, 0.85), width=lw)
    cv.over(lay)
    text(cv, name, 'AvenirNext-Bold', u * 2.0, p[0], p[1] - u * 0.7, col, glow=(col, 0.9), blur=u * 0.8)


def wordmark(cv, center, u, tagline):
    """QRX in the four colours, one world per stroke, and the lines under it"""
    size = u * 15
    y = center[1] - size * 0.34
    f = font('AvenirNext-Heavy', size * cv.S)
    track = u * 0.6 * cv.S
    adv = [f.getlength(ch) + track for ch in 'QRX']
    w = sum(adv)
    mask = Image.new('L', (cv.W, cv.H), 0)
    d = ImageDraw.Draw(mask)
    px = cv.X(center[0]) - w / 2
    for ch, a in zip('QRX', adv):
        d.text((px, cv.Y(y)), ch, font=f, fill=255, anchor='ls')
        px += a
    x0, x1 = cv.X(center[0]) - w / 2 - u * cv.S, cv.X(center[0]) + w / 2 + u * cv.S
    t = np.clip((np.arange(cv.W, dtype=np.float32) - x0) / (x1 - x0), 0, 1)
    ramp = Canvas._ramp([0xFF9A2E, 0xFFFFFF, 0x2E9BFF, 0xFFFFFF, 0x2EFFB4, 0xFFFFFF, 0xE24BFF], t)
    m = np.asarray(mask, np.float32)[..., None] / 255.0
    cv.px[:] = cv.px * (1 - m) + ramp[None, :, :] * m
    text(cv, 'FORMERLY KNOWN AS AEROS', 'AvenirNext-Medium', u * 1.5, center[0], y - u * 2.6, 0xDDE7FF, 0.92, tracking=u * 0.42)
    if tagline:
        text(cv, 'THE WORLDS OF PPC  ·  x86  ·  ARM  ·  68K  COMBINED', 'AvenirNext-DemiBold', u * 1.05,
             center[0], y - u * 4.8, 0x9FB4DA, 0.9, tracking=u * 0.28)


def design_convergence(cv, w, h):
    """the four worlds run from the corners into the core"""
    u = min(w, h) / 64
    cv.linear_v([0x05070F, 0x0A1020, 0x04060C], h, 0)
    starfield(cv, w, h, 88172645463325252, int(w * h / 1800))
    center = (w * 0.54, h * 0.50)
    corners = [(w * 0.20, h * 0.80), (w * 0.80, h * 0.80), (w * 0.20, h * 0.20), (w * 0.80, h * 0.20)]
    for i, p in enumerate(corners):
        col = WORLDS[i][1]
        cv.radial_screen([(col, 0.38), (col, 0.10), (col, 0)], p[0], p[1], max(w, h) * 0.52)
        seed = ((i + 1) * 0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF) | 1
        traces(cv, p, center, col, u, seed, 15)
    cv.radial_screen([(0xFFFFFF, 0.35), (0x6E8BFF, 0.18), (0x000000, 0)], center[0], center[1], u * 24)
    for i, p in enumerate(corners):
        chip_badge(cv, p, WORLDS[i][0], WORLDS[i][1], u)
    wordmark(cv, center, u, True)


def design_horizon(cv, w, h):
    """a horizon with a grid floor and a sun made of the four colours"""
    u = min(w, h) / 64
    horizon = h * 0.42
    cv.linear_v([0x030310, 0x16103A, 0x4A1566], h, horizon)
    starfield(cv, w, h, 1234567890123, int(w * h / 2600))
    sun = (w * 0.54, horizon + u * 2)
    # the sun: a gradient disc cut by bands
    ys = cv.h - (np.arange(cv.H, dtype=np.float32) + 0.5) / cv.S
    xs = (np.arange(cv.W, dtype=np.float32) + 0.5) / cv.S
    disc = ((xs[None, :] - sun[0]) ** 2 + (ys[:, None] - sun[1]) ** 2) <= (u * 15) ** 2
    t = np.clip((ys - (sun[1] + u * 15)) / ((sun[1] - u * 8) - (sun[1] + u * 15)), 0, 1)
    sun_col = Canvas._ramp([0xE24BFF, 0x2E9BFF, 0x2EFFB4, 0xFF9A2E], t)[:, None, :]
    band = np.zeros(cv.H, bool)
    y, th = sun[1] - u * 12, u * 0.2
    while y < sun[1] + u * 3:
        band |= (ys >= y) & (ys < y + th)
        y += u * 1.4
        th += u * 0.05
    m = disc & ~band[:, None]
    cv.px[m] = np.broadcast_to(sun_col, cv.px.shape)[m]
    cv.px[disc & band[:, None]] = rgb(0x16103A)
    cv.radial_screen([(0xFF70D0, 0.35), (0xFF70D0, 0)], sun[0], sun[1], u * 30)
    # the floor: below the horizon, a perspective grid
    below = ys < horizon
    t = np.clip((ys - horizon) / (0 - horizon), 0, 1)
    floor = Canvas._ramp([0x0B0420, 0x020208], t)[:, None, :]
    cv.px[below] = np.broadcast_to(floor, cv.px.shape)[below]
    lay = cv.layer()
    d = ImageDraw.Draw(lay)
    lw = max(1, int(round(max(1, u * 0.09) * cv.S)))
    for i in range(-22, 23):
        d.line([(cv.X(sun[0] + i * u * 0.9), cv.Y(horizon)), (cv.X(sun[0] + i * u * 15), cv.Y(-u))], fill=rgba(0x38C8FF, 0.75), width=lw)
    dd = 1.0
    while dd < 40:
        yy = horizon - horizon * (dd * dd) / 1600
        d.line([(0, cv.Y(yy)), (cv.W, cv.Y(yy))], fill=rgba(0x38C8FF, 0.75), width=lw)
        dd += 2.2
    lay = Image.composite(lay, Image.new('RGBA', lay.size, (0, 0, 0, 0)),
                          Image.fromarray((below * 255).astype(np.uint8)).resize(lay.size))
    cv.over(lay, (0x2E9BFF, 1.0, u * 0.8, 0, 0))
    for i, (name, col) in enumerate(WORLDS):
        x = w * 0.54 + (i - 1.5) * u * 11
        text(cv, name, 'AvenirNext-Bold', u * 2.0, x, horizon - u * 4, col, glow=(col, 1.0), blur=u * 1.4)
    wordmark(cv, (w * 0.54, h * 0.76), u * 0.9, False)
    text(cv, 'THE WORLDS OF PPC  ·  x86  ·  ARM  ·  68K  COMBINED', 'AvenirNext-DemiBold', u * 1.0,
         w * 0.54, u * 3.4, 0xBCD0F0, 0.85, tracking=u * 0.28)


def design_aurora(cv, w, h):
    """quiet: large soft fields of the four colours and a small mark"""
    u = min(w, h) / 64
    cv.px[:] = rgb(0x070A12)
    r = RNG(42424242424242)
    for p in range(36):
        col = WORLDS[p % 4][1]
        band = (p % 4) / 4
        x = (band + r.next() * 0.3 - 0.05) * w
        y = h * (0.15 + r.next() * 0.75)
        cv.radial_screen([(col, 0.13), (col, 0)], x, y, u * (14 + r.next() * 22))
    lay = cv.layer()
    d = ImageDraw.Draw(lay)
    i = 0.0
    while i < w + h:
        d.line([(cv.X(i), cv.Y(0)), (cv.X(i - h), cv.Y(h))], fill=(255, 255, 255, 13), width=cv.S)
        i += u * 2
    cv.over(lay)
    text(cv, 'QRX', 'AvenirNext-Heavy', u * 5, w - u * 10, u * 6.2, 0xFFFFFF, 0.92, tracking=u * 0.2, glow=(0x8FB8FF, 0.8), blur=u * 1.2)
    text(cv, 'FORMERLY KNOWN AS AEROS', 'AvenirNext-Medium', u * 0.72, w - u * 10, u * 4.4, 0xDDE7FF, 0.8, tracking=u * 0.16)
    text(cv, 'PPC · x86 · ARM · 68K', 'AvenirNext-DemiBold', u * 0.78, w - u * 10, u * 3.0, 0x9FB4DA, 0.8, tracking=u * 0.1)


def write_wallpaper(out, name, w, h, draw, png):
    cv = Canvas(w, h)
    draw(cv, float(w), float(h))
    img = cv.image()
    d = os.path.join(out, 'Wallpapers')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, f'{name}-{w}x{h}.qrxwp'), 'wb') as f:
        f.write(b'QRXW' + struct.pack('>II', w, h) + img.tobytes())
    if png:
        img.save(os.path.join(d, f'{name}-{w}x{h}.png'))


# ---- icons: drawn at 4x in a 48 unit square (y up) and reduced
KINDS = ['drawer', 'disk', 'ram', 'apps', 'file', 'program', 'windows']
THEMES = {
    'qrx':     dict(a=0x12203C, b=0x0A1020, line=0x37E6C8, paper=0x18263F, accent=0xE24BFF, radius=5, width=1.6, glow=True),
    'windows': dict(a=0xFFD966, b=0xE8A91C, line=0x5A4210, paper=0xFFFFFF, accent=0x1E6FD9, radius=1, width=1.2, glow=False),
    'mac':     dict(a=0x8CC8FF, b=0x3A8EEB, line=0x1F5FB0, paper=0xF7F9FC, accent=0x3A8EEB, radius=7, width=0.9, glow=False),
    'amiga':   dict(a=0x6E8FD0, b=0x2F5AA8, line=0x0A1A3A, paper=0xE9E9E9, accent=0xFF8A1E, radius=0, width=1.8, glow=False),
}


class Icon:
    """a 48-unit icon at scale K; shapes go onto the shadowed layer until shadow_off()"""
    def __init__(self, K=8):
        self.K = K
        self.N = 48 * K
        self.shadowed = Image.new('RGBA', (self.N, self.N), (0, 0, 0, 0))
        self.plain = Image.new('RGBA', (self.N, self.N), (0, 0, 0, 0))
        self.cur = self.shadowed

    def shadow_off(self): self.cur = self.plain
    def P(self, x, y): return (x * self.K, (48 - y) * self.K)
    def R(self, x, y, w, h): return [x * self.K, (48 - y - h) * self.K, (x + w) * self.K, (48 - y) * self.K]

    def paste(self, img):
        self.cur.alpha_composite(img)

    def grad_rect(self, x, y, w, h, rad, top, bottom):
        """a rounded rectangle filled top to bottom"""
        m = Image.new('L', (self.N, self.N), 0)
        ImageDraw.Draw(m).rounded_rectangle(self.R(x, y, w, h), radius=rad * self.K, fill=255)
        t = np.clip((np.arange(self.N, dtype=np.float32) / self.K - (48 - y - h)) / max(h, 1e-6), 0, 1)
        col = Canvas._ramp([top, bottom], t)
        arr = np.zeros((self.N, self.N, 4), np.uint8)
        arr[..., :3] = (col[:, None, :] * 255).astype(np.uint8)
        arr[..., 3] = np.asarray(m)
        self.paste(Image.fromarray(arr, 'RGBA'))

    def draw(self):
        return ImageDraw.Draw(self.cur)

    def stroke_rect(self, x, y, w, h, rad, col, width):
        self.draw().rounded_rectangle(self.R(x, y, w, h), radius=rad * self.K, outline=rgba(col),
                                      width=max(1, int(round(width * self.K))))

    def fill_rect(self, x, y, w, h, col, a=1.0):
        self.draw().rectangle(self.R(x, y, w, h), fill=rgba(col, a))

    def poly(self, pts, fill=None, outline=None, width=1.0):
        self.draw().polygon([self.P(*p) for p in pts], fill=fill, outline=outline,
                            width=max(1, int(round(width * self.K))) if outline else 0)


def draw_icon(ic, kind, t):
    pa, pb, line, paper, accent = t['a'], t['b'], t['line'], t['paper'], t['accent']
    if kind == 'drawer':
        if t['radius'] == 0:              # Amiga: a drawer with handles
            ic.grad_rect(5, 8, 38, 30, 0, pa, pb)
            ic.shadow_off()
            ic.stroke_rect(5, 8, 38, 30, 0, line, t['width'])
            for y in (25, 11):
                ic.fill_rect(8, y, 32, 10, paper)
                ic.stroke_rect(8, y, 32, 10, 0, line, 1)
                ic.fill_rect(19, y + 3, 10, 4, accent)
        else:
            tab = [(5, 32), (5, 39), (19, 39), (23, 34), (43, 34), (43, 30)]
            ic.poly(tab, fill=rgba(pb), outline=rgba(line) if t['glow'] else None, width=t['width'])
            ic.grad_rect(4, 7, 40, 27, t['radius'], pa, pb)
            ic.shadow_off()
            ic.stroke_rect(4, 7, 40, 27, t['radius'], line, t['width'])
            if t['glow']:
                ic.fill_rect(10, 12, 28, 2, 0xE24BFF)
            else:
                ic.fill_rect(7, 29, 34, 2, 0xFFFFFF, 0.35)
    elif kind in ('disk', 'ram', 'apps'):
        white = t['paper'] == 0xFFFFFF
        top = t['b'] if kind == 'ram' else (0xD8D8D8 if white else t['a'])
        bottom = t['a'] if kind == 'ram' else (0x9A9A9A if white else t['b'])
        ic.grad_rect(5, 12, 38, 25, t['radius'], top, bottom)
        ic.shadow_off()
        ic.stroke_rect(5, 12, 38, 25, t['radius'], line, t['width'])
        if kind == 'ram':
            for i in range(6):
                ic.fill_rect(9 + i * 5.2, 19, 3.4, 12, accent)
            for i in range(8):
                ic.fill_rect(8 + i * 4.4, 9, 2.2, 3, line)
        else:
            if kind == 'disk':
                ic.fill_rect(9, 29, 18, 1.2, line, 0.75)
                ic.fill_rect(9, 25, 18, 1.2, line, 0.75)
            ic.draw().ellipse(ic.R(34, 17, 5, 5), fill=rgba(0xFF9A2E if kind == 'apps' else accent))
            if kind == 'apps':
                for i, v in enumerate([0xFF9A2E, 0x2E9BFF, 0x2EFFB4, 0xE24BFF]):
                    ic.fill_rect(10 + (i % 2) * 7, 16 + (i // 2) * 7, 5.5, 5.5, v)
    elif kind == 'file':
        p = [(11, 5), (37, 5), (37, 33), (28, 43), (11, 43)]
        ic.poly(p, fill=rgba(paper))
        ic.shadow_off()
        ic.poly(p, outline=rgba(line), width=t['width'])
        for i in range(5):
            ic.fill_rect(15, 10 + i * 5, 18, 1.6, line, 0.45)
        ic.poly([(28, 43), (28, 33), (37, 33)], fill=rgba(accent))
    else:                                  # program, windows
        white = t['paper'] == 0xFFFFFF
        ic.grad_rect(5, 7, 38, 34, t['radius'], 0xF4F4F4 if white else t['a'], 0xC8C8C8 if white else t['b'])
        ic.shadow_off()
        ic.stroke_rect(5, 7, 38, 34, t['radius'], line, t['width'])
        ic.fill_rect(6, 33, 36, 7, accent)
        ic.fill_rect(36, 35, 3, 3, paper)
        ic.fill_rect(31, 35, 3, 3, paper)
        if kind == 'windows':
            for i, v in enumerate([0x2E9BFF, 0xFF9A2E, 0x2EFFB4, 0xE24BFF]):
                ic.fill_rect(17 + (i % 2) * 8, 13 + (i // 2) * 8, 6.5, 6.5, v)
        else:
            col = 0x303030 if white else line
            d = ic.draw()
            lw = 2 * ic.K
            d.line([ic.P(13, 26), ic.P(19, 21), ic.P(13, 16)], fill=rgba(col), width=lw, joint='curve')
            d.line([ic.P(22, 15), ic.P(31, 15)], fill=rgba(col), width=lw)


def render_icon(kind, t):
    ic = Icon()
    draw_icon(ic, kind, t)
    out = Image.new('RGBA', (ic.N, ic.N), (0, 0, 0, 0))
    a = ic.shadowed.getchannel('A')
    if t['glow']:                          # a glow in the line colour
        sh = a.filter(ImageFilter.GaussianBlur(3 * ic.K / 2))
        col = rgba(t['line'], 0.8)
    else:                                  # a soft shadow one unit down
        sh = a.filter(ImageFilter.GaussianBlur(1.6 * ic.K / 2)).transform(a.size, Image.AFFINE, (1, 0, 0, 0, 1, -ic.K))
        col = (0, 0, 0, int(0.35 * 255))
    shadow = Image.new('RGBA', a.size, col[:3] + (0,))
    shadow.putalpha(sh.point(lambda v: v * col[3] // 255))
    out.alpha_composite(shadow)
    out.alpha_composite(ic.shadowed)
    out.alpha_composite(ic.plain)
    return out.resize((48, 48), Image.LANCZOS)


def write_icon(out, theme, kind, img):
    d = os.path.join(out, 'Icons', theme)
    os.makedirs(d, exist_ok=True)
    px = img.tobytes()
    data = bytearray(b'QRXI' + struct.pack('>II', 48, 48))
    for i in range(0, len(px), 4):                       # RGBA -> A R G B, not premultiplied
        data += bytes((px[i + 3], px[i], px[i + 1], px[i + 2]))
    with open(os.path.join(d, kind + '.argb'), 'wb') as f:
        f.write(data)


def main():
    out = sys.argv[1]
    png = len(sys.argv) > 2
    sizes = [(1024, 768), (1280, 720), (1280, 1024), (1920, 1080)]
    only = os.environ.get('QRX_ASSET_SIZES')          # e.g. 1024x768 for a quick look
    if only:
        sizes = [tuple(int(v) for v in s.split('x')) for s in only.split(',')]
    for w, h in sizes:
        write_wallpaper(out, 'Convergence', w, h, design_convergence, png)
        write_wallpaper(out, 'Horizon', w, h, design_horizon, png)
        write_wallpaper(out, 'Aurora', w, h, design_aurora, png)
    sheet = Image.new('RGBA', (96 * len(KINDS), 96 * len(THEMES)), rgba(0x24304A))
    for r, (name, t) in enumerate(THEMES.items()):
        for i, k in enumerate(KINDS):
            img = render_icon(k, t)
            write_icon(out, name, k, img)
            sheet.alpha_composite(img.resize((96, 96), Image.LANCZOS), (i * 96, r * 96))
    if png:
        sheet.save(os.path.join(out, 'Icons', 'sheet.png'))
    print('done')


main()
