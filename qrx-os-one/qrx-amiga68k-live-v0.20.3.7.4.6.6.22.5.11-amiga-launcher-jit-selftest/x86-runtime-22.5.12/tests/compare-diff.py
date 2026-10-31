#!/usr/bin/env python3
"""Compare native and emulated records of gen-diff-test.py.
usage: compare-diff.py META NATIVE.bin EMU.bin"""
import struct
import sys

NAMES = ['eax', 'ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi', 'eflags', 'mem']
ARITH = 0x8D5


def main():
    meta = [l.rstrip('\n').split('\t', 1) for l in open(sys.argv[1])]
    nat = open(sys.argv[2], 'rb').read()
    emu = open(sys.argv[3], 'rb').read()
    if len(nat) != len(meta) * 40:
        print(f'native record has {len(nat)} bytes, expected {len(meta) * 40}')
        return 2
    if len(emu) != len(nat):
        print(f'FAIL: emulated record has {len(emu)} bytes, native {len(nat)}')
        return 1
    bad = 0
    for i, (und, insn) in enumerate(meta):
        a = struct.unpack_from('<9I', nat, i * 40)
        b = struct.unpack_from('<9I', emu, i * 40)
        u = int(und, 16)
        diffs = []
        for k, n in enumerate(NAMES):
            x, y = a[k], b[k]
            if n == 'eflags':
                x &= ARITH & ~u
                y &= ARITH & ~u
            if x != y:
                diffs.append(f'{n}: native={x:08x} emu={y:08x}')
        if diffs:
            bad += 1
            if bad <= 40:
                print(f'DIFF case {i}: {insn}: ' + '; '.join(diffs))
    print(f'{len(meta) - bad}/{len(meta)} cases identical (undefined flags masked)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
