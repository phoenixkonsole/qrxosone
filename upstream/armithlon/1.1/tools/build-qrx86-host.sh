#!/usr/bin/env bash
# Native macOS/Linux QRX86 Win32 HLE, optional host-GPU backend over Unix socket.
# Source must be the verified phoenixkonsole/qrxosone branch checkout.
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${1:-$HERE/data/qrxosone}"
OUT="$HERE/build-qrx86-host"
CC="${CC:-clang}"; AR="${AR:-ar}"
command -v "$CC" >/dev/null || { echo 'Install clang (Xcode CLI tools)' >&2;exit 1; }
[ -d "$REPO/.git" ] || { echo "QRX repo missing: $REPO (./armithlon fetch)" >&2;exit 1; }
python3 "$HERE/tools/patch-qrx86-headless-gpu.py" "$REPO"
python3 "$HERE/tools/patch-qrx86-host-input.py" "$REPO"
python3 "$HERE/tools/patch-qrx86-opengl.py" "$REPO"
SRC="$(find "$REPO/qrx-os-one" -type d -name x86-runtime-22.5.12 -print -quit)"
[ -d "$SRC/win32" ] || { echo 'QRX86 runtime source missing' >&2;exit 1; }
mkdir -p "$OUT"
"$SRC/qmath/build-qmath.sh" "$CC" "$AR" "$OUT"
"$CC" -std=gnu99 -O2 -Wall -Wno-unused-parameter -Wno-sign-compare \
  -DQRX86_HOST_GPU -DQRX86_HOST_INPUT -DQRX86_HOST_GL -I"$HERE/engine/include" \
  -o "$OUT/qrx-win32" "$SRC/core/qrx86core.c" "$SRC"/win32/*.c \
  "$HERE/build-qrx68100/libqrx68100_core.a" "$OUT/libqmath.a" -lm -lpthread
printf 'Built ARMITHLON QRX86 Win32 host interpreter: %s\n' "$OUT/qrx-win32"
echo 'This is a native host interpreter, NOT a complete Win32 runtime or fast JIT.'
