#!/usr/bin/env python3
"""End-to-end QRX68/2: REAL 68k-HUNK event loop, cross-process host bridge,
atomic frame updates, IDCMP GetMsg/ReplyMsg, multiple concurrent programs.
No AROS/FS-UAE SDK or native Musashi CPU claimed here.
"""
from pathlib import Path
import subprocess
import tempfile
import time
import sys
import importlib.util

ROOT=Path(__file__).resolve().parents[1]
sp=importlib.util.spec_from_file_location('svc',ROOT/'integration/qrx68-bridge/qrx68_service.py')
s=importlib.util.module_from_spec(sp);sp.loader.exec_module(s)
RUNNER=Path(sys.argv[1]).resolve()
HUNK=(ROOT/'data/qrx68/QRX68-Live.hunk').read_bytes()
HEADER=b'P6\n640 256\n255\n'
def eventually(pred,delay=7):
    stop=time.monotonic()+delay
    while time.monotonic()<stop:
        try:
            if pred():return True
        except (FileNotFoundError,ValueError):pass
        time.sleep(.025)
    raise AssertionError('timed out waiting on live QRX68/2 execution')
def pixel(ppm,x,y):
    raw=ppm.read_bytes()
    assert raw.startswith(HEADER) and len(raw)==len(HEADER)+640*256*3
    return raw[len(HEADER)+(y*640+x)*3:len(HEADER)+(y*640+x)*3+3]

def main():
    assert s.validate_event(b'QRX68/2 0 8 104 0 50 50\n')==b'QRX68/2 0 8 104 0 50 50\n'
    for raw in (b'QRX68/2 0 9999 1 0 0 0\n',b'QRX68/2 4 8 1 0 0 0\n',
                b'QRX68/2 0 8 1 0 -999999 0\n',b'QRX68/2 0 8 1 0 0 0;bad\n'):
        try:s.validate_event(raw)
        except ValueError:pass
        else:raise AssertionError(f'accepted untrusted event {raw!r}')
    with tempfile.TemporaryDirectory(prefix='qrx68-live25-') as t:
        bridge=Path(t);s.setup_spool(bridge)
        for ident in ('LIVEAMIGA001','LIVEAMIGA002'):
            (bridge/'inbox'/f'{ident}.hunk').write_bytes(HUNK)
            (bridge/'inbox'/f'{ident}.req').write_bytes(s.MARKER)
        daemon=subprocess.Popen([sys.executable,str(ROOT/'integration/qrx68-bridge/qrx68_service.py'),
                                 '--bridge',str(bridge),'--runner',str(RUNNER),
                                 '--interactive','--interval','.02','--timeout','20'],
                                stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        try:
            for ident in ('LIVEAMIGA001','LIVEAMIGA002'):
                ppm=bridge/'results'/f'{ident}.ppm'
                eventually(lambda:(bridge/'results'/f'{ident}.live').is_file())
                eventually(lambda:pixel(ppm,25,30)==bytes([64,96,244]))
                assert not (bridge/'results'/f'{ident}.status').exists()
            # Malformed event is rejected, genuine mouse redraws the second window.
            i='LIVEAMIGA002';p=bridge/'inbox'
            (p/f'{i}.0000000001.evt').write_bytes(b'QRX68/2 0 1023 0 0 0 0\n')
            eventually(lambda: not (p/f'{i}.0000000001.evt').exists())
            assert pixel(bridge/'results'/f'{i}.ppm',115,35)==bytes([0,0,0])
            (p/f'{i}.0000000002.evt').write_bytes(b'QRX68/2 0 8 104 0 50 50\n')
            eventually(lambda:pixel(bridge/'results'/f'{i}.ppm',115,35)==bytes([238,44,44]))
            assert (bridge/'results'/f'{i}.frame').read_bytes().startswith(b'QRX68/2 FRAME 2')
            # Both sessions have separate CPU/GPU state.
            assert pixel(bridge/'results/LIVEAMIGA001.ppm',115,35)==bytes([0,0,0])
            for ident in ('LIVEAMIGA001','LIVEAMIGA002'):
                (p/f'{ident}.0000000003.evt').write_bytes(b'QRX68/2 0 512 0 0 0 0\n')
                eventually(lambda:(bridge/'results'/f'{ident}.status').exists())
                assert (bridge/'results'/f'{ident}.status').read_text().startswith('OK\n')
                assert b'QRX68: PASS' in (bridge/'results'/f'{ident}.err').read_bytes()
            print('PASS QRX68/2 concurrent real 68k HUNK IDCMP WaitPort/GetMsg/ReplyMsg, close, incremental frames, fail-closed event filtering')
        finally:
            daemon.terminate()
            try:daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:daemon.kill();daemon.wait()

if __name__=='__main__':main()
