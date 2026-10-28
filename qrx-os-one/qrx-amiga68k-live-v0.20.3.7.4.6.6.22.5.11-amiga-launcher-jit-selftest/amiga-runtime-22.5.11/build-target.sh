#!/usr/bin/env bash
# QRX OS One 22.5.11 - build the 22.5.11 native m68k components with the Deadwood
# AROS toolchain, convert ELF -> HUNK with Deadwood elf2hunk and gate HUNK magic.
#   build-target.sh <out-dir>
# Environment: QRX_AROS_CC (m68k-aros-gcc), QRX_ELF2HUNK, QRX_AROS_CFLAGS (extra
# -I/-L/-B flags; diagnostics-v6.6/deploy.sh passes its CCARGS here).
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: build-target.sh OUTDIR}"
CC="${QRX_AROS_CC:?QRX_AROS_CC (Deadwood m68k-aros-gcc) required}"
E2H="${QRX_ELF2HUNK:?QRX_ELF2HUNK (Deadwood elf2hunk) required}"
case "$CC" in *m68k-amigaos-gcc) echo "ERROR: legacy m68k-amigaos-gcc rejected" >&2; exit 2;; esac
read -r -a EXTRA <<< "${QRX_AROS_CFLAGS:-}"
CFLAGS=(-Os -m68020 -fno-pic -fno-PIC -Wall -Wextra -fstack-usage "${EXTRA[@]}")
mkdir -p "$OUT/elf" "$OUT/hunk" "$OUT/su"
build(){ local name="$1"; shift
  (cd "$OUT/su" && "$CC" "${CFLAGS[@]}" -I"$HERE/src" -o "$OUT/elf/$name.elf" "$@")
  python3 - "$OUT/elf/$name.elf" <<'PY'
import struct,sys
b=open(sys.argv[1],'rb').read(64)
assert b[:4]==b'\x7fELF', 'not ELF'
m=struct.unpack('>H',b[18:20])[0]
assert m==4, f'ELF machine {m} != 4 (m68k)'
print(f'ELF-PROOF {sys.argv[1].rsplit("/",1)[-1]} machine=4 (m68k)')
PY
  "$E2H" "$OUT/elf/$name.elf" "$OUT/hunk/$name"
  [[ "$(od -An -tx1 -N4 "$OUT/hunk/$name" | tr -d ' \n')" == 000003f3 ]] || { echo "HUNK gate failed: $name" >&2; exit 1; }
  echo "HUNK-PROOF $name size=$(stat -c %s "$OUT/hunk/$name") sha256=$(sha256sum "$OUT/hunk/$name" | awk '{print $1}')"
}
build QRXAmigaRun "$HERE/src/qrx_amigarun.c" "$HERE/src/qrx_hunk.c"
build qrx-jit-selftest "$HERE/src/qrx_jit_selftest.c"
build QRXHunkTest "$HERE/tests/amiga/qrx_hunktest.c"
build QRXHunkTestNeedLib -DQRX_NEEDLIB "$HERE/tests/amiga/qrx_hunktest.c"
cp -f "$OUT/hunk/QRXAmigaRun" "$OUT/hunk/amigarun"
cp -f "$OUT/hunk/QRXAmigaRun" "$OUT/hunk/amigacheck"
# Stack budget gate: no function of the 22.5.11 tools may need > 4 KiB of stack.
awk -F'\t' '{split($1,a,":"); if ($2+0 > 4096) {print "STACK-GATE FAIL " $0; bad=1} } END {exit bad}' "$OUT"/su/*.su
echo "STACK-GATE OK: max frame $(awk -F'\t' '{if($2+0>m)m=$2+0} END{print m}' "$OUT"/su/*.su) bytes"
# Negative fixtures for the target test (data files, deliberately NOT runnable).
mkdir -p "$OUT/fixtures"
s=$(stat -c %s "$OUT/hunk/QRXHunkTest"); head -c $((s / 2)) "$OUT/hunk/QRXHunkTest" > "$OUT/fixtures/QRXHunkTestTruncated"
python3 - "$OUT/fixtures" <<'PY'
import struct,sys
d=sys.argv[1]
e=bytearray(64); e[0:4]=b'\x7fELF'; e[4]=1; e[5]=2; e[6]=1; e[16:20]=struct.pack('>HH',2,20)
open(d+'/ppc-os4-elf.fixture','wb').write(bytes(e))
open(d+'/x86-pe.fixture','wb').write(b'MZ\x90\x00'+bytes(60))
PY
echo "FIXTURES: $(ls "$OUT/fixtures" | tr '\n' ' ')"
