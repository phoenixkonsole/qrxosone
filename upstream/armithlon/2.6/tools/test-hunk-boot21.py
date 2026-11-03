#!/usr/bin/env python3
"""Actual standalone QRX68k firmware test of Amiga HUNK CODE (no AmigaOS calls)."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

def words(*w):
    return b''.join(struct.pack('>I', x) for x in w)

p = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='qrx-hunk21-') as d:
    share = Path(d)
    code = bytes.fromhex('49f900ff000c18bc004818bc004b18bc000a4e75')
    assert len(code) == 20
    hunk = words(0x3f3,0,1,0,0,5,0x3e9,5) + code + words(0x3f2)
    (share/'TEST.HUNK').write_bytes(hunk)
    r = subprocess.run([str(p),'--ram-mib','96','--share',str(share),
                        '--volatile-disk','--command','run HOST:TEST.HUNK',
                        '--no-interactive'],capture_output=True,text=True,timeout=20)
    assert r.returncode == 0,(r.returncode,r.stdout,r.stderr)
    assert 'Launching host-mounted Amiga HUNK' in r.stdout,r.stdout
    assert 'HK\n' in r.stdout,r.stdout
    assert '96 MiB' in r.stderr,r.stderr
    # Reject the same binary on bad checksum/short file.
    (share/'TEST.HUNK').write_bytes(hunk[:-3])
    r = subprocess.run([str(p),'--ram-mib','96','--share',str(share),
                        '--volatile-disk','--command','run HOST:TEST.HUNK',
                        '--no-interactive'],capture_output=True,text=True,timeout=20)
    assert r.returncode == 0 and 'ERR: not a valid host' in r.stdout,r.stdout
    print('PASS actual 68k Amiga HUNK executable via HOST: with 96MiB RAM; malformed input rejected')
