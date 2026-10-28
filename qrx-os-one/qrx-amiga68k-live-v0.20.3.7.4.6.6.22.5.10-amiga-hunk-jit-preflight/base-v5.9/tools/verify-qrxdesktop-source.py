#!/usr/bin/env python3
from pathlib import Path
import re, sys
s = Path(sys.argv[1]).read_text(errors='replace')
tokens=("#include <proto/graphics.h>","#include <proto/dos.h>","#include <string.h>","#include <stdio.h>","#include <stdlib.h>","#define TERM_LINE_LEN","#define INPUT_LEN","#define PREFS_PATH","#define SESSION_NONE","#define SESSION_TAKEOVER","#define SESSION_WHDLOAD","#define TERM_MIN_W","#define TERM_MIN_H","#define FILE_MIN_W","#define FILE_MIN_H","#define CLIP_COPY","#define CLIP_MOVE","enum LookFeel","static struct Screen *g_screen","static struct Window *g_desktop","static struct Window *g_terminal","PoseidonBase","g_session_action")
missing=[x for x in tokens if x not in s]
stage=len(re.findall(r'(?m)^static\s+volatile\s+ULONG\s+qrx_boot_stage\s*=',s))
mark=len(re.findall(r'(?m)^static\s+void\s+qrx_boot_mark\s*\(',s))
p=s.find('#include <proto/intuition.h>'); q=s.find('struct QRXLaunchProfile')
head_ok=p>=0 and q>p
if head_ok:
    seg=s[p:q]
    for x in ('#include <proto/graphics.h>','#include <stdio.h>','#define TERM_LINE_LEN','#define SESSION_TAKEOVER','#define TERM_MIN_W','#define CLIP_COPY','enum LookFeel'):
        if x not in seg: head_ok=False
if missing or stage!=1 or mark!=1 or not head_ok:
    raise SystemExit(f'FAIL missing={missing} stage={stage} mark={mark} top_block={head_ok}')
getenv_pos=s.find('getenv("QRXSCREEN")')
stdlib_pos=s.rfind('#include <stdlib.h>',0,getenv_pos if getenv_pos>=0 else 0)
if getenv_pos<0 or stdlib_pos<0:
    raise SystemExit('FAIL declaration_order=stdlib_before_getenv')
wh_start=s.find('static struct Screen *qrx_try_lock_workbench(void)')
wh_end=s.find('}',wh_start)
if wh_start<0 or wh_end<0 or 'qrx_boot_stage' in s[wh_start:wh_end+1]:
    raise SystemExit('FAIL declaration_order=early_workbench_helper')
print(f'OK tokens={len(tokens)} stage={stage} mark={mark} top_block=yes')
