#!/usr/bin/env python3
"""Write a tiny valid 4-channel ProTracker module (M.K.) for player tests:
one 8-bit square-wave sample, one pattern with a C-major arpeggio."""
import struct, sys
name = b'QRX test module'.ljust(20, b'\0')
smp = bytes(([0x40] * 16 + [0xC0] * 16) * 32)          # 1024-byte square wave
samples = [struct.pack('>22sHbBHH', b'square'.ljust(22, b'\0'), len(smp) // 2, 0, 64, 0, len(smp) // 2)]
samples += [struct.pack('>22sHbBHH', b'\0' * 22, 0, 0, 0, 0, 1)] * 30
order = bytes([0]) + bytes(127)
periods = [428, 339, 285, 214]                            # C-2 E-2 G-2 C-3
pattern = b''
for row in range(64):
    for ch in range(4):
        if row % 4 == 0 and ch == 0:
            p = periods[(row // 4) % 4]
            pattern += struct.pack('>BBBB', (1 & 0xF0) | (p >> 8), p & 0xFF, (1 & 0x0F) << 4, 0)
        else:
            pattern += b'\0\0\0\0'
data = name + b''.join(samples) + bytes([1, 127]) + order + b'M.K.' + pattern + smp
open(sys.argv[1], 'wb').write(data)
print('test module', len(data), 'bytes')
