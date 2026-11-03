#!/usr/bin/env python3
"""Version-pinned, fail-closed QRXDesktop 6.6.20 QRX68 double-click routing.

Normal AROS HUNK remains native. QRXMODE=QRX68 icon ToolType or a file ending
in .qrx68 runs via installed QRX68Run, which queues the HUNK for the host bridge.
This is an AROS source patch, NOT a compiled or tested guest desktop.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('p23', here/'patch-qrxdesktop-qrx68-23.py')
p23 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p23)
TAG = '/* QRXDesktop-2.4-QRX68-Host-Bridge */'
FIRST = '''    if (!has_icon || !IconBase || !full) return 0;'''
SECOND = '''    lock=Lock((STRPTR)"QRX:C/QRX68Run",ACCESS_READ);'''
THIRD = '''    if (!lock){
        add_line("QRX68: AROS guest handler QRX:C/QRX68Run missing.");return 0;
    }
    UnLock(lock);
    { LONG rc=qrx19_launch_guest("QRX:C/QRX68Run",full);return rc<0?0:rc; }'''


def patch(source):
    if TAG in source:
        return source
    s = p23.patch(source)
    for item in (FIRST,SECOND,THIRD):
        if s.count(item) != 1:
            raise ValueError('QRXDesktop 2.3 source layout drift; refusing automatic patch')
    s=s.replace(FIRST, '''    ''' + TAG + '''
    /* Explicit extension selects isolated QRX68; standard HUNK stays native. */
    if (full && qrx_ci_ends(full,".qrx68")) return 1;
''' + FIRST,1)
    s=s.replace(SECOND, '''    lock=Lock((STRPTR)"QRX:C/QRX68Run",ACCESS_READ);''',1)
    s=s.replace(THIRD, '''    {
        const char *runner="QRX:C/QRX68Run";
        if (!lock) {
            runner="C:QRX68Run";
            lock=Lock((STRPTR)runner,ACCESS_READ);
        }
        if (!lock) {
            add_line("QRX68: QRX68Run not installed on QRX: or C:.");return 0;
        }
        UnLock(lock);
        { LONG rc=qrx19_launch_guest(runner,full);return rc<0?0:rc; }
    }''',1)
    return s


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source',type=Path)
    ap.add_argument('output',type=Path)
    args=ap.parse_args()
    raw=args.source.read_bytes()
    if (TAG.encode() not in raw and p23.TAG.encode() not in raw and
       p23.mod.TAG.encode() not in raw and
       hashlib.sha256(raw).hexdigest()!=p23.mod.UPSTREAM_SHA256):
        ap.error('unreviewed QRXDesktop source; expected canonical pinned version')
    result=patch(raw.decode('utf-8'))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(result)
    print('Patched source:',args.output,'SHA256:',hashlib.sha256(result.encode()).hexdigest())

if __name__=='__main__':main()
