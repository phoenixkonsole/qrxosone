#!/usr/bin/env python3
"""Unpacks an InstallShield 3 archive (_SETUP.1, DATA.Z ...: signature 13 5D 65 8C), as old Windows
setups (1995-1998) ship their files, so the program can be copied to the QRX system without running
the setup. The files are compressed with PKWARE's Data Compression Library ("implode"); the decoder
below follows the format as Mark Adler's blast.c describes it.
    is3-extract.py ARCHIVE OUTDIR        unpacks every file into OUTDIR/<group>/<name>
    is3-extract.py -l ARCHIVE            lists the files
Every file is checked against the size the archive records."""
import struct
import sys
from pathlib import Path


class DclError(Exception):
    pass


def _huffman(rep):
    lengths = []
    for b in rep:
        lengths += [b & 15] * ((b >> 4) + 1)
    count = [0] * 14
    for n in lengths:
        count[n] += 1
    offs = [0] * 14
    for n in range(1, 13):
        offs[n + 1] = offs[n] + count[n]
    symbol = [0] * len(lengths)
    for s, n in enumerate(lengths):
        if n:
            symbol[offs[n]] = s
            offs[n] += 1
    return count, symbol


_LIT = _huffman(bytes([11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8, 9, 7, 6, 7, 8, 7, 6,
                       55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5, 7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7,
                       9, 8, 25, 11, 8, 11, 9, 12, 8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24,
                       10, 27, 44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45, 44, 173]))
_LEN = _huffman(bytes([2, 35, 36, 53, 38, 23]))
_DIST = _huffman(bytes([2, 20, 53, 230, 247, 151, 248]))
_BASE = (3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264)
_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8)


def dcl_decompress(data):
    """PKWARE DCL implode stream -> bytes."""
    pos = 0
    bitbuf = 0
    bitcnt = 0

    def bits(n):
        nonlocal pos, bitbuf, bitcnt
        while bitcnt < n:
            if pos >= len(data):
                raise DclError("stream ends early")
            bitbuf |= data[pos] << bitcnt
            pos += 1
            bitcnt += 8
        v = bitbuf & ((1 << n) - 1)
        bitbuf >>= n
        bitcnt -= n
        return v

    def decode(h):
        count, symbol = h
        code = first = index = 0
        for n in range(1, 14):
            code |= bits(1) ^ 1                     # the codes are stored inverted
            c = count[n]
            if code < first + c:
                return symbol[index + code - first]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise DclError("bad code")

    lit = bits(8)
    dict_bits = bits(8)
    if lit > 1 or not 4 <= dict_bits <= 6:
        raise DclError("not a DCL stream")
    out = bytearray()
    while True:
        if bits(1):
            sym = decode(_LEN)
            length = _BASE[sym] + bits(_EXTRA[sym])
            if length == 519:
                return bytes(out)
            db = 2 if length == 2 else dict_bits
            dist = (decode(_DIST) << db) + bits(db) + 1
            if dist > len(out):
                raise DclError("distance too far back")
            for _ in range(length):
                out.append(out[-dist])
        else:
            out.append(decode(_LIT) if lit else bits(8))


def read_archive(raw):
    if raw[:4] != b"\x13\x5d\x65\x8c":
        raise SystemExit("not an InstallShield 3 archive")
    nfiles, = struct.unpack_from("<H", raw, 0x0C)
    toc, = struct.unpack_from("<I", raw, 0x29)
    ndirs, = struct.unpack_from("<H", raw, 0x31)
    p = toc
    dirs = []
    for _ in range(ndirs):
        count, chunk, nlen = struct.unpack_from("<HHH", raw, p)
        dirs.append((raw[p + 6:p + 6 + nlen].decode("latin-1"), count))
        p += chunk
    files = []
    for di, (dname, count) in enumerate(dirs):
        for _ in range(count):
            _flags, _dir, usize, csize, off, _dostime, _attr, chunk = struct.unpack_from("<BHIIIIIH", raw, p)
            nlen = raw[p + 29]
            name = raw[p + 30:p + 30 + nlen].decode("latin-1")
            files.append((dname, name, usize, csize, off))
            p += chunk
    if len(files) != nfiles:
        raise SystemExit(f"table of contents: {len(files)} files, the header says {nfiles}")
    return files


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "-l":
        for d, n, us, cs, off in read_archive(Path(args[1]).read_bytes()):
            print(f"{us:10d} {cs:10d}  {d}/{n}")
        return
    if len(args) != 2:
        raise SystemExit(__doc__)
    raw = Path(args[0]).read_bytes()
    out = Path(args[1])
    bad = 0
    for d, n, us, cs, off in read_archive(raw):
        if "/" in n or "\\" in n or n in ("", ".", "..") or "/" in d or d in (".", ".."):
            raise SystemExit(f"unsafe name in the archive: {d}/{n}")
        try:
            data = dcl_decompress(raw[off:off + cs])
        except DclError as e:
            print(f"FAIL {d}/{n}: {e}")
            bad += 1
            continue
        if len(data) != us:
            print(f"FAIL {d}/{n}: {len(data)} bytes, the archive says {us}")
            bad += 1
            continue
        (out / d).mkdir(parents=True, exist_ok=True)
        (out / d / n).write_bytes(data)
        print(f"ok   {d}/{n} ({us} bytes)")
    if bad:
        raise SystemExit(f"{bad} files failed")


if __name__ == "__main__":
    main()
