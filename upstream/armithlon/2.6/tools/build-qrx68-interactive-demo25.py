#!/usr/bin/env python3
"""Original HUNK 68k live Intuition app: draw, wait for mouse, redraw, exit on CLOSEWINDOW.
Assembly deliberately uses documented Motorola 68k opcodes and Classic LVOs.
Works on the portable subset and should run on Musashi once validated.
"""
from pathlib import Path
import struct
import argparse

BASE=0x10000
b=bytearray();labels={};fix=[]
def word(v):b.extend(struct.pack('>H',v&65535))
def long(v):b.extend(struct.pack('>I',v&0xffffffff))
def label(name):labels[name]=len(b)
def absolute(name):fix.append(('long',len(b),name));long(0)
def branch(op,name):
    where=len(b);word(op);word(0);fix.append(('branch',where+2,name))
def strdata(name,value):label(name);b.extend(value.encode()+b'\0')
def movea_d(reg,a):word(0x2040|(a<<9)|reg)
def move_d(src,dest):word(0x2000|(dest<<9)|src)
def movea_disp(a,d,offset):word(0x2068|(d<<9)|a);word(offset)
def libcall(lvo):word(0x4eae);word(-lvo)
def imm_d(reg,value):word(0x7000|(reg<<9)|(value&0xff))

# Exec -> intuition.library; store Intuition base D6, Window D7.
word(0x2c78);word(4)
word(0x43f9);absolute('intuition')
imm_d(0,0);libcall(552);move_d(0,6);movea_d(0,6)
word(0x41f9);absolute('newwindow')
libcall(204);move_d(0,7);movea_d(0,5)
# Open graphics.library. Save graphics base in D4.
word(0x2c78);word(4);word(0x43f9);absolute('graphics')
imm_d(0,0);libcall(552);move_d(0,4);movea_d(0,6)
movea_disp(5,1,50)  # A1=Window.RPort
imm_d(0,4);libcall(342)
imm_d(0,10);imm_d(1,12);imm_d(2,90);imm_d(3,80);libcall(306)
# SysBase for WaitPort, GetMsg, ReplyMsg.
word(0x2c78);word(4)
label('wait')
movea_disp(5,0,86);libcall(384)   # WaitPort(Window.UserPort)
movea_disp(5,0,86);libcall(372)   # GetMsg
move_d(0,5);movea_d(0,1)        # D5=msg; A1=msg
word(0x2029);word(20)          # MOVE.L 20(A1),D0 class
word(0x0c80);long(8)           # CMPI.L IDCMP_MOUSEBUTTONS,D0
branch(0x6600,'notmouse')
# draw red rectangle, then return to Exec and ReplyMsg.
movea_d(4,6);movea_disp(5,1,50)
imm_d(0,2);libcall(342)
imm_d(0,100);imm_d(1,12);imm_d(2,120);imm_d(3,80);libcall(306)
word(0x2c78);word(4)
label('notmouse')
movea_d(5,1)                   # A1=msg
libcall(378)                   # ReplyMsg
# use D0 after ReplyMsg? It has class from CMP, reply preserved value.
word(0x0c80);long(512)         # CMPI.L close? D0 preserved
branch(0x6600,'wait')
movea_d(6,6);movea_d(7,0)
libcall(72)                    # CloseWindow
word(0x4e75)
strdata('intuition','intuition.library')
strdata('graphics','graphics.library')
while len(b)%2:b.append(0)
label('newwindow')
# NewWindow +10: IDCMPFlags = close | mouse buttons = 520
b.extend(struct.pack('>hhhhBBII',8,8,256,128,1,0,512|8,0))
b.extend(bytes(48-18))
for typ,at,name in fix:
    target=labels[name]
    if typ=='long':struct.pack_into('>I',b,at,BASE+target)
    else:
        opcode_at=at-2
        off=target-(opcode_at+2)
        assert -32768<=off<=32767
        struct.pack_into('>h',b,at,off)
while len(b)%4:b.append(0)
n=len(b)//4
payload=struct.pack('>8I',0x3f3,0,1,0,0,n,0x3e9,n)+b+struct.pack('>I',0x3f2)
if __name__=='__main__':
    pa=argparse.ArgumentParser();pa.add_argument('output',type=Path);a=pa.parse_args()
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(payload)
    print('QRX68 live HUNK',len(payload),'bytes')
