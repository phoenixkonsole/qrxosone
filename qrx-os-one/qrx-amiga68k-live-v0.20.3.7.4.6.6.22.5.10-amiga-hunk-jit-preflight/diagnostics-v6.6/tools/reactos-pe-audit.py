#!/usr/bin/env python3
import sys,struct,json
from pathlib import Path

def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def cstr(b,o):
    e=b.find(b'\0',o); return b[o:e if e>=0 else len(b)].decode('ascii','replace')
def pe(path):
    b=Path(path).read_bytes()
    if b[:2]!=b'MZ': raise ValueError('not MZ')
    peo=u32(b,0x3c)
    if b[peo:peo+4]!=b'PE\0\0': raise ValueError('not PE')
    nsec=u16(b,peo+6); opt=peo+24; magic=u16(b,opt)
    if magic!=0x10b: raise ValueError('not PE32')
    sec0=opt+u16(b,peo+20); imp_rva=u32(b,opt+104)
    secs=[]
    for i in range(nsec):
        o=sec0+40*i; va=u32(b,o+12); vs=u32(b,o+8); raw=u32(b,o+20); rs=u32(b,o+16); secs.append((va,max(vs,rs),raw))
    def off(rva):
        for va,sz,raw in secs:
            if va<=rva<va+sz:return raw+rva-va
        return rva if rva<len(b) else None
    imports=[]
    io=off(imp_rva) if imp_rva else None
    if io is not None:
        while io+20<=len(b):
            oft=u32(b,io); name=u32(b,io+12); ft=u32(b,io+16)
            if not (oft or name or ft): break
            no=off(name); dll=cstr(b,no).lower() if no is not None else '?'; funcs=[]; tr=off(oft or ft)
            if tr is not None:
                while tr+4<=len(b):
                    v=u32(b,tr); tr+=4
                    if not v: break
                    if v&0x80000000: funcs.append('#'+str(v&0xffff))
                    else:
                        x=off(v); funcs.append(cstr(b,x+2) if x is not None else '?')
            imports.append({'dll':dll,'imports':funcs}); io+=20
    return imports
root=Path(sys.argv[1]); names=sys.argv[2:] or ['calc.exe','notepad.exe','clock.exe']; starts=[root/x for x in names]
report={}; missing=set(); seen=set()
def walk(p):
    key=p.name.lower()
    if key in seen:return
    seen.add(key)
    try: im=pe(p)
    except Exception as e: report[str(p.relative_to(root))]={'error':str(e)}; return
    report[str(p.relative_to(root))]=im
    for d in im:
        q=root/d['dll']
        if q.exists(): walk(q)
        else: missing.add(d['dll'])
for p in starts:
    if p.exists(): walk(p)
    else: missing.add(p.name.lower())
out={'files_scanned':len(seen),'missing_files':sorted(missing),'graph':report}
print(json.dumps(out,indent=2))
if missing: sys.exit(2)
