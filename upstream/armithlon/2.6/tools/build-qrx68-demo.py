#!/usr/bin/env python3
"""Make a small REAL 68000 HUNK executable calling Exec and DOS through 6-byte LVO vectors.

No Amiga ROMs, copied games, external toolchain, fake results, or private files.
Emitted image contains 68k instructions, not a host-generated output message.
"""
from pathlib import Path
import struct
root = Path(__file__).resolve().parents[1]
output = root / 'data/qrx68/QRX68-Hello.hunk'
code = bytearray()
def w(n): code.extend(struct.pack('>H', n & 0xffff))
def l(n): code.extend(struct.pack('>I', n))
w(0x2c78); w(4)          # MOVEA.L 4.W,A6 (SysBase)
w(0x43f9); libptr = len(code); l(0) # LEA dos.library,A1
w(0x7000)                # MOVEQ #0,D0
w(0x4eae); w(-552)       # JSR OpenLibrary(A6)
w(0x2c40)                # MOVEA.L D0,A6
w(0x4eae); w(-60)        # JSR Output(A6)
w(0x2200)                # MOVE.L D0,D1
w(0x243c); msgptr = len(code); l(0) # MOVE.L #msg,D2
w(0x760c)                # MOVEQ #12,D3
w(0x4eae); w(-48)        # JSR Write(A6)
w(0x4e75)                # RTS
liboff=len(code);code.extend(b'dos.library\x00')
msgoff=len(code);code.extend(b'Hello QRX68\n\x00')
struct.pack_into('>I', code, libptr, 0x10000+liboff)
struct.pack_into('>I', code, msgptr, 0x10000+msgoff)
while len(code) % 4:code.append(0)
header = (0x3f3,0,1,0,0,len(code)//4,0x3e9,len(code)//4)
image=struct.pack('>8I',*header)+code+struct.pack('>I',0x3f2)
output.parent.mkdir(parents=True,exist_ok=True)
output.write_bytes(image)
print(output,len(image),'bytes')
