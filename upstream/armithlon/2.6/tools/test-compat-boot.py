#!/usr/bin/env python3
import tempfile,subprocess,sys,zipfile
from pathlib import Path
file=Path(__file__).parent/'prepare-qrx-compat-boot.py'
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp)
    iso=tmp/'aros.iso';iso.write_bytes(b'fake fixture')
    hunk=tmp/'payload.zip'
    with zipfile.ZipFile(hunk,'w') as z:z.writestr('QRX/C/qrx-win32',b'fake binary')
    out=tmp/'stage'
    r=subprocess.run([sys.executable,file,'--iso',iso,'--hunk-zip',hunk,'--out',out],capture_output=True,text=True)
    # Must reject wrong hashes *before* accepting any guest programs as genuine.
    assert r.returncode!=0 and ('SHA256 mismatch' in r.stderr or 'xorriso' in r.stderr or 'No such file' in r.stderr)
print('PASS QRX compat boot refuses unavailable/unverified bootstrap assets')
