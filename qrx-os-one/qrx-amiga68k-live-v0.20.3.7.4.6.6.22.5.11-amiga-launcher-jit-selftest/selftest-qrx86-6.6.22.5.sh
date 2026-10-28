#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d /tmp/qrx86-225.XXXXXX)"; trap 'rm -rf "$TMP"' EXIT
cc -std=c99 -O2 -Wall -Wextra -Werror -o "$TMP/qrx86" "$ROOT/diagnostics-v6.6/tools/qrx86_grt_exec.c"
python3 - "$TMP" <<'PY'
from pathlib import Path
import struct,sys
out=Path(sys.argv[1])
def wr(path, code, bridge=None):
    base=0x00400000; img=bytearray(0x1000); img[0x100:0x100+len(code)]=code
    bridges=[] if bridge is None else [bridge]
    with open(path,'wb') as f:
        f.write(b'QRXGRT1\0'); u=lambda x:f.write(struct.pack('<I',x))
        for x in [1,1,len(bridges),2,0,0x7ffde000,0x7ffdf000,0x7f010000]:u(x)
        for x in [base,len(img),0x100,0x2000,0]:u(x)
        n=b'test.dll';f.write(n+b'\0'*(64-len(n)))
        for addr,name,argc,flags in bridges:
            for x in [addr,0,argc,flags]:u(x)
            n=name.encode();f.write(n+b'\0'*(64-len(n)))
        for rec in [(1,0,0,base,1,0),(3,0,base+0x100,base,1,0)]:
            for x in rec:u(x)
        f.write(img)
wr(out/'ret.qrxgrt', bytes.fromhex('b8 01 00 00 00 c2 0c 00'))
wr(out/'fs.qrxgrt', bytes.fromhex('64 a1 18 00 00 00 c2 0c 00'))
bridge=0x7ff00000; ip=0x00400100; rel=(bridge-(ip+5))&0xffffffff
wr(out/'bridge.qrxgrt', b'\xe8'+struct.pack('<I',rel)+bytes.fromhex('c2 0c 00'), (bridge,'RtlGetProcessHeap',0,1))
PY
"$TMP/qrx86" --jit=off "$TMP/ret.qrxgrt" | grep -q 'QRX86 EXEC PASS'
"$TMP/qrx86" --jit=off "$TMP/fs.qrxgrt" | grep -q 'eax=7ffde000'
"$TMP/qrx86" --jit=off "$TMP/bridge.qrxgrt" | grep -q 'eax=7f010000'
echo 'QRX86 6.6.22.5 portable executor synthetic tests: PASS'
