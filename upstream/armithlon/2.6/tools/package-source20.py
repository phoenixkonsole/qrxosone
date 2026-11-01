#!/usr/bin/env python3
"""Reproducible cumulative source ZIP (skips build artifacts and private data)."""
from pathlib import Path
import hashlib, zipfile
root=Path(__file__).resolve().parents[1]
out=root.parent/(root.name+'-Cumulative-Source.zip')
excluded={'SHA256SUMS','SOURCE-MANIFEST.sha256','.DS_Store'}
def keep(p):
    r=p.relative_to(root)
    if not p.is_file() or r.name in excluded or r.suffix in {'.pyc','.o','.a'}:return False
    if any(x in {'__pycache__','.git'} or x.startswith('build-qrx68100') or x.startswith('build-qrx86') for x in r.parts):return False
    if r.parts[0]=='data' and r.parts[1:2]==('qrxosone',):return False
    return True
entries=[(p,p.relative_to(root)) for p in sorted(root.rglob('*')) if keep(p)]
manifest=''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {r.as_posix()}\n' for p,r in entries)
for name in excluded.intersection({'SHA256SUMS','SOURCE-MANIFEST.sha256'}):
    (root/name).write_text(manifest)
for name in ('SHA256SUMS','SOURCE-MANIFEST.sha256'):
    entries.append((root/name,Path(name)))
with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for p,rel in entries:
        entry=zipfile.ZipInfo(root.name+'/'+rel.as_posix(),date_time=(2026,11,1,2,0,0))
        entry.create_system=3
        entry.compress_type=zipfile.ZIP_DEFLATED
        entry.external_attr=((0o100000|(p.stat().st_mode&0o777))<<16)
        z.writestr(entry,p.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
print(out)
print(f'{len(entries)} files, {out.stat().st_size} bytes')
print('SHA256',hashlib.sha256(out.read_bytes()).hexdigest())
