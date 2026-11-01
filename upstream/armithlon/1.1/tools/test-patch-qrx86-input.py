#!/usr/bin/env python3
"""Tests source signature gate and idempotent QRX86 22.5.12 host-input patch."""
from pathlib import Path
import tempfile,subprocess,sys
script=Path(__file__).with_name('patch-qrx86-host-input.py')
markers='''#include "qrxwin.h"
static int hl_init(qwin *w) { (void)w; g_hl.now = 1000; return 1; }
static uint64_t hl_now(qwin *w) { hl_tick(w); return g_hl.now; }
static int hl_wait_outer(qwin *w, uint32_t timeout, qevent *ev)
{
    if (hl_pending_up) { *ev = *hl_pending_up; hl_pending_up = NULL; return 1; }
    return hl_wait(w, timeout, ev);
}
static void hl_shutdown(qwin *w) { (void)w; if (g_hl.script) fclose(g_hl.script); g_hl.script = NULL; }
'''
with tempfile.TemporaryDirectory() as d:
    root=Path(d)
    target=root/'qrx-os-one/demo/x86-runtime-22.5.12/win32/backend_headless.c'
    target.parent.mkdir(parents=True)
    target.write_text(markers)
    subprocess.run([sys.executable,str(script),str(root)],check=True,stdout=subprocess.PIPE)
    once=target.read_bytes()
    assert once!=markers.encode()
    for signal in ('QRX86_HOST_INPUT_BRIDGE_0_8','qrx86_input_wait','QRX86_INPUT_MOUSE_DOWN','host_now_ms'):
        assert signal.encode() in once
    subprocess.run([sys.executable,str(script),str(root)],check=True,stdout=subprocess.PIPE)
    assert target.read_bytes()==once
    bad=target.read_text().replace('QRX86_HOST_INPUT_BRIDGE_0_8','x').replace('static int hl_init(qwin *w) {','static int altered(qwin *w) {')
    target.write_text(bad)
    rc=subprocess.run([sys.executable,str(script),str(root)],capture_output=True)
    assert rc.returncode!=0 and target.read_text()==bad
print('QRX86 22.5.12 host-input source patch + idempotency + fail-closed PASS')
