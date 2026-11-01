#!/usr/bin/env python3
"""End-to-end HUNK drawing tests; do not conflate with full Intuition/AROS GUI."""
import subprocess,sys,tempfile
from pathlib import Path

exe=Path(sys.argv[1]); hunk=Path(sys.argv[2]); assert exe.is_file() and hunk.is_file()
with tempfile.TemporaryDirectory(prefix='qrx68-gui23-') as tmp:
    image=Path(tmp)/'frame.ppm'
    args=[str(exe),'--budget','100000','--experimental-gui','--snapshot',str(image),str(hunk)]
    p=subprocess.run(args,capture_output=True,text=True,timeout=30)
    assert p.returncode==0,(p.stdout,p.stderr)
    assert 'unsupported APIs' in p.stderr and '0 unsupported APIs' in p.stderr,p.stderr
    data=image.read_bytes()
    prefix=b'P6\n640 256\n255\n'
    assert data.startswith(prefix)
    pix=data[len(prefix):]
    assert len(pix)==640*256*3,len(pix)
    def at(x,y):return pix[(y*640+x)*3:(y*640+x)*3+3]
    assert at(25,30)==bytes((64,96,244)),at(25,30)
    assert at(5,5)==bytes((0,0,0)),at(5,5)
    assert at(150,30)==bytes((0,0,0)),at(150,30)
    # Guest must explicitly request GUI permission; default OpenLibrary fails.
    p=subprocess.run([str(exe),'--budget','500','--snapshot',str(image),str(hunk)],capture_output=True,timeout=30)
    assert p.returncode!=0
print('PASS: real 68k HUNK -> Intuition OpenWindow -> graphics SetAPen/RectFill -> verified PPM, opt-in only')
