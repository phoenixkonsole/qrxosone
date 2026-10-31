#!/usr/bin/env python3
from pathlib import Path
import re, sys

if len(sys.argv) != 2:
    raise SystemExit('usage: patch-qrxdesktop-workbench-aware.py qrx_desktop.c')

p=Path(sys.argv[1]); s=p.read_text(errors='replace')
marker='/* QRX v0.20.3.1 Workbench-aware startup */'
proto='/* QRX v0.20.3.2 prototype-safe patch */'
anchor='#include <proto/intuition.h>'
if anchor not in s: raise SystemExit('proto/intuition.h include anchor missing')
pos=s.find('\n',s.find(anchor))+1
prefix=s[:pos]; extra=''
if '#include <stdlib.h>' not in prefix: extra+='#include <stdlib.h>\n'
if '#include <proto/dos.h>' not in prefix: extra+='#include <proto/dos.h>\n'
if extra:
    s=s[:pos]+extra+s[pos:]
    pos += len(extra)

if marker not in s:
    helper='''
/* QRX v0.20.3.1 Workbench-aware startup */
enum QRXScreenPolicy { QRX_SCREEN_AUTO=0, QRX_SCREEN_OWN, QRX_SCREEN_WORKBENCH };
static enum QRXScreenPolicy qrx_screen_policy = QRX_SCREEN_AUTO;
static struct Screen *qrx_locked_workbench = NULL;
static int qrx_using_foreign_screen = 0;
static int qrx_strieq(const char *a,const char *b){if(!a||!b)return 0;while(*a&&*b){char ca=*a,cb=*b;if(ca>='a'&&ca<='z')ca-=32;if(cb>='a'&&cb<='z')cb-=32;if(ca!=cb)return 0;++a;++b;}return *a==0&&*b==0;}
static void qrx_detect_screen_policy(void){char *v=getenv("QRXSCREEN");qrx_screen_policy=QRX_SCREEN_AUTO;if(!v||!*v)return;if(qrx_strieq(v,"OWN"))qrx_screen_policy=QRX_SCREEN_OWN;else if(qrx_strieq(v,"WORKBENCH"))qrx_screen_policy=QRX_SCREEN_WORKBENCH;}
static struct Screen *qrx_try_lock_workbench(void){struct Screen *sc=NULL;int tries;for(tries=0;tries<20&&!sc;++tries){sc=LockPubScreen((STRPTR)"Workbench");if(!sc)Delay(5);}return sc;}
static void qrx_release_foreign_screen(void){if(qrx_locked_workbench){UnlockPubScreen((STRPTR)"Workbench",qrx_locked_workbench);qrx_locked_workbench=NULL;}qrx_using_foreign_screen=0;}
'''
    s=s[:pos]+helper+s[pos:]
if proto not in s: s=s.replace(marker,marker+'\n'+proto,1)

m=re.search(r'(?s)(static\s+int\s+qrx_open_runtime_screen\s*\([^)]*\)\s*\{)(.*?)(\n\})',s)
if not m: raise SystemExit('qrx_open_runtime_screen not found')
body=m.group(2)
if 'qrx_detect_screen_policy();' not in body:
    body='''
    qrx_detect_screen_policy();
    if (qrx_screen_policy != QRX_SCREEN_OWN) {
        qrx_locked_workbench = qrx_try_lock_workbench();
        if (qrx_locked_workbench) { qrx_using_foreign_screen = 1; PutStr("QRX: Workbench/Wanderer detected; preserving foreign screen.\\n"); }
        else if (qrx_screen_policy == QRX_SCREEN_WORKBENCH) PutStr("QRX: requested WORKBENCH screen not available; falling back to OWN.\\n");
    }
'''+body
body=re.sub(r'(?m)^(\s*)CloseScreen\(([^;]+)\);',r'\1if (!qrx_using_foreign_screen) CloseScreen(\2);',body)
s=s[:m.start()]+m.group(1)+body+m.group(3)+s[m.end():]
p.write_text(s)
chk=p.read_text(errors='replace')
assert marker in chk and proto in chk and 'getenv("QRXSCREEN")' in chk and 'LockPubScreen((STRPTR)"Workbench")' in chk
_g = chk.index('getenv("QRXSCREEN")')
_d = chk.index("Delay(5)")
assert chk.rfind("#include <stdlib.h>", 0, _g) >= 0
assert chk.rfind("#include <proto/dos.h>", 0, _d) >= 0
print('QRXDesktop Workbench patch OK (declaration-order safe):',p)
