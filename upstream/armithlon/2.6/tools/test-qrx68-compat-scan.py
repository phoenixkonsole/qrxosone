#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path
p = Path(__file__).resolve().parents[1]
scanner = p/'tools/qrx68-compat-scan.py'
qrx68 = Path(sys.argv[1])
with tempfile.TemporaryDirectory() as t:
    root = Path(t)
    (root/'valid.hunk').write_bytes((p/'data/qrx68/QRX68-Hello.hunk').read_bytes())
    (root/'malformed.hunk').write_bytes(bytes.fromhex('000003f3')+b'\0'*24)
    (root/'random.bin').write_bytes(b'ABCDEF'*64)
    (root/'symlink.hunk').symlink_to(root/'valid.hunk')
    out=root/'report.json'
    proc=subprocess.run([sys.executable,str(scanner),str(root), '--runtime',str(qrx68),
                         '--out',str(out),'--budget','1000'],capture_output=True,text=True,
                        timeout=12)
    assert proc.returncode==0,(proc.stdout,proc.stderr)
    j=json.loads(out.read_text())
    assert j['samples']==2,j
    assert j['status_counts']['PASS']==1,j
    assert j['status_counts']['GUEST_FAULT']==1,j
    assert len(j['files'])==2,j
print('PASS QRX68 bounded batch scanner: genuine HUNK, malformed, non-HUNK, symlink')
