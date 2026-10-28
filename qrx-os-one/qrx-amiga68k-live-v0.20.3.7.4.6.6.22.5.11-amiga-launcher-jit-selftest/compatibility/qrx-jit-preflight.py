#!/usr/bin/env python3
"""Host-side static verification of the existing m68k threaded JIT encoding.
No generated native code is executed. A pass DOES NOT authorize enabling JIT.
"""
import pathlib,struct,sys

def emit(helper,count):
    if not 0<=helper<=0xffffffff or not 0<=count<=4096:raise ValueError("invalid inputs")
    return b"".join(struct.pack(">HI",0x4eb9,helper) for _ in range(count))+struct.pack(">H",0x4e75)
def verify(code,helper,count):
    if len(code)!=count*6+2:raise ValueError("length mismatch")
    for i in range(count):
        op,target=struct.unpack_from(">HI",code,i*6)
        if op!=0x4eb9 or target!=helper:raise ValueError("JSR encoding mismatch")
    if struct.unpack_from(">H",code,count*6)[0]!=0x4e75:raise ValueError("missing RTS")
    return True
if __name__=="__main__":
    for n in (0,1,3,64,4096):
        for ptr in (0,0x12345678,0xffffffff):verify(emit(ptr,n),ptr,n)
    print("PASS: m68k JSR abs.l / RTS static encoding checks; native execution NOT tested")
