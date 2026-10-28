#!/usr/bin/env python3
"""Synthetic QRXGRT1 handoff writer for host regression tests of QRX86GRTExec.

Layout mirrors selftest-qrx86-6.6.22.5.sh: one mapped PE image (test.dll, 4 KiB at
0x00400000, code at RVA 0x100), an optional ntdll bridge entry, and two init records
(host bootstrap + DLL_PROCESS_ATTACH of the code at RVA 0x100).
"""
import struct, sys

BASE = 0x00400000

def write(path, code, bridge=None):
    img = bytearray(0x1000)
    img[0x100:0x100 + len(code)] = code
    bridges = [] if bridge is None else [bridge]
    with open(path, 'wb') as f:
        u = lambda x: f.write(struct.pack('<I', x))
        f.write(b'QRXGRT1\0')
        for x in [1, 1, len(bridges), 2, 0, 0x7ffde000, 0x7ffdf000, 0x7f010000]:
            u(x)
        for x in [BASE, len(img), 0x100, 0x2000, 0]:
            u(x)
        n = b'test.dll'; f.write(n + b'\0' * (64 - len(n)))
        for addr, name, argc, flags in bridges:
            for x in [addr, 0, argc, flags]:
                u(x)
            n = name.encode(); f.write(n + b'\0' * (64 - len(n)))
        for rec in [(1, 0, 0, BASE, 1, 0), (3, 0, BASE + 0x100, BASE, 1, 0)]:
            for x in rec:
                u(x)
        f.write(img)

H = bytes.fromhex
CASES = {
    # mov eax,1 ; ret 0Ch
    'ret': H('b8 01 00 00 00 c2 0c 00'),
    # 4x nop ; mov eax,1 ; ret 0Ch  -> exactly 6 guest instructions in the DllMain call
    'count6': H('90 90 90 90 b8 01 00 00 00 c2 0c 00'),
    # mov edi,6ff00000 ; mov ecx,000F4250 (1,000,016) ; mov al,5a ; rep stosb ; mov eax,ecx ; ret 0Ch
    # A correct IA-32 interpreter must finish with ECX=0 -> eax=00000000.
    'repfull': H('bf 00 00 f0 6f b9 50 42 0f 00 b0 5a f3 aa 89 c8 c2 0c 00'),
    # L: mov esi,00400000 ; mov ecx,00001000 ; rep lodsb ; jmp L   (endless, REP-heavy)
    'repspin': H('be 00 00 40 00 b9 00 10 00 00 f3 ac eb f2'),
    # mov eax,0 ; mov ah,12 ; mov al,34 ; movzx eax,ax ; ret 0Ch  -> eax=00001234 (MOV r8,imm8 incl. AH)
    'movr8': H('b8 00 00 00 00 b4 12 b0 34 0f b7 c0 c2 0c 00'),
    # jmp $  (endless single-instruction loop)
    'spin': H('eb fe'),
    # ud2  (unsupported -> must fail closed)
    'ud2': H('0f 0b'),
}

if __name__ == '__main__':
    out = sys.argv[1]
    for name, code in CASES.items():
        write(f'{out}/{name}.qrxgrt', code)
    print(' '.join(CASES))
