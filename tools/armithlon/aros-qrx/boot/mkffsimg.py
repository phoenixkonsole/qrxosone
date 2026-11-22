#!/usr/bin/env python3
"""Builds an Amiga Fast File System (DOS\\1) disk image from a host directory.

Usage: mkffsimg.py <directory> <image> [size in MB, default 32; above about 50 MB with bitmap
extension blocks] [volume name, default QRXSYS]

The image has no partition table: blocks 0 and 1 are the boot block, the root block is in the
middle, as on a floppy. qrxdisk.device serves it on the 68100EC64 reference machine and AROS's
afs.handler mounts it as DH0:. Names are kept (at most 30 characters, ASCII).
"""
import os
import struct
import sys
import time

BSIZE = 512
LONGS = BSIZE // 4
HT_SIZE = LONGS - 56                    # 72 hash slots, and 72 data block pointers per header
T_HEADER, T_LIST = 2, 16
ST_ROOT, ST_USERDIR, ST_FILE = 1, 2, -3 & 0xFFFFFFFF
RESERVED = 2
BITS_PER_BITMAP = (LONGS - 1) * 32
AMIGA_EPOCH = 252460800                 # 1978-01-01 in Unix time


def amiga_date(t):
    t = int(max(t, AMIGA_EPOCH)) - AMIGA_EPOCH
    days, rest = divmod(t, 86400)
    return days, rest // 60, (rest % 60) * 50


def name_hash(name):
    h = len(name)
    for c in name.upper().encode("latin-1"):
        h = (h * 13 + c) & 0x7FF
    return h % HT_SIZE


class Image:
    def __init__(self, size_mb, volname):
        self.n = size_mb * 1024 * 1024 // BSIZE
        self.root = (RESERVED + self.n - 1) // 2
        self.blocks = {}                # block number: list of 128 longs, or bytes for data
        self.used = set(range(RESERVED)) | {self.root}
        nbm = (self.n - RESERVED + BITS_PER_BITMAP - 1) // BITS_PER_BITMAP
        # The root holds 25 bitmap pointers; each extension block holds 127 more and a link.
        next_free = self.root + 1
        self.bitmaps = list(range(next_free, next_free + nbm))
        next_free += nbm
        next = max(0, (nbm - 25 + LONGS - 2) // (LONGS - 1))
        self.bm_ext = list(range(next_free, next_free + next))
        if next_free + next > self.n:
            raise SystemExit("image too small for its bitmap")
        self.used |= set(self.bitmaps) | set(self.bm_ext)
        self.next = RESERVED
        self.volname = volname

    def alloc(self):
        while self.next in self.used:
            self.next += 1
        if self.next >= self.n:
            raise SystemExit("image full")
        b = self.next
        self.used.add(b)
        return b

    @staticmethod
    def put_name(longs, at_byte, name):
        raw = bytearray(32)
        enc = name.encode("latin-1")[:30]
        raw[0] = len(enc)
        raw[1:1 + len(enc)] = enc
        for i in range(8):
            longs[at_byte // 4 + i] = struct.unpack(">I", raw[i * 4:i * 4 + 4])[0]

    @staticmethod
    def checksum(longs, at=5):
        longs[at] = 0
        longs[at] = -sum(longs) & 0xFFFFFFFF

    def add_entry(self, dir_longs, block, name):
        """Hash entry 'block' named 'name' into the directory 'dir_longs' (root or user dir)."""
        slot = 6 + name_hash(name)
        self.blocks[block][LONGS - 4] = dir_longs[slot]     # hash_chain: the old head of the bucket
        dir_longs[slot] = block

    def header(self, own, parent, name, mtime, sec_type):
        b = [0] * LONGS
        b[0] = T_HEADER
        b[1] = own
        days, mins, ticks = amiga_date(mtime)
        b[LONGS - 23], b[LONGS - 22], b[LONGS - 21] = days, mins, ticks
        self.put_name(b, BSIZE - 80, name)
        b[LONGS - 3] = parent
        b[LONGS - 1] = sec_type
        return b

    def add_file(self, path, parent, name, st):
        own = self.alloc()
        hdr = self.header(own, parent, name, st.st_mtime, ST_FILE)
        self.blocks[own] = hdr
        data = open(path, "rb").read()
        datablocks = []
        for off in range(0, len(data), BSIZE):
            b = self.alloc()
            self.blocks[b] = data[off:off + BSIZE].ljust(BSIZE, b"\0")
            datablocks.append(b)
        hdr[LONGS - 47] = len(data)                         # byte_size
        hdr[LONGS - 48] = 0                                 # protection: rwed for the owner
        hdr[4] = datablocks[0] if datablocks else 0         # first_data
        table, cur, chunks = hdr, own, [datablocks[i:i + HT_SIZE] for i in range(0, len(datablocks), HT_SIZE)]
        for i, chunk in enumerate(chunks or [[]]):
            if i > 0:
                ext = self.alloc()
                eb = [0] * LONGS
                eb[0], eb[1] = T_LIST, ext
                eb[LONGS - 3] = own                         # parent: the file header
                eb[LONGS - 1] = ST_FILE
                self.blocks[ext] = eb
                table[LONGS - 2] = ext                      # extension of the previous one
                table = eb
            table[2] = len(chunk)                           # high_seq
            for j, blk in enumerate(chunk):
                table[6 + HT_SIZE - 1 - j] = blk            # filled from the end
        return own

    def add_dir(self, path, parent, name, st):
        own = self.alloc()
        d = self.header(own, parent, name, st.st_mtime, ST_USERDIR)
        self.blocks[own] = d
        self.fill(path, own, d)
        return own

    def fill(self, path, own, dir_longs):
        for name in sorted(os.listdir(path)):
            if name.startswith("."):
                continue
            if len(name.encode("latin-1", "replace")) > 30:
                raise SystemExit("name longer than 30 characters: " + name)
            full = os.path.join(path, name)
            st = os.stat(full)
            if os.path.isdir(full):
                blk = self.add_dir(full, own, name, st)
            else:
                blk = self.add_file(full, own, name, st)
            self.add_entry(dir_longs, blk, name)

    def build(self, src):
        r = [0] * LONGS
        r[0] = T_HEADER
        r[3] = HT_SIZE
        r[LONGS - 50] = 0xFFFFFFFF                          # bm_flag: the bitmap is valid
        for i, b in enumerate(self.bitmaps[:25]):
            r[LONGS - 49 + i] = b
        if self.bm_ext:
            r[LONGS - 24] = self.bm_ext[0]                  # bm_ext: the first extension block
            rest = self.bitmaps[25:]
            for k, eb in enumerate(self.bm_ext):
                longs = [0] * LONGS
                part = rest[k * (LONGS - 1):(k + 1) * (LONGS - 1)]
                longs[:len(part)] = part
                longs[LONGS - 1] = self.bm_ext[k + 1] if k + 1 < len(self.bm_ext) else 0
                self.blocks[eb] = struct.pack(">%dI" % LONGS, *longs)
        now = amiga_date(time.time())
        r[LONGS - 23:LONGS - 20] = list(now)                # root altered
        r[LONGS - 10:LONGS - 7] = list(now)                 # volume altered
        r[LONGS - 7:LONGS - 4] = list(now)                  # created
        self.put_name(r, BSIZE - 80, self.volname)
        r[LONGS - 1] = ST_ROOT
        self.blocks[self.root] = r
        self.fill(src, self.root, r)

        img = bytearray(self.n * BSIZE)
        img[0:4] = b"DOS\x01"
        boot = list(struct.unpack(">%dI" % (2 * LONGS), img[0:2 * BSIZE]))
        boot[2] = self.root
        s = 0
        for v in boot:                                      # boot block sum with carry, inverted
            s += v
            if s > 0xFFFFFFFF:
                s = (s + 1) & 0xFFFFFFFF
        boot[1] = ~s & 0xFFFFFFFF
        img[0:2 * BSIZE] = struct.pack(">%dI" % (2 * LONGS), *boot)

        for blk, content in self.blocks.items():
            if isinstance(content, list):
                if content[0] in (T_HEADER, T_LIST):
                    self.checksum(content)
                content = struct.pack(">%dI" % LONGS, *content)
            img[blk * BSIZE:(blk + 1) * BSIZE] = content

        # Bitmap: bit set is free; bit 0 of the first long is block 2
        for i, bmb in enumerate(self.bitmaps):
            longs = [0] * LONGS
            for k in range(LONGS - 1):
                base = RESERVED + i * BITS_PER_BITMAP + k * 32
                if base >= self.n:
                    continue
                v = 0
                for bit in range(32):
                    blk = base + bit
                    if blk < self.n and blk not in self.used:
                        v |= 1 << bit
                longs[1 + k] = v
            self.checksum(longs, 0)
            img[bmb * BSIZE:(bmb + 1) * BSIZE] = struct.pack(">%dI" % LONGS, *longs)
        return bytes(img)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    name = sys.argv[4] if len(sys.argv) > 4 else "QRXSYS"
    with open(sys.argv[2], "wb") as f:
        f.write(Image(size, name).build(sys.argv[1]))


if __name__ == "__main__":
    main()
