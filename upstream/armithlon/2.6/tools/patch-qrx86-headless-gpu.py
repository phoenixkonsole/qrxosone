#!/usr/bin/env python3
"""Idempotently attach *host-native* QRX86 22.5.12 headless qsurf to GPU ABI.

  python3 tools/patch-qrx86-headless-gpu.py /path/to/qrxosone

The target source is patched only after checking exact original markers.
The bridge is opt-in: define QRX86_HOST_GPU and link ARMITHLON's core lib.
AROS m68k builds never enable this native-only plugin (pthreads in host lib).
No original HLE API behavior or QRX86 JIT selection is changed.
"""
from pathlib import Path
import sys
if len(sys.argv)!=2:
    raise SystemExit('Usage: patch-qrx86-headless-gpu.py /path/to/qrxosone')
root=Path(sys.argv[1]).resolve()
files=list(root.glob('qrx-os-one/*/x86-runtime-22.5.12/win32/backend_headless.c'))
if len(files)!=1:raise SystemExit(f'Expected one QRX86 22.5.12 source, found {len(files)}')
p=files[0]
s=p.read_text()
marker='static void hl_present(qwin *w, qwnd *t) { (void)w; (void)t; }'
if 'QRX86_HOST_GPU' in s:
    print('QRX86 GPU backend patch already present:',p)
    raise SystemExit(0)
assert s.count(marker)==1,'Refuse patch: target hl_present signature has changed'
s=s.replace(marker,'''/* ARMITHLON QRX86 -> host GPU optional backend. qsurf.pix is a native
 * 0x00RRGGBB array, not an x86 guest pointer. This hook is invoked only when
 * the existing QRX86 Win32 HLE calls backend present(). */
#if defined(QRX86_HOST_GPU) && !defined(__AROS__)
extern void qrx86_gpu_backend_present(const uint32_t *pixels,int width,int height);
#endif
static void hl_present(qwin *w, qwnd *t) {
    (void)w;
#if defined(QRX86_HOST_GPU) && !defined(__AROS__)
    if(t && t->surf.pix && t->surf.w>0 && t->surf.h>0)
        qrx86_gpu_backend_present(t->surf.pix,t->surf.w,t->surf.h);
#else
    (void)t;
#endif
}''')
p.write_text(s)
print('Applied opt-in host-GPU frame hook:',p)
