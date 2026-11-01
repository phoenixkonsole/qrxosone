#!/usr/bin/env python3
"""End-to-end host QRX68/1 queue tests with a REAL portable 68k-HUNK runner.
NOT an AROS guest execution test, nor full Musashi/AmigaOS compatibility proof.
"""
import importlib.util
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / 'integration/qrx68-bridge/qrx68_service.py'
spec = importlib.util.spec_from_file_location('qrx68bridge', MOD)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def stage(spool, ident, payload):
    (spool/'inbox'/f'{ident}.hunk').write_bytes(payload)
    (spool/'inbox'/f'{ident}.req').write_bytes(m.MARKER)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('test-qrx68-bridge24.py /path/to/real/qrx68-run')
    runner = Path(sys.argv[1]).resolve()
    hello = (ROOT/'data/qrx68/QRX68-Hello.hunk').read_bytes()
    gui = (ROOT/'data/qrx68/QRX68-GUI.hunk').read_bytes()
    with tempfile.TemporaryDirectory(prefix='qrx68-bridge-test-') as t:
        root = Path(t)
        m.setup_spool(root)
        stage(root,'HELLODEMO001',hello)
        assert m.process_request(root, 'HELLODEMO001', runner) == 'ok'
        assert (root/'results/HELLODEMO001.status').read_bytes().startswith(b'OK\n')
        assert b'Hello QRX68' in (root/'results/HELLODEMO001.out').read_bytes()
        assert m.process_request(root,'HELLODEMO001',runner)=='already-completed'
        stage(root,'GRAPHICS001',gui)
        assert m.process_request(root, 'GRAPHICS001', runner) == 'ok'
        ppm = (root/'results/GRAPHICS001.ppm').read_bytes()
        assert ppm.startswith(b'P6\n640 256\n255\n'), len(ppm)
        assert len(ppm) == len(b'P6\n640 256\n255\n')+640*256*3
        stage(root,'BADHUNK0001',b'not an Amiga program')
        assert m.process_request(root, 'BADHUNK0001', runner) == 'error'
        assert (root/'results/BADHUNK0001.status').read_bytes().startswith(b'ERR\n')
        # The service refuses a symlink pointing outside the shared root.
        outside = root/'external-guest-file.hunk'
        outside.write_bytes(hello)
        os.symlink(outside,root/'inbox/SYMLINK00001.hunk')
        (root/'inbox/SYMLINK00001.req').write_bytes(m.MARKER)
        assert m.process_request(root,'SYMLINK00001',runner)=='error'
        # Invalid id must never be interpolated into a file path.
        try:
            m.process_request(root, '../TRAVERSAL',runner)
        except ValueError:
            pass
        else:
            raise AssertionError('unvalidated ID accepted')
        # Guest publishes .req LAST, service --once batch processing.
        stage(root,'SERVICE000001',hello)
        subprocess.run([sys.executable,str(MOD),'--bridge',str(root),'--runner',str(runner),'--once'],check=True)
        assert (root/'results/SERVICE000001.status').read_text().startswith('OK\n')
        # Executable was really run: check host print, not just file signatures.
        assert b'Hello QRX68' in (root/'results/SERVICE000001.out').read_bytes()
        assert not (root/'inbox/SERVICE000001.req').exists()
        print('PASS QRX68/1 real HUNK Exec/DOS, GUI snapshot, one-shot service, fail-closed format, no-follow symlink, fixed paths')

if __name__=='__main__':main()
