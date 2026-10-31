#!/usr/bin/env python3
from pathlib import Path
import sys
root=Path(__file__).resolve().parent
src=root/'diagnostics-v6.6/tools/qrx86_grt_exec.c'
dep=root/'diagnostics-v6.6/deploy.sh'
desk=root/'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c'
for p in (src,dep,desk):
    if not p.is_file(): raise SystemExit(f'missing: {p}')
s=src.read_text()
required=[
 'QRXGRT1', 'bootstrap_vm', 'fs_base', 'case 0xa1:', 'bridge_dispatch',
 'RtlGetProcessHeap', 'NtAllocateVirtualMemory', 'block_build',
 'jit_compile_native', '0x4eb9', '0x4e75', 'CacheClearU',
 'DLLMAIN_PROCESS_ATTACH', 'EXE_TLS_PROCESS_ATTACH', 'QRX86 STOP'
]
for x in required:
    if x not in s: raise SystemExit(f'QRX86 .22.5 source gate missing: {x}')
d=dep.read_text()
import re
if not re.search(r'^REL="0\.20\.3\.7\.4\.6\.6\.22\.5(?:\.[0-9]+)?"$', d, re.M):
    raise SystemExit('deploy gate missing: compatible REL 0.20.3.7.4.6.6.22.5[.patch]')
for x in ['QRX86GRTExec.elf','QRX:C/QRX86GRTExec --jit=on','FINAL ISO GATE: QRX:C/QRX86GRTExec missing']:
    if x not in d: raise SystemExit(f'deploy gate missing: {x}')
t=desk.read_text()
for x in ['winjit ', 'wintrace ', 'QRX:C/QRX86GRTExec --jit=on T:QRX-calc.qrxgrt', 'QRX:C/QRX86GRTExec --jit=on T:QRX-notepad.qrxgrt']:
    if x not in t: raise SystemExit(f'desktop command gate missing: {x}')
print('QRX86 6.6.22.5 source/integration gates: PASS')

# 22.5.7 safe trace regression gate
desk=(root/"base-v5.9/tools/qrx_desktop.canonical-6.6.20.c").read_text(errors="ignore")
for x in ["QRX:C/QRX86GRTExec --jit=off --trace --max-steps 100000 T:QRX-wintrace.qrxgrt"]:
    if x not in desk: raise SystemExit(f"safe trace gate missing: {x}")
execsrc=(root/"diagnostics-v6.6/tools/qrx86_grt_exec.c").read_text(errors="ignore")
for x in ["--max-steps", "QRX86 WATCHDOG:", "trace_lines<2000u"]:
    if x not in execsrc: raise SystemExit(f"watchdog gate missing: {x}")
print("QRX86 6.6.22.5.7 safe trace/watchdog gates: PASS")
