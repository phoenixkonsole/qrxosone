#!/usr/bin/env python3
"""ARMITHLON 1.2: optional QRX i386 X11 -> Metal socket hook.

Fail closed if the audited QRX X11 native_present implementation drifts.
Apply ONLY to an isolated native-host staging copy, not a source checkout.
Target: phoenixkonsole/qrxosone @ cc2ba06a (2026-10-31).
"""
from pathlib import Path
import sys

MARK = 'ARMITHLON_QRX86_X11_HOST_GPU_1_2'
INCLUDE = '#include "qrx_x11.h"'
ANCHOR = '''static void native_present(qxx_window *w) {
    const char *dir = getenv("QRX_X11_DUMP_DIR");
    char path[1024];
    FILE *f;
    int i;
'''
INSERT = '''static void native_present(qxx_window *w) {
    const char *dir = getenv("QRX_X11_DUMP_DIR");
    char path[1024];
    FILE *f;
    int i;
#if defined(QRX86_HOST_X11_GPU) && !defined(__AROS__)
    /* ARMITHLON_QRX86_X11_HOST_GPU_1_2: host-native X11 frames to Metal;
     * no guest pointers are exported, original PPM dumps remain supported. */
    if (w->mapped && w->pixels && w->w > 0 && w->h > 0)
        (void)qrx_x11_bridge_present(w->pixels, (uint32_t)w->w, (uint32_t)w->h);
#endif
'''

def patch(repo: Path):
    paths = list(repo.glob('qrx-os-one/*/x86-runtime-22.5.12/linux/qrx_x11.c'))
    if len(paths) != 1:
        raise ValueError(f'Expected exactly one QRX X11 file; found {len(paths)} in {repo}')
    target = paths[0]
    content = target.read_text(encoding='utf-8')
    if MARK in content:
        if INSERT not in content or '#include "qrx_x11_bridge.h"' not in content:
            raise ValueError('Partial/modified X11 integration, refusing to continue')
        return target, False
    if content.count(ANCHOR) != 1 or content.count(INCLUDE) != 1:
        raise ValueError('QRX X11 source drift: expected native_present and header signatures')
    if '#include "qrx_x11_bridge.h"' in content:
        raise ValueError('Unexpected preexisting X11 GPU integration')
    out = content.replace(INCLUDE, INCLUDE + '''
#if defined(QRX86_HOST_X11_GPU) && !defined(__AROS__)
#include "qrx_x11_bridge.h"
#endif''', 1).replace(ANCHOR, INSERT, 1)
    if out == content or out.count(MARK) != 1:
        raise ValueError('Internal X11 patch failure')
    target.write_text(out, encoding='utf-8')
    return target, True

if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: patch-qrx86-x11-host-gpu.py REPO_OR_STAGING_ROOT')
    try:
        target, changed = patch(Path(sys.argv[1]).resolve())
    except ValueError as exc:
        raise SystemExit(str(exc))
    print(('Patched' if changed else 'Already patched') + ': ' + str(target))
