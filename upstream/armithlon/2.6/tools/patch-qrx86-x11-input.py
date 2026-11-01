#!/usr/bin/env python3
"""Inject opt-in macOS Q86I input into QRX's actual embedded X11 server.

Anchors refer to phoenixkonsole/qrxosone @ cc2ba06a. Never run on Git checkout:
only a disposable build staging copy is allowed. Preserves AROS X11 backend.
"""
import sys
from pathlib import Path
MARK='ARMITHLON_QRX_X11_INPUT_1_3'
ANCHOR='void qxx_pump(qxx *x) { (void)x; }'
REPLACE=r'''/* ARMITHLON_QRX_X11_INPUT_1_3; host-native, limited single X11 client. */
static qxx *qrx_input_owner;
static QRX86InputListener qrx_input_listener = { .fd = -1 };
static int qrx_input_started;
static void qrx_x11_input_shutdown(qxx *x) {
    if (qrx_input_owner != x) return;
    if (qrx_input_started) qrx86_input_close(&qrx_input_listener);
    qrx_input_owner = NULL; qrx_input_started = 0;
}
void qxx_pump(qxx *x) {
    QRX86InputEvent e;
    qxx_window *w=NULL;
    unsigned i;
    const char *sock;
    if (!x || !x->setup) return;
    sock = getenv("ARMITHLON_QRX86_INPUT_SOCKET");
    if (!sock || !*sock) return;
    if (!qrx_input_owner) {
        qrx_input_owner = x;
        qrx_input_started = qrx86_input_listen(&qrx_input_listener, sock);
    }
    if (qrx_input_owner != x || !qrx_input_started) return;
    for (i=0; i<QXX_WINDOWS; i++)
        if (x->windows[i].id && x->windows[i].mapped) { w=&x->windows[i]; break; }
    if (!w) return;
    for (i=0; i<12; i++) {
        int v = qrx86_input_wait(&qrx_input_listener, &e, 0);
        int type=0, detail=0, px, py;
        if (v <= 0) break;
        px=(int)((uint64_t)e.x * (unsigned)w->w / 800u);
        py=(int)((uint64_t)e.y * (unsigned)w->h / 450u);
        if (px>=w->w) px=w->w-1;
        if (py>=w->h) py=w->h-1;
        switch(e.type) {
            case QRX86_INPUT_MOUSE_MOVE: type=6;break;
            case QRX86_INPUT_MOUSE_DOWN: type=4;detail=e.button==2?3:e.button==3?2:1;break;
            case QRX86_INPUT_MOUSE_UP: type=5;detail=e.button==2?3:e.button==3?2:1;break;
            case QRX86_INPUT_KEY_DOWN: type=2;detail=e.character ? (int)e.character : (int)e.code;break;
            case QRX86_INPUT_KEY_UP: type=3;detail=e.character ? (int)e.character : (int)e.code;break;
        }
        if (type && (w->mask & (1u << (type==6?6:type==4?2:type==5?3:type==2?0:1))))
            event(x,w,type,detail,px,py);
    }
}'''
INCLUDE='#include "qrx_x11_bridge.h"'
ADD=INCLUDE+'\n#include "qrx86_input.h"'
EXIT_ANCHOR='''void qxx_free(qxx *x) {
    unsigned i;
    if (!x)
        return;
'''
EXIT_ADD=EXIT_ANCHOR+'''#if defined(QRX86_HOST_X11_GPU) && !defined(__AROS__)
    qrx_x11_input_shutdown(x);
#endif
'''

def patch(top:Path):
    found=list(top.glob('qrx-os-one/*/x86-runtime-22.5.12/linux/qrx_x11.c'))
    if len(found)!=1:raise ValueError(f'Expected one X11 source file, got {len(found)}')
    file=found[0]
    data=file.read_text()
    if MARK in data:
        if REPLACE not in data or EXIT_ADD not in data or ADD not in data:
            raise ValueError('Partial X11 input patch detected')
        return file,False
    for old in [ANCHOR,EXIT_ANCHOR,INCLUDE]:
        if data.count(old)!=1:raise ValueError(f'Source drift on anchor {old[:25]}')
    if '#include "qrx86_input.h"' in data:raise ValueError('Unexpected existing host input')
    data=data.replace(INCLUDE,ADD,1).replace(ANCHOR,REPLACE,1).replace(EXIT_ANCHOR,EXIT_ADD,1)
    if data.count(MARK)!=1:raise ValueError('Patch failed')
    file.write_text(data)
    return file,True

if __name__=='__main__':
    if len(sys.argv)!=2:raise SystemExit('Usage: patch-qrx86-x11-input.py STAGING_DIR')
    try:
        p,changed=patch(Path(sys.argv[1]).resolve())
    except ValueError as e:raise SystemExit(str(e))
    print(('Patched' if changed else 'Idempotent')+': '+str(p))
