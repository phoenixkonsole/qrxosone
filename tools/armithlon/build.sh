#!/usr/bin/env bash
# Build the complete imported devkit and the QRX86 host bridge without patching the checkout.
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
VENDOR="$REPO/upstream/armithlon/2.6"
OUT="${1:-$REPO/.build/armithlon}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
CC="${CC:-cc}"; AR="${AR:-ar}"
cmake -S "$VENDOR/engine" -B "$OUT/engine" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$OUT/engine" --parallel "${QRX_BUILD_JOBS:-4}"
ctest --test-dir "$OUT/engine" --output-on-failure
COMPONENT=qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest
# A fresh disposable copy prevents stale patch results and preserves source changes.
ADAPTER=$(mktemp -d "$OUT/adapter.XXXXXX")
SRC="$ADAPTER/qrx-os-one/$COMPONENT/x86-runtime-22.5.12"
mkdir -p "$(dirname "$SRC")"
cp -R "$REPO/qrx-os-one/$COMPONENT/x86-runtime-22.5.12" "$SRC"
for patch in headless-gpu host-input opengl; do
  python3 "$VENDOR/tools/patch-qrx86-$patch.py" "$ADAPTER"
done
bash "$SRC/qmath/build-qmath.sh" "$CC" "$AR" "$OUT"
"$CC" -std=gnu99 -O2 -Wall -Wno-unused-parameter -Wno-sign-compare \
  -DQRX86_HOST_GPU -DQRX86_HOST_INPUT -DQRX86_HOST_GL -I"$VENDOR/engine/include" \
  -o "$OUT/qrx-win32" "$SRC/core/qrx86core.c" "$SRC"/win32/*.c \
  "$OUT/engine/libqrx68100_core.a" "$OUT/libqmath.a" -lm -lpthread
"$CC" -std=gnu99 -O2 -o "$OUT/qrx-i386" "$SRC/core/qrx86core.c" \
  "$SRC"/linux/*.c "$OUT/libqmath.a" -lm
printf 'Built QRX86 host bridges in %s\n' "$OUT"
