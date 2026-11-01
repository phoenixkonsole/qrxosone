#!/usr/bin/env python3
"""HUNK/Exec/DOS + graphics smoke under real, externally fetched Musashi.
Only registered when QRX_ENABLE_MUSASHI=ON; never presented as passing here.
"""
import subprocess,sys,tempfile
from pathlib import Path
binary=Path(sys.argv[1]);hello=Path(sys.argv[2]);gui=Path(sys.argv[3]);
for name,app,extra in [('hello',hello,[]),('gui',gui,['--experimental-gui'])]:
    with tempfile.TemporaryDirectory(prefix='qrx68-musashi23-') as td:
        args=[str(binary),'--budget','100000',*extra]
        if name=='gui': args+=['--snapshot',str(Path(td)/'snapshot.ppm')]
        args.append(str(app))
        p=subprocess.run(args,capture_output=True,text=True,timeout=30)
        assert p.returncode==0,(name,p.stdout,p.stderr)
        assert 'CPU=Musashi-68040' in p.stderr,p.stderr
        assert '0 unsupported APIs' in p.stderr,p.stderr
        if name=='hello':assert 'Hello QRX68' in p.stdout
        if name=='gui':
            data=(Path(td)/'snapshot.ppm').read_bytes()
            assert data.startswith(b'P6\n640 256\n255\n') and len(data)==491535
print('PASS: HUNK programs actually executed by Musashi 68040 with Exec/DOS and graphics HLE')
