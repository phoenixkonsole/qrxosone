#!/usr/bin/env python3
import subprocess
from pathlib import Path
import sys
import tempfile
P = Path(__file__).resolve().with_name('amigaos3-profile.py')
with tempfile.TemporaryDirectory(prefix='qrx-amigaos3-') as td:
    t = Path(td)
    osdir=t/'sys';(osdir/'S').mkdir(parents=True)
    (osdir/'S'/'Startup-Sequence').write_text('echo boot\n')
    rom=t/'licensed-placeholder.rom';rom.write_bytes(bytes(512*1024))  # shape only, NO licensed ROM
    share=t/'shared';share.mkdir()
    out=t/'guest.fs-uae'
    cmd=[sys.executable,str(P),'--kickstart',str(rom),'--system-dir',str(osdir),
         '--share',str(share),'--output',str(out),'--dry-run']
    a=subprocess.run(cmd,capture_output=True,text=True)
    assert a.returncode==0,(a.stderr,a.stdout)
    txt=out.read_text()
    assert 'cpu = 68040' in txt and 'jit_compiler = 0' in txt
    assert 'hard_drive_0 = ' in txt and 'hard_drive_1 = ' in txt
    assert 'QRXSHARE' in txt and 'kickstart_file' in txt
    assert subprocess.run(cmd[:-1]+['--cpu','68060','--dry-run'],capture_output=True).returncode==0
    (osdir/'S'/'Startup-Sequence').unlink()
    assert subprocess.run(cmd,capture_output=True).returncode==2
    print('PASS real AmigaOS3 FS-UAE profile configurator validates paths and preserves single-core compatibility')
