#!/usr/bin/env bash
# QRX OS One 22.5.12 - build qrx-i386 (Linux i386 personality) and qrx-win32
# (Win32 HLE) for AROS m68k with the Deadwood toolchain, convert ELF -> HUNK
# with Deadwood elf2hunk and gate the HUNK magic.
#   build-target.sh <out-dir>
# Environment: QRX_AROS_CC, QRX_ELF2HUNK, QRX_AROS_CFLAGS (as for amiga-runtime-22.5.11).
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: build-target.sh OUTDIR}"
CC="${QRX_AROS_CC:?QRX_AROS_CC (Deadwood m68k-aros-gcc) required}"
E2H="${QRX_ELF2HUNK:?QRX_ELF2HUNK (Deadwood elf2hunk) required}"
case "$CC" in *m68k-amigaos-gcc) echo "ERROR: legacy m68k-amigaos-gcc rejected" >&2; exit 2;; esac
read -r -a EXTRA <<< "${QRX_AROS_CFLAGS:-}"
# -m68040 code also runs on the 68060; the FPU is required for the x87 emulation (doubles).
CFLAGS=(-O2 -m68040 -fno-pic -fno-PIC -std=gnu99 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation "${EXTRA[@]}")
mkdir -p "$OUT/elf" "$OUT/hunk"
build(){ local name="$1"; shift
  "$CC" "${CFLAGS[@]}" -o "$OUT/elf/$name.elf" "$@" -lm
  python3 - "$OUT/elf/$name.elf" <<'PY'
import struct,sys
b=open(sys.argv[1],'rb').read(64)
assert b[:4]==b'\x7fELF' and struct.unpack('>H',b[18:20])[0]==4, 'not an m68k ELF'
print(f'ELF-PROOF {sys.argv[1].rsplit("/",1)[-1]} machine=4 (m68k)')
PY
  "$E2H" "$OUT/elf/$name.elf" "$OUT/hunk/$name"
  [[ "$(od -An -tx1 -N4 "$OUT/hunk/$name" | tr -d ' \n')" == 000003f3 ]] || { echo "HUNK gate failed: $name" >&2; exit 1; }
  echo "HUNK-PROOF $name size=$(stat -c %s "$OUT/hunk/$name") sha256=$(sha256sum "$OUT/hunk/$name" | awk '{print $1}')"
}
build qrx-i386 "$HERE/core/qrx86core.c" "$HERE/linux/qrx86linux.c" "$HERE/linux/qrx_i386_main.c"
build qrx-win32 "$HERE/core/qrx86core.c" "$HERE"/win32/{hle,pe,kernel,user,gdi,backend_headless,backend_aros,qrx_win32_main}.c -lcybergraphics
