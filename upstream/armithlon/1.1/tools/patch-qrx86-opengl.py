#!/usr/bin/env python3
"""Optional, signature-gated QRX86 22.5.12 host OpenGL subset.
Does not alter the upstream GitHub branch or AROS behavior. This patch is
only for native --host QRX86 build and deliberately fails closed on drift.
"""
from pathlib import Path
import sys
if len(sys.argv)!=2: raise SystemExit('Usage: patch-qrx86-opengl.py /path/to/qrxosone')
repo=Path(sys.argv[1]).resolve()
roots=list(repo.glob('qrx-os-one/*/x86-runtime-22.5.12/win32'))
if len(roots)!=1: raise SystemExit(f'Expected one QRX86 Win32 source tree; found {len(roots)}')
win=roots[0]
marker='ARMITHLON_QRX86_GL_HLE_0_9'
changes={
 'hle.c': [('static const qhle *tables[] = { qw_files, qw_extra, qw_kernel32, qw_user32, qw_gdi32, qw_misc, qw_ansi, qw_dx, NULL };',
            'static const qhle *tables[] = { qw_files, qw_extra, qw_kernel32, qw_user32, qw_gdi32, qw_misc, qw_ansi, qw_dx, qw_gl, NULL }; /* '+marker+' */')],
 'qrxwin.h': [('extern const qhle qw_dx[];     /* dx.c: DirectDraw/DirectSound/DirectInput */',
               'extern const qhle qw_dx[];     /* dx.c: DirectDraw/DirectSound/DirectInput */\nextern const qhle qw_gl[]; /* '+marker+' */')],
 'pe.c': [('"msvcrt40", "crtdll", NULL', '"msvcrt40", "crtdll", "opengl32", NULL /* '+marker+' */')],
 'kernel.c': [
 ('"ddraw", "dsound", "dinput", NULL };', '"ddraw", "dsound", "dinput", "opengl32", NULL }; /* '+marker+' */'),
 ('#define QW_NKNOWN 12','#define QW_NKNOWN 13 /* '+marker+' */')]
}
# Atomic validation before mutating any file.
new={}
for name, replacements in changes.items():
    p=win/name
    if not p.is_file(): raise SystemExit('Missing required source '+str(p))
    contents=p.read_text()
    if marker in contents:
        for _,replacement in replacements:
            if replacement not in contents: raise SystemExit('Partially applied GL HLE: '+name)
    else:
        for old,replacement in replacements:
            if contents.count(old)!=1: raise SystemExit('QRX86 source drift: '+name+' / '+old[:75])
            contents=contents.replace(old,replacement)
    new[p]=contents
hle=win/'qrx_gl_hle.c'
example=Path(__file__).parent/'qrx_gl_hle.c'
if not example.exists(): raise SystemExit('Bundled OpenGL HLE source missing')
if hle.exists() and hle.read_bytes()!=example.read_bytes(): raise SystemExit('Refusing to overwrite nonmatching GL HLE')
for p,contents in new.items():
    if p.read_text()!=contents: p.write_text(contents)
if not hle.exists(): hle.write_bytes(example.read_bytes())
print('QRX86 22.5.12 GL HLE installed in local checkout; upstream unchanged:',win)
