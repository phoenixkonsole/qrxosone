#!/usr/bin/env python3
"""Checks card-game snapshots (binary PPM P6): felt green, white card faces, and
that two snapshots differ (the game reacted).  Usage:
  check-cards.py felt FILE            green table + card faces present
  check-cards.py differ FILE1 FILE2   pictures differ in at least 1000 pixels"""
import sys

def ppm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    px = d[i+1:i+1+w*h*3]
    return w, h, px

def felt(path):
    w, h, px = ppm(path)
    n = w * h
    green = sum(1 for k in range(0, len(px), 3) if px[k] < 40 and 60 <= px[k+1] <= 110 and px[k+2] < 40)
    white = sum(1 for k in range(0, len(px), 3) if px[k] > 240 and px[k+1] > 240 and px[k+2] > 240)
    ok = green > n * 0.25 and white > n * 0.03
    print(f'{path}: green {100*green/n:.1f}% white {100*white/n:.1f}% -> {"OK" if ok else "FAIL"}')
    return ok

def differ(a, b):
    wa, ha, pa = ppm(a); wb, hb, pb = ppm(b)
    if (wa, ha) != (wb, hb): print('size differs -> OK'); return True
    diff = sum(1 for k in range(0, len(pa), 3) if pa[k:k+3] != pb[k:k+3])
    print(f'{a} vs {b}: {diff} pixels differ -> {"OK" if diff >= 1000 else "FAIL"}')
    return diff >= 1000

ok = felt(sys.argv[2]) if sys.argv[1] == 'felt' else differ(sys.argv[2], sys.argv[3])
sys.exit(0 if ok else 1)
