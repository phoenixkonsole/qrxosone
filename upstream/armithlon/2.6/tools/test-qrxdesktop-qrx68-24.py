#!/usr/bin/env python3
"""Patch regression from existing source fixture, not an Amiga guest compile."""
import importlib.util
import ast
from pathlib import Path
p=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('p24',p/'patch-qrxdesktop-qrx68-24.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
t=(p/'test-qrxdesktop-qrx68-23.py').read_text()
original=next(ast.literal_eval(node.value) for node in ast.parse(t).body if isinstance(node,ast.Assign) and any(isinstance(target,ast.Name) and target.id=="source" for target in node.targets))
a=m.patch(original)
assert m.TAG in a
assert a==m.patch(a)
assert 'qrx_ci_ends(full,".qrx68")' in a
assert 'C:QRX68Run' in a and 'QRX:C/QRX68Run' in a
assert 'QRXMODE' in a
assert 'qrx19_launch_guest(runner,full)' in a
assert 'if (qrx_is_pe32_i386(full))' in a
assert 'QRX:C/QRXWinRun' in a
print('PASS pinned desktop patch: explicit QRX68 routing; AROS native and QRX86 paths preserved (source regression)')
