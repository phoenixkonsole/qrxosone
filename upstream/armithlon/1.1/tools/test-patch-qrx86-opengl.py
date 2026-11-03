#!/usr/bin/env python3
"""Stub source signature/idempotence/fail-closed regression test."""
from pathlib import Path
import tempfile, subprocess,sys
root=Path(__file__).parent
files={
'hle.c':'static const qhle *tables[] = { qw_files, qw_extra, qw_kernel32, qw_user32, qw_gdi32, qw_misc, qw_ansi, qw_dx, NULL };',
'qrxwin.h':'extern const qhle qw_dx[];     /* dx.c: DirectDraw/DirectSound/DirectInput */',
'pe.c':'"msvcrt40", "crtdll", NULL',
'kernel.c':'"ddraw", "dsound", "dinput", NULL };\n#define QW_NKNOWN 12'
}
with tempfile.TemporaryDirectory() as t:
    dst=Path(t)/'qrx-os-one/test/x86-runtime-22.5.12/win32'
    dst.mkdir(parents=True)
    for n,s in files.items():(dst/n).write_text(s+'\n')
    cmd=[sys.executable,str(root/'patch-qrx86-opengl.py'),t]
    subprocess.run(cmd,check=True,stdout=subprocess.PIPE)
    first={n:p.read_bytes() for n in [*files.keys(),'qrx_gl_hle.c'] if (p:=dst/n).exists()}
    subprocess.run(cmd,check=True,stdout=subprocess.PIPE)
    for n,b in first.items():assert (dst/n).read_bytes()==b
    (dst/'kernel.c').write_text('corrupted source')
    fail=subprocess.run(cmd,capture_output=True)
    assert fail.returncode!=0
    print('Optional QRX86 OpenGL 1.x HLE source patch idempotency and fail closed PASS')
