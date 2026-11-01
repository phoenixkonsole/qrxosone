#!/usr/bin/env python3
"""Requires real, fetched Musashi. Tests actual 68040 boot/QEX and dual-core.
This test is never counted in portable CTest when Musashi is absent.
"""
import pathlib
import subprocess
import sys
import tempfile

exe=pathlib.Path(sys.argv[1]).resolve()
assert exe.exists(),exe
with tempfile.TemporaryDirectory(prefix='qrx-musashi19-') as work:
    disk=pathlib.Path(work)/'qrxf.img'
    def launch(*args):
        p=subprocess.run([str(exe),'--disk',str(disk),*args],
                         stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                         text=True,timeout=30)
        print('RUN',args,'rc',p.returncode,'STDOUT',p.stdout[-600:],'STDERR',p.stderr[-300:],flush=True)
        if p.returncode!=0:raise AssertionError('Musashi 68040 boot failed')
        return p.stdout,p.stderr
    out,_=launch('--command','run HELLO.QEX')
    assert 'Hello QRX' in out and 'unsupported' not in out
    out,_=launch('--command','run BENCH.QEX')
    assert 'Musashi guest app retired' in out
    commands=pathlib.Path(work)/'commands.txt'
    commands.write_text('help\nrun HELLO.QEX\nbench\nls\n',encoding='utf8')
    out,err=launch('--dual','--script',str(commands))
    assert 'Hello QRX' in out
    assert 'core 1 running diagnostic worker' in out
    assert 'counter=2000' in err,err
    out,_=launch('--command','cat README.TXT')
    assert 'Welcome to QRX OS One' in out
print('PASS: REAL Musashi 68040 firmware/QEX, cold restart, dual-core worker')
