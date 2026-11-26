#!/usr/bin/env python3
"""Names the addresses of a QRX_BACKTRACE stack dump (libqrxaroscompat, qrxposix.c) on the host.
Usage: qrxbt.py <program (the ET_REL AROS executable)> <console log> [llvm bin dir]
The program's .text is loaded as one piece; its base is the printed address of qrx_bt_anchor minus
the symbol's value. Each "QRXBT frame/stack" address inside .text is printed with its function
(demangled) and the offset into it. Stack words are candidates: data that happens to point into
the code shows up too, the call chain is the run of functions that fit together."""
import bisect, re, subprocess, sys

prog, log = sys.argv[1], sys.argv[2]
llvm = sys.argv[3] if len(sys.argv) > 3 else '/opt/qrx-os-one/ec64llvm/build/bin'
syms = []
text_size = 0
for line in subprocess.run([llvm + '/llvm-readelf', '-S', '-W', prog], capture_output=True, text=True).stdout.splitlines():
    m = re.search(r'\]\s+\.text\s+PROGBITS\s+\S+\s+\S+\s+([0-9a-f]+)', line)
    if m:
        text_size = int(m.group(1), 16)
for line in subprocess.run([llvm + '/llvm-nm', '-n', '--defined-only', prog], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
vals = [s[0] for s in syms]
anchor = dict((n, v) for v, n in syms).get('qrx_bt_anchor')
if anchor is None:
    sys.exit('qrx_bt_anchor is not in the program (libqrxaroscompat without QRX_BACKTRACE)')
text = open(log, errors='replace').read()
m = re.search(r'QRXBT anchor (0x)?([0-9a-fA-F]+)', text)
if not m:
    sys.exit('no QRXBT dump in the log')
base = int(m.group(2), 16) - anchor
sg = re.search(r'QRXBT signal (?:0x)?([0-9a-fA-F]+)', text)
print(f'signal {int(sg.group(1), 16) if sg else "?"}, .text at {base:#x} ({text_size:#x} bytes)')
names = []
for kind, a in re.findall(r'QRXBT (frame \d+|stack) (?:0x)?([0-9a-fA-F]+)', text):
    off = int(a, 16) - base
    if not 0 <= off < text_size:
        continue
    i = bisect.bisect_right(vals, off) - 1
    if i < 0:
        continue
    names.append((kind, off, syms[i][1], off - syms[i][0]))
import os, shutil
filt = llvm + '/llvm-cxxfilt' if os.path.exists(llvm + '/llvm-cxxfilt') else shutil.which('c++filt') or 'cat'
dem = subprocess.run([filt], input='\n'.join(n[2] for n in names), capture_output=True, text=True).stdout.splitlines()
for (kind, off, name, d), dn in zip(names, dem):
    print(f'{kind:9} +{off:#010x}  {dn[:160]} +{d:#x}')
