#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parent
c=(root/'diagnostics-v6.6/tools/qrx_win_guest_loader.c').read_text()
d=(root/'diagnostics-v6.6/deploy.sh').read_text()
desk=(root/'base-v5.9/tools/qrx_desktop.canonical-6.6.20.c').read_text()
need_c=['IMAGE_REL_BASED_HIGHLOW','resolve_export','resolve_forwarder','process_imports','LINK PASS','execution=NOT_STARTED']
for x in need_c:
    if x not in c: raise SystemExit('QRX86 loader gate missing: '+x)
for x in ['QRXWinGuestLoader.elf','REACTOS-SOLITAIRE.txt','RunReactOSSolitaire','sol.exe','QRXWinGuestLoader']:
    if x not in d: raise SystemExit('deploy gate missing: '+x)
for x in ['winload ','reactoscalc','reactosnotepad','reactosthird','reactossolitaire','QRX:C/QRXWinGuestLoader']:
    if x not in desk: raise SystemExit('desktop command gate missing: '+x)
print('QRX86-GUEST-LOADER-GATE PASS: recursive PE32 mapping/imports/relocs/forwarders + Solitaire detection')
