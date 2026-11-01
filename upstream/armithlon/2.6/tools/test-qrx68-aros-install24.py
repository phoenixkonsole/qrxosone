#!/usr/bin/env python3
"""Test HUNK validation/idempotence and non-destructive installer policy."""
import importlib.util
from pathlib import Path
import tempfile
p=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('qrx68installer',p/'install-qrx68-aros.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as t:
    root=Path(t);(root/'S').mkdir();(root/'QRX'/'C').mkdir(parents=True)
    src=root/'launcher';src.write_bytes((p.parent/'data/qrx68/QRX68-Hello.hunk').read_bytes())
    m.install(src,root)
    assert (root/'C/QRX68Run').read_bytes()==src.read_bytes()
    assert (root/'QRX/C/QRX68Run').read_bytes()==src.read_bytes()
    m.install(src,root) # strict idempotence
    (root/'C/QRX68Run').write_bytes(b'CUSTOM EXISTING SYSTEM FILE')
    try: m.install(src,root)
    except ValueError: pass
    else: raise AssertionError('overwritten existing binary')
    binary=root/'fakeelf';binary.write_bytes(b'\x7fELF' + bytes(40))
    try: m.install(binary,root)
    except ValueError: pass
    else: raise AssertionError('installed non-HUNK')
print('PASS AROS guest installer validates binary and never overwrites existing programs')
