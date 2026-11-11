#!/usr/bin/env bash
# Build the portable QRXPPC reference interpreter and its 68k guest test.
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="$REPO/.build/qrx-sdk/bin"
CC="$SDK/aros-gcc"
ELF2HUNK="$SDK/elf2hunk"
OUT="${1:-$REPO/.build/qrxppc}"
TREE="$REPO/.build/bundle/SystemFull"

if [ ! -x "$CC" ] || [ ! -x "$ELF2HUNK" ]; then
    echo "AROS SDK missing: $CC" >&2
    exit 1
fi

mkdir -p "$OUT"
cd "$REPO"

echo "  CC  QRXPPC.elf"
# -fno-optimize-sibling-calls: a tail call becomes a `bra.l` with an
# R_68K_PC32 relocation, and elf2hunk can only express such a relocation below
# 64 KiB of text.  The test binary is larger than that, so tail calls must stay
# ordinary `jsr` (R_68K_32) instead.
"$CC" -O2 -m68040 -fno-pic -fno-PIC -fno-optimize-sibling-calls \
    -Itools/qrxppc \
    tools/qrxppc/qrx_ppc.c tools/qrxppc/qrx_ppc_fpu.c \
    tools/qrxppc/qrx_ppc_jit.c tools/qrxppc/qrx_pef.c \
    tools/qrxppc/qrx_mix.c tools/qrxppc/qrx_ppc_test.c \
    -o "$OUT/QRXPPC.elf"

echo "  HUNK QRXPPC"
hunk_out="$("$ELF2HUNK" "$OUT/QRXPPC.elf" "$OUT/QRXPPC" 2>&1)"
printf '%s\n' "$hunk_out"
if printf '%s\n' "$hunk_out" | grep -q 'relocation(s) failed'; then
    echo "elf2hunk failed for QRXPPC" >&2
    exit 1
fi
if ! file "$OUT/QRXPPC" | grep -q 'AmigaOS loadseg()ble executable/binary'; then
    echo "QRXPPC is not a loadseg()ble Hunk" >&2
    exit 1
fi

if [ -d "$TREE" ]; then
    mkdir -p "$TREE/C"
    cp "$OUT/QRXPPC" "$TREE/C/QRXPPC"
    echo "  installed .build/bundle/SystemFull/C/QRXPPC"
fi

echo "Done: $OUT/QRXPPC"
