#!/usr/bin/env python3
"""Compare fpu-diff output: exact lines must match, '~' lines within 4 ulp."""
import struct
import sys


def ulps(a, b):
    def k(h):
        u = int(h, 16)
        return u if u < 1 << 63 else (1 << 63) - u
    return abs(k(a) - k(b))


nat = open(sys.argv[1]).read().splitlines()
emu = open(sys.argv[2]).read().splitlines()
bad = 0
if len(nat) != len(emu):
    print(f'line count differs: native {len(nat)} emu {len(emu)}')
    bad += 1
for i, (a, b) in enumerate(zip(nat, emu)):
    if a == b:
        continue
    ta, va = a.split(' ', 1)
    if ta.startswith('~') and b.startswith(ta + ' '):
        vb = b.split(' ', 1)[1]
        da = struct.unpack('<d', int(va, 16).to_bytes(8, 'little'))[0]
        if ulps(va, vb) <= 4 or (da != da):
            continue
    bad += 1
    if bad <= 30:
        print(f'DIFF line {i + 1}: native "{a}" emu "{b}"')
print(f'{len(nat) - bad}/{len(nat)} x87 results match')
sys.exit(1 if bad else 0)
