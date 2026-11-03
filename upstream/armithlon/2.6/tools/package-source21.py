#!/usr/bin/env python3
"""Deterministic cumulative source ZIP including a file-by-file SHA256 manifest."""
import hashlib
from pathlib import Path
import sys
import zipfile
ROOT=Path(__file__).resolve().parents[1]
DEST=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT.parent/(ROOT.name+'-Cumulative-Source.zip')
EXCLUDED={'SHA256SUMS','SOURCE-MANIFEST.sha256'}
files=sorted(p for p in ROOT.rglob('*') if p.is_file() and
    p.relative_to(ROOT).parts[0] not in ('data','build-qrx68100','build-qrx68100-full') and
    '__pycache__' not in p.parts and p.suffix not in ('.pyc','.o'))
files=[p for p in files if p.name not in EXCLUDED]
manifest=''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(ROOT)}\n' for p in files)
for name in EXCLUDED:(ROOT/name).write_text(manifest)
files.extend(ROOT/name for name in sorted(EXCLUDED))
DEST.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(DEST,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for p in sorted(files):
        info=zipfile.ZipInfo(f'{ROOT.name}/{p.relative_to(ROOT)}',(2026,11,1,0,0,0))
        info.compress_type=zipfile.ZIP_DEFLATED
        info.external_attr=((0o755 if p.stat().st_mode & 0o111 else 0o644)&0xffff)<<16
        z.writestr(info,p.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
with zipfile.ZipFile(DEST) as z:
    assert z.testzip() is None
    for line in manifest.splitlines():
        sha,rel=line.split('  ',1)
        assert hashlib.sha256(z.read(f'{ROOT.name}/{rel}')).hexdigest()==sha
print(f'{DEST} files={len(files)} bytes={DEST.stat().st_size} sha256={hashlib.sha256(DEST.read_bytes()).hexdigest()}')
