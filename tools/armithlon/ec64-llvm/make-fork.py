#!/usr/bin/env python3
"""Copies LLVM's M68k backend to M68k64 with the names changed, as the base for the m68k64 patch.
GlobalISel and the disassembler are left out for now.
Usage: make-fork.py <llvm-project>/llvm/lib/Target"""
import os, re, shutil, sys

target = sys.argv[1]
src, dst = os.path.join(target, 'M68k'), os.path.join(target, 'M68k64')
shutil.rmtree(dst, ignore_errors=True)
for root, dirs, files in os.walk(src):
    dirs[:] = [d for d in dirs if d not in ('Disassembler', 'GISel')]
    out = root.replace(src, dst)
    os.makedirs(out, exist_ok=True)
    for name in files:
        text = open(os.path.join(root, name), encoding='utf8').read()
        text = text.replace('M68k', 'M68k64').replace('"m68k"', '"m68k64"').replace('Triple::m68k', 'Triple::m68k64')
        open(os.path.join(out, name.replace('M68k', 'M68k64')), 'w', encoding='utf8').write(text)
