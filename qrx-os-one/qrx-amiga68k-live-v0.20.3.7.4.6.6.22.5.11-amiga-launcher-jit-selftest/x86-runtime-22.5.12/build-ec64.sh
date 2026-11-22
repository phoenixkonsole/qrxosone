#!/usr/bin/env bash
# QRX OS One 64 bit - build qrx-win32 for the 68100EC64 (64-bit AROS, m68k64 clang).
#   build-ec64.sh <out-dir>
# Environment: QRX_EC64_AROS = the AROS build's bin directory that holds native-m68k64 and
# darwin-aarch64 (see tools/armithlon/aros-qrx/README.md on the AROS 64 branch),
# QRX_EC64_CLANG = the m68k64 clang wrapper (tc/bin/clang of that build).
# Windows on the AROS screen through cybergraphics (with -s SCRIPT headless, as on the 32-bit system);
# without the x86 JIT, which emits 68040 code. Sound goes to the board (win32/sound_ec64.c).
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: build-ec64.sh OUTDIR}"
B="${QRX_EC64_AROS:?QRX_EC64_AROS (AROS build bin directory) required}"
CLANG="${QRX_EC64_CLANG:?QRX_EC64_CLANG (m68k64 clang wrapper) required}"
export PATH="$B/darwin-aarch64/tools:$PATH"
mkdir -p "$OUT"
cat > "$OUT/cc64" <<EOF
#!/bin/sh
exec "$CLANG" --sysroot "$B/native-m68k64/AROS/Development" "\$@"
EOF
chmod +x "$OUT/cc64"
bash "$HERE/qmath/build-qmath.sh" "$OUT/cc64" "$(dirname "$CLANG")/llvm-ar" "$OUT" -O2
"$OUT/cc64" -O2 -std=gnu99 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -o "$OUT/qrx-win32" "$HERE/core/qrx86core.c" \
    "$HERE"/win32/{hle,ne,pe,kernel,user,gdi,ansi,files,dx,extra,threads,compat,net,dplay,sound_ec64,backend_headless,backend_aros,qrx_win32_main}.c \
    "$OUT/libqmath.a" -lcybergraphics
echo "qrx-win32 for m68k64: $OUT/qrx-win32"
