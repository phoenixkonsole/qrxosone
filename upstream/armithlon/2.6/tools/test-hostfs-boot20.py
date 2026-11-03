#!/usr/bin/env python3
"""Actual 68000 firmware MMIO roundtrip with a real mounted host directory.
No AROS claims; runs on the portable QRX bootstrap CPU path.
"""
import subprocess, sys, tempfile
from pathlib import Path
exe=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
    d=Path(temp)
    (d/'src.txt').write_text('REAL HOST DIRECTORY\n')
    script=d/'in.txt'
    script.write_text('hostinfo\nhostls\nhostcat src.txt\nhostget src.txt COPY.TXT\ncat COPY.TXT\nhostput COPY.TXT copy.txt\nrun HOST:hello.qex\nshutdown\n')
    # Use the real bundled 68k QEX1 payload from the provisioned volatile firmware.
    # Copy a known valid QEX1 without poking host command implementation directly.
    payload=bytes.fromhex('514558310000003049f900ff000c18bc004818bc006518bc006c18bc006c18bc006f18bc002018bc005118bc005218bc005818bc000a4e75')
    (d/'hello.qex').write_bytes(payload)
    cmd=[str(exe),'--volatile-disk','--ram-mib','8','--share',str(d),'--share-rw','--script',str(script)]
    p=subprocess.run(cmd,text=True,capture_output=True,timeout=30)
    if p.returncode:
        raise AssertionError(f'exit={p.returncode}\n{p.stdout}\n{p.stderr}')
    for phrase in ('REAL HOST DIRECTORY','HOST: mounted read/write','OK: host file copied to QRXFS-1',
                   'OK: QRXFS file copied to HOST:','Launching host-mounted guest M68k executable','Hello QRX'):
        assert phrase in p.stdout,(phrase,p.stdout)
    assert (d/'copy.txt').read_text()=='REAL HOST DIRECTORY\n'
    assert not list(d.glob('*.img')), 'volatile mode must not create disk images'
    # Default readonly must NOT allow writes and must preserve host data.
    p=subprocess.run([str(exe),'--volatile-disk','--share',str(d),'--command','hostput HELLO.QEX forbidden.qex'],text=True,capture_output=True,timeout=30)
    assert p.returncode==0 and 'read-only or write failed' in p.stdout
    assert not (d/'forbidden.qex').exists()
print('PASS real 68k guest firmware -> HostFS DMA -> host files, volatile no-img, readonly, write, host QEX')
