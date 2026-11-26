#!/usr/bin/env python3
"""Reads one file out of an Amiga Fast File System (DOS\\1) disk image, as mkffsimg.py lays it out
(no partition table, the root block in the middle) and AROS's afs.handler then changes it.

Usage: ffsget.py <image> <path in the image, e.g. T/shot.png> <output file>

For files the guest wrote that cannot come back through the console (run-guest.sh QRX_KEEP_DISK:
qrxboot writes the RAM disk back to the image at the end, after warm reboots too)."""
import struct
import sys

BSIZE = 512
LONGS = BSIZE // 4
HT_SIZE = LONGS - 56
ST_USERDIR, ST_ROOT = 2, 1


def name_hash(name):
    h = len(name)
    for c in name.upper().encode("latin-1"):
        h = (h * 13 + c) & 0x7FF
    return h % HT_SIZE


def main():
    img, path, out = sys.argv[1:4]
    data = open(img, "rb").read()
    n = len(data) // BSIZE

    def blk(b):
        if not 0 < b < n:
            raise SystemExit(f"block {b} outside the image")
        return struct.unpack(">%dI" % LONGS, data[b * BSIZE:(b + 1) * BSIZE])

    def name_of(longs):
        raw = struct.pack(">8I", *longs[LONGS - 20:LONGS - 12])
        return raw[1:1 + raw[0]].decode("latin-1")

    cur = (2 + n - 1) // 2                       # the root
    if blk(cur)[LONGS - 1] != ST_ROOT:
        raise SystemExit("no root block where mkffsimg.py puts it")
    for part in [p for p in path.split("/") if p]:
        d = blk(cur)
        if d[LONGS - 1] not in (ST_ROOT, ST_USERDIR):
            raise SystemExit(f"{part}: parent is not a directory")
        b = d[6 + name_hash(part)]
        while b:
            e = blk(b)
            if name_of(e).lower() == part.lower():
                break
            b = e[LONGS - 4]                     # hash chain
        if not b:
            raise SystemExit(f"{part}: not found")
        cur = b
    hdr = blk(cur)
    size = hdr[LONGS - 47]
    blocks, table = [], hdr
    while True:
        cnt = table[2]
        blocks += [table[6 + HT_SIZE - 1 - j] for j in range(cnt)]
        ext = table[LONGS - 2]
        if not ext:
            break
        table = blk(ext)
    content = b"".join(data[b * BSIZE:(b + 1) * BSIZE] for b in blocks)[:size]
    if len(content) != size:
        raise SystemExit(f"only {len(content)} of {size} bytes in the block lists")
    open(out, "wb").write(content)
    print(f"{path}: {size} bytes")


main()
