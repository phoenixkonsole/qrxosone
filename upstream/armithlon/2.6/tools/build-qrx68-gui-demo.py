#!/usr/bin/env python3
"""Build original tiny 68k Amiga HUNK drawing through QRX68 Intuition/gfx HLE.
No assembler/download needed; all 68k opcodes assembled directly into HUNK_CODE.
"""
import argparse
import struct
from pathlib import Path

BASE=0x10000
code=bytearray()
patches=[]
def w(v):code.extend(struct.pack('>H',v & 0xffff))
def l(v):code.extend(struct.pack('>I',v & 0xffffffff))
def label_ref(name):patches.append((len(code),name));l(0)
def string(name, value):
    labels[name]=len(code);code.extend(value.encode('ascii')+b'\0')
labels={}
# ExecBase at 4.w, OpenLibrary intuition.library
w(0x2c78);w(0x0004)
w(0x43f9);label_ref('intuition') # LEA absolute,A1
w(0x7000)
w(0x4eae);w(-552)
w(0x2c40) # MOVEA.L D0,A6
w(0x41f9);label_ref('newwindow') # LEA NewWindow,A0
w(0x4eae);w(-204) # OpenWindow
w(0x2a40) # MOVEA.L D0,A5
# Open graphics.library
w(0x2c78);w(0x0004)
w(0x43f9);label_ref('graphics')
w(0x7000)
w(0x4eae);w(-552)
w(0x2c40)
w(0x226d);w(50) # MOVEA.L 50(A5),A1 (window->RPort)
w(0x7004) # MOVEQ #4,D0 (blue)
w(0x4eae);w(-342) # SetAPen
w(0x700a) # MOVEQ #10,D0
w(0x7210) # MOVEQ #16,D1
w(0x7478) # MOVEQ #120,D2
w(0x7638) # MOVEQ #56,D3
w(0x4eae);w(-306) # RectFill
w(0x4e75) # RTS
string('intuition','intuition.library')
string('graphics','graphics.library')
while len(code)%2:code.append(0)
labels['newwindow']=len(code)
# Classic NewWindow: LeftEdge/TopEdge/Width/Height, DetailPen, BlockPen,
# IDCMPFlags=0, Flags=0, Gadget=NULL, CheckMark=NULL, Title=NULL,
# Screen=NULL, BitMap=NULL, remaining min/max/types initialized zero.
code.extend(struct.pack('>hhhhBBII',8,8,256,128,1,0,0,0))
code.extend(bytes(48-18))
assert len(code)%2==0
for off,name in patches:struct.pack_into('>I',code,off,BASE+labels[name])
while len(code)%4:code.append(0)
n=len(code)//4
file=struct.pack('>8I',0x3f3,0,1,0,0,n,0x3e9,n)+code+struct.pack('>I',0x3f2)
ap=argparse.ArgumentParser();ap.add_argument('output',type=Path);args=ap.parse_args()
args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_bytes(file)
print(args.output,len(file),'bytes')
