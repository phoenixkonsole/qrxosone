#!/usr/bin/env python3
import importlib.util, tempfile, pathlib,struct,subprocess,sys
base=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("hunk",base/"qrx-hunk-audit.py");h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
with tempfile.TemporaryDirectory() as tmp:
 p=pathlib.Path(tmp)/"test.hunk";p.write_bytes(struct.pack(">7I",0x3f3,0,1,0,0,1,0))
 assert h.inspect(p)["hunks"]==1
 p.write_bytes(b"\x00\x00\x03\xf3")
 try:h.inspect(p);raise AssertionError("truncated accepted")
 except ValueError:pass
subprocess.run([sys.executable,str(base/"qrx-jit-preflight.py")],check=True)
print("PASS: HUNK header bounds and JIT encoding static tests")
