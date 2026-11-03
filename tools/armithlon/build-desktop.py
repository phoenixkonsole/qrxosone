#!/usr/bin/env python3
"""Build canonical QRXDesktop with a local layer-safe RTG wallpaper patch.
Preserves canonical source and the source-hash manifests. A synthetic wallpaper
is explicitly used only when the original generated wallpaper is unavailable.
"""
from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[2]
source=next((root/'qrx-os-one').glob('*/base-v5.9/tools/qrx_desktop.canonical-6.6.20.c'))
out=root/'.build/qrx-desktop';out.mkdir(parents=True,exist_ok=True)
s=source.read_text();old='rp = &s->RastPort;'
assert s.count(old)==1
s=s.replace(old,'/* Keep wallpaper within desktop refresh/layer clipping. */\n    rp = g_desktop ? g_desktop->RPort : &s->RastPort;')
(out/'qrx_desktop.armithlon.c').write_text(s)
if not (out/'qrx_wallpaper.h').exists():
 generator=next((root/'qrx-os-one').glob('*/amiga-runtime-22.5.11/tests/desktop/gen-test-wallpaper.py'))
 subprocess.run([sys.executable,str(generator),str(out/'qrx_wallpaper.h')],check=True)
sdk=root/'.build/aros-sdk/bin'
subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC','-Wno-error=incompatible-pointer-types','-I'+str(out),str(out/'qrx_desktop.armithlon.c'),'-o',str(out/'QRXDesktop')],check=True)
subprocess.run([str(sdk/'elf2hunk'),str(out/'QRXDesktop'),str(out/'QRXDesktop.hunk')],check=True)
