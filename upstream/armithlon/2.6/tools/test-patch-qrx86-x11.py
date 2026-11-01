#!/usr/bin/env python3
"""Hermetic patch idempotence, drift detection and isolation checks."""
from pathlib import Path
import tempfile, sys, subprocess, shutil
sys.path.insert(0, str(Path(__file__).resolve().parent))
import importlib.util
path=Path(__file__).parent/'patch-qrx86-x11-host-gpu.py'
spec=importlib.util.spec_from_file_location('qxx_patch',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
with tempfile.TemporaryDirectory() as t:
    root=Path(t)
    x=root/'qrx-os-one'/'audit'/'x86-runtime-22.5.12'/'linux'/'qrx_x11.c'
    x.parent.mkdir(parents=True)
    original='''#include "qrx_x11.h"
#include <stdio.h>
#include <stdlib.h>
typedef struct {int mapped,w,h;unsigned *pixels;}qxx_window;
static void native_present(qxx_window *w) {
    const char *dir = getenv("QRX_X11_DUMP_DIR");
    char path[1024];
    FILE *f;
    int i;
    if (!dir || !w->mapped) return;
    (void)path;(void)f;(void)i;
}
'''
    x.write_text(original)
    (x.parent / "qrx_x11.h").write_text("/* fixture */\n")
    p,changed=mod.patch(root)
    assert p==x and changed
    updated=x.read_text()
    assert mod.MARK in updated and updated.count(mod.MARK)==1
    assert mod.patch(root)==(x,False) and x.read_text()==updated
    # The integrated code must compile in host mode and in non-host mode.
    for flags in ([],['-DQRX86_HOST_X11_GPU']):
        shim=root/'qrx_x11_bridge.h'
        shim.write_text('#include <stdint.h>\nint qrx_x11_bridge_present(const unsigned *,uint32_t,uint32_t);\n')
        cmd=['cc','-std=c99','-fsyntax-only','-I',str(root),*flags,str(x)]
        subprocess.run(cmd,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    x.write_text(original.replace('    int i;','    volatile int i;'))
    try:mod.patch(root)
    except ValueError as e:assert 'drift' in str(e)
    else:raise AssertionError('Drift not detected')
print('QRX latest-X11 signature-gated patch, host compile, idempotence, drift PASS')
