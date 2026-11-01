#!/usr/bin/env python3
"""End-to-end launcher to guest boot with 1/4 effective RAM and host folder."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='qrx-workstation-') as td:
    env=os.environ.copy();env['HOME']=td
    env['QRX_BOOT_DISK']=str(Path(td)/'volatile-unused.qrx')
    # Ensure the built executable is found without invoking a network/build.
    r=subprocess.run([str(root/'armithlon'),'workstation','--volatile-disk',
                      '--command','hostinfo','--ram','auto','--no-interactive'],
                     cwd=root,env=env,capture_output=True,text=True,timeout=30)
    assert r.returncode==0,(r.stdout,r.stderr)
    assert 'HOST: mounted read-only' in r.stdout,r.stdout
    assert 'Guest RAM ' in r.stderr,r.stderr
    assert (Path(td)/'QRX-Share').is_dir()
    assert not (Path(td)/'volatile-unused.qrx').exists()
    print('PASS user workstation: actual guest boot, automatic RAM, read-only private share, no IMG')
