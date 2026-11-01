#!/usr/bin/env python3
"""True host app-router -> QRX68 HUNK -> 68k Exec/DOS ABI end-to-end test."""
from pathlib import Path
import os
import subprocess
import sys

qrx_app = Path(sys.argv[1])
qrx68 = Path(sys.argv[2])
demo = Path(__file__).resolve().parents[1] / 'data/qrx68/QRX68-Hello.hunk'
env = dict(os.environ, QRX_HOST_68K=str(qrx68))
r = subprocess.run([str(qrx_app), 'launch', str(demo)], env=env,
                   text=True, capture_output=True, timeout=10, check=False)
assert r.returncode == 0, (r.stdout, r.stderr)
assert r.stdout == 'Hello QRX68\n', (r.stdout, r.stderr)
assert 'HLE calls, 0 unsupported' in r.stderr, r.stderr
print('PASS qrx-app -> isolated QRX68 HUNK -> real 68k CPU/LVO -> DOS Write stdout')
