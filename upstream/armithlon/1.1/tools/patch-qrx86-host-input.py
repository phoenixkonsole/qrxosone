#!/usr/bin/env python3
"""Idempotent QRX86 22.5.12 native-host interactive backend adapter.
Do not change original AROS behavior; script-driven headless remains default.
"""
from pathlib import Path
import sys
if len(sys.argv) != 2:
    raise SystemExit('Usage: patch-qrx86-host-input.py /path/to/qrxosone')
root=Path(sys.argv[1]).resolve()
candidates=list(root.glob('qrx-os-one/*/x86-runtime-22.5.12/win32/backend_headless.c'))
if len(candidates)!=1:
    raise SystemExit(f'Expected exactly one QRX86 22.5.12 headless backend, got {len(candidates)}')
p=candidates[0]
s=p.read_text()
if 'QRX86_HOST_INPUT_BRIDGE_0_8' in s:
    print('Interactive host patch already applied:',p)
    raise SystemExit(0)
markers={
'#include "qrxwin.h"':'''#include "qrxwin.h"
/* QRX86_HOST_INPUT_BRIDGE_0_8: Native host input is separate from the
 * existing deterministic script/test backend. Never build for AROS m68k. */
#if defined(QRX86_HOST_INPUT) && !defined(__AROS__)
#include "qrx86_input.h"
#include <time.h>
static QRX86InputListener s_host_input = {.fd = -1};
static int s_interactive = 0;
static uint64_t host_now_ms(void){
    struct timespec t={0};
    if(clock_gettime(CLOCK_MONOTONIC,&t)!=0)return 0;
    return (uint64_t)t.tv_sec*1000u + (uint64_t)t.tv_nsec/1000000u;
}
#endif''',
'static int hl_init(qwin *w) { (void)w; g_hl.now = 1000; return 1; }':'''static int hl_init(qwin *w) {
    (void)w; g_hl.now = 1000;
#if defined(QRX86_HOST_INPUT) && !defined(__AROS__)
    const char *path=getenv("ARMITHLON_QRX86_INPUT_SOCKET");
    if(path && *path && !g_hl.script){
        if(!qrx86_input_listen(&s_host_input,path)){
            fprintf(stderr,"QRX86 host input socket unavailable: %s\\n",path);
            return 0;
        }
        s_interactive=1;
    }
#endif
    return 1;
}''',
'static uint64_t hl_now(qwin *w) { hl_tick(w); return g_hl.now; }':'''static uint64_t hl_now(qwin *w) {
#if defined(QRX86_HOST_INPUT) && !defined(__AROS__)
    if(s_interactive){(void)w;return host_now_ms();}
#endif
    hl_tick(w); return g_hl.now;
}''',
'''static int hl_wait_outer(qwin *w, uint32_t timeout, qevent *ev)
{
    if (hl_pending_up) { *ev = *hl_pending_up; hl_pending_up = NULL; return 1; }
    return hl_wait(w, timeout, ev);
}''':'''static int hl_wait_outer(qwin *w, uint32_t timeout, qevent *ev)
{
#if defined(QRX86_HOST_INPUT) && !defined(__AROS__)
    if(s_interactive){
        (void)w;
        QRX86InputEvent in={0};
        int n=qrx86_input_wait(&s_host_input,&in,timeout);
        if(n<=0)return n;
        memset(ev,0,sizeof(*ev));
        ev->x=(int)in.x;ev->y=(int)in.y;
        ev->vk=(int)in.code;ev->ch=(int)in.character;
        ev->button=(int)in.button;
        switch(in.type){
        case QRX86_INPUT_KEY_DOWN: ev->type=QE_KEYDOWN;break;
        case QRX86_INPUT_KEY_UP: ev->type=QE_KEYUP;break;
        case QRX86_INPUT_MOUSE_MOVE: ev->type=QE_MOUSEMOVE;break;
        case QRX86_INPUT_MOUSE_DOWN: ev->type=QE_MOUSEDOWN;break;
        case QRX86_INPUT_MOUSE_UP: ev->type=QE_MOUSEUP;break;
        default: return 0;
        }
        return 1;
    }
#endif
    if (hl_pending_up) { *ev = *hl_pending_up; hl_pending_up = NULL; return 1; }
    return hl_wait(w, timeout, ev);
}''',
'static void hl_shutdown(qwin *w) { (void)w; if (g_hl.script) fclose(g_hl.script); g_hl.script = NULL; }':'''static void hl_shutdown(qwin *w) {
    (void)w; if (g_hl.script) fclose(g_hl.script); g_hl.script = NULL;
#if defined(QRX86_HOST_INPUT) && !defined(__AROS__)
    if(s_interactive){qrx86_input_close(&s_host_input);s_interactive=0;}
#endif
}'''
}
for old,new in markers.items():
    if s.count(old)!=1:
        raise SystemExit(f'Refusing partial patch: changed source marker: {old[:85]}')
for old,new in markers.items():
    s=s.replace(old,new)
p.write_text(s)
print('Applied optional native-input QRX86 22.5.12 adapter:',p)
