#!/usr/bin/env bash
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WK="${1:?usage: run-jit-tests.sh WORKDIR}"
CC="${QRX_M68K_CC:-m68k-linux-gnu-gcc}"
AR="${QRX_M68K_AR:-m68k-linux-gnu-ar}"
AS="${QRX_M68K_AS:-m68k-linux-gnu-as}"
OC="${QRX_M68K_OBJCOPY:-m68k-linux-gnu-objcopy}"
QEMU="${QRX_M68K_QEMU:-qemu-m68k}"
OPT="${QRX_JIT_TEST_OPT:--O2}"
case "$OPT" in -O2|-O3|-Os) ;; *) echo "unsupported test optimization" >&2;exit 2;; esac
mkdir -p "$WK"
WK="$(cd "$WK" && pwd)"
bash "$HERE/qmath/build-qmath.sh" "$CC" "$AR" "$WK" -m68020 -Wno-error=implicit-int -Wno-error=declaration-missing-parameter-type > "$WK/build.log" 2>&1
"$CC" -std=gnu99 "$OPT" -m68020 -static -DQRX_JIT_POSIX -Wall -Wno-unused-function -Wno-clobbered -Wno-sign-compare -Wno-misleading-indentation \
  "$HERE/tests/jit-differential.c" "$HERE/tests/jit-abi.S" "$WK/libqmath.a" -lm -o "$WK/jit-tests" >> "$WK/build.log" 2>&1
cat > "$WK/reference.s" <<'ASM'
.text
movea.l 4(%sp),%a0
move.l 8(%a0),%d0
move.l 12(%a0),%d1
add.l %d1,%d0
sub.l %d1,%d0
and.l %d1,%d0
or.l %d1,%d0
eor.l %d1,%d0
move.l %d0,16(%a0)
move.l #0x12345678,20(%a0)
rts
ASM
"$AS" -m68020 -o "$WK/reference.o" "$WK/reference.s"
"$OC" -O binary -j .text "$WK/reference.o" "$WK/reference.bin"
"$QEMU" -cpu m68020 "$WK/jit-tests" --emit > "$WK/emitted.bin"
cmp "$WK/reference.bin" "$WK/emitted.bin"
echo 'PASS backend emission matches independent GNU assembler'
# Linux's packaged libc/libgcc contain 68020 MUL/DIV not supported by 68060.
# Use equivalent portable test-runtime routines, not an instruction emulator.
"$CC" -std=gnu99 "$OPT" -m68060 -static -DQRX_JIT_POSIX -fno-builtin \
  "$HERE/tests/jit-differential.c" "$HERE/tests/jit-abi.S" "$HERE/tests/m68k-linux-68060-support.c" \
  "$WK/libqmath.a" -lm -o "$WK/jit-tests60" >> "$WK/build.log" 2>&1
for cpu in 20 30 40 60; do
  binary="$WK/jit-tests"; [[ "$cpu" == 60 ]] && binary="$WK/jit-tests60"
  "$QEMU" -cpu "m680$cpu" "$binary" "$cpu" > "$WK/m680$cpu.log" 2>&1
  cat "$WK/m680$cpu.log"
done
