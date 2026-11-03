#!/usr/bin/env python3
"""Version-guarded optional QRX68 QRXDesktop integration.
QRXMODE=QRX68 icon ToolType -> QRX:C/QRX68Run only when installed.
Normal native 68k HUNK launch is unchanged (AmigaOS/AROS ABI first).
This does NOT implement an AROS QRX68 service or install a guest binary.
"""
from pathlib import Path
import argparse,hashlib,importlib.util
root=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('qrx_19',root/'patch-qrxdesktop-launch19.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
TAG='/* QRXDesktop-2.3-OptIn-QRX68-HUNK */'
MARK='    if (dir_type > 0) return open_filemanager(full);'
HELPER=r'''/* QRXDesktop-2.3-OptIn-QRX68-HUNK */
/* Native HUNK remains default; only explicit ToolType triggers compatibility. */
static int qrx23_compat_selected(const char *full, UBYTE has_icon) {
    struct DiskObject *d;
    const char *value;
    int match=0;
    if (!has_icon || !IconBase || !full) return 0;
    d=GetDiskObject((STRPTR)full);
    if (!d) return 0;
    value=qrx_tooltype_value(d->do_ToolTypes,"QRXMODE");
    if (value && !qrx_ci_cmp(value,"QRX68"))match=1;
    FreeDiskObject(d);
    return match;
}
static int qrx23_is_hunk(const char *path) {
    UBYTE header[4]; BPTR f; LONG n;
    if (!path || !*path)return 0;
    f=Open((STRPTR)path, MODE_OLDFILE);
    if (!f)return 0;
    n=Read(f,header,4);Close(f);
    return n==4 && header[0]==0 && header[1]==0 &&
           header[2]==3 && header[3]==0xf3;
}
static LONG qrx23_open_compat(const char *full, UBYTE has_icon){
    BPTR lock;
    if (!qrx23_compat_selected(full,has_icon))return -1; /* native */
    if (!qrx23_is_hunk(full)){
        add_line("QRX68: QRXMODE=QRX68 applies to Amiga HUNK only.");return 0;
    }
    lock=Lock((STRPTR)"QRX:C/QRX68Run",ACCESS_READ);
    if (!lock){
        add_line("QRX68: AROS guest handler QRX:C/QRX68Run missing.");return 0;
    }
    UnLock(lock);
    { LONG rc=qrx19_launch_guest("QRX:C/QRX68Run",full);return rc<0?0:rc; }
}

'''
def patch(original):
    if TAG in original:return original
    src=mod.patch(original)
    if src.count(mod.HEAD)!=1:raise ValueError('launch hook not unique')
    helperpos=src.index(mod.HEAD)
    src=src[:helperpos]+HELPER+src[helperpos:]
    first=src.index(mod.HEAD)
    pos=src.find(MARK,first)
    if pos<0 or src.find(MARK,pos+len(MARK))>=0:raise ValueError('desktop open-path layout drift')
    src=src[:pos+len(MARK)]+'''\n    /* Never change ordinary native Amiga HUNK launch. */
    { LONG compatibility=qrx23_open_compat(full,has_icon);
      if (compatibility>=0) return compatibility; }
'''+src[pos+len(MARK):]
    return src

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source',type=Path);ap.add_argument('output',type=Path)
    a=ap.parse_args()
    data=a.source.read_bytes()
    known=mod.UPSTREAM_SHA256
    if TAG.encode() not in data and mod.TAG.encode() not in data and hashlib.sha256(data).hexdigest()!=known:
        ap.error('unreviewed QRXDesktop revision: fail closed, expected pinned SHA')
    result=patch(data.decode('utf-8'))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(result)
    print(a.output,hashlib.sha256(result.encode()).hexdigest())
if __name__=='__main__':main()
