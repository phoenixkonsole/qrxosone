#!/usr/bin/env python3
"""Produce a separate ROM with the AROS lowlevel input-signal lifetime fixed."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
if a.source.resolve()==a.output.resolve():raise SystemExit('Refusing to overwrite original ROM')
b=bytearray(a.source.read_bytes())
for offset,expected in [(0x628e2,bytes.fromhex('4eae feb0')),(0x628ea,bytes.fromhex('50e8 000f'))]:
 if b[offset:offset+4]!=expected:raise SystemExit(f'Unexpected ROM opcode at {offset:x}; no output written')
 b[offset:offset+4]=bytes.fromhex('4e71 4e71')
a.output.write_bytes(b)
