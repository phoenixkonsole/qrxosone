#!/usr/bin/env bash
# Build the portable QRXPPC reference interpreter and its 68k guest test.
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="$REPO/.build/qrx-sdk/bin"
CC="$SDK/aros-gcc"
ELF2HUNK="$SDK/elf2hunk"
OUT="${1:-$REPO/.build/qrxppc}"
# An isolated tree lets guest tests install tools without touching the bundle.
TREE="${QRXPPC_INSTALL_TREE:-$REPO/.build/bundle/SystemFull}"

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
    tools/qrxppc/qrx_macapp.c tools/qrxppc/qrx_toolbox.c \
    tools/qrxppc/qrx_mix.c tools/qrxppc/qrx_apprun.c \
    tools/qrxppc/qrx_appstub.c tools/qrxppc/qrx_ppc_test.c \
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


echo "  CC  QRXAppRun.elf"
"$CC" -O2 -m68040 -fno-pic -fno-PIC -fno-optimize-sibling-calls \
    -Itools/qrxppc \
    tools/qrxppc/qrx_ppc.c tools/qrxppc/qrx_ppc_fpu.c \
    tools/qrxppc/qrx_pef.c tools/qrxppc/qrx_macapp.c \
    tools/qrxppc/qrx_toolbox.c tools/qrxppc/qrx_mix.c \
    tools/qrxppc/qrx_apprun.c tools/qrxppc/qrx_appstub.c \
    tools/qrxppc/qrx-app.c \
    -o "$OUT/QRXAppRun.elf"

echo "  HUNK QRXAppRun"
hunk_out="$("$ELF2HUNK" "$OUT/QRXAppRun.elf" "$OUT/QRXAppRun" 2>&1)"
printf '%s\n' "$hunk_out"
if printf '%s\n' "$hunk_out" | grep -q 'relocation(s) failed'; then
    echo "elf2hunk failed for QRXAppRun" >&2
    exit 1
fi
if ! file "$OUT/QRXAppRun" | grep -q 'AmigaOS loadseg()ble executable/binary'; then
    echo "QRXAppRun is not a loadseg()ble Hunk" >&2
    exit 1
fi

# ---------------------------------------------------------------- PPC Mach-O prototype
echo "  CC  QRXMach.elf"
"$CC" -O2 -m68040 -fno-pic -fno-PIC -fno-optimize-sibling-calls \
    -Itools/qrxppc tools/qrxppc/qrx_macho.c tools/qrxppc/qrx-mach.c \
    tools/qrxppc/qrx_ppc.c tools/qrxppc/qrx_ppc_fpu.c \
    -o "$OUT/QRXMach.elf"

echo "  CC  QRXMachTest.elf"
"$CC" -O2 -m68040 -fno-pic -fno-PIC -fno-optimize-sibling-calls \
    -Itools/qrxppc tools/qrxppc/qrx_macho.c tools/qrxppc/test_macho.c \
    tools/qrxppc/qrx_ppc.c tools/qrxppc/qrx_ppc_fpu.c tools/qrxppc/qrx_ppc_jit.c \
    -o "$OUT/QRXMachTest.elf"

for binary in QRXMach QRXMachTest; do
    echo "  HUNK $binary"
    hunk_out="$("$ELF2HUNK" "$OUT/$binary.elf" "$OUT/$binary" 2>&1)"
    printf '%s\n' "$hunk_out"
    if printf '%s\n' "$hunk_out" | grep -q 'relocation(s) failed'; then
        echo "elf2hunk failed for $binary" >&2
        exit 1
    fi
    if ! file "$OUT/$binary" | grep -q 'AmigaOS loadseg()ble executable/binary'; then
        echo "$binary is not a loadseg()ble Hunk" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------- 68k Mac runtime
# QRX68KRun executes classic 68k MacOS applications on the Musashi 68020 core.
# Musashi's generated opcode table is built here: it needs the A-line handler to
# consult the illegal-instruction callback, which is where the Toolbox HLE hangs.
MUSASHI="$REPO/upstream/armithlon/2.6/engine/third_party/Musashi"
GEN="$OUT/m68kgen"
if [ ! -f "$MUSASHI/m68k_in.c" ]; then
    echo "Musashi sources missing: $MUSASHI" >&2
    exit 1
fi
echo "  GEN m68kops.c"
bash "$REPO/tools/qrxppc/m68k_gen.sh" "$GEN"

echo "  CC  QRX68KRun.elf"
"$CC" -O1 -m68040 -fno-pic -fno-PIC -fno-optimize-sibling-calls \
    -DM68K_ILLG_HAS_CALLBACK=1 \
    -Itools/qrxppc -I"$MUSASHI" -I"$GEN" \
    tools/qrxppc/qrx_m68k.c tools/qrxppc/qrx_macapp.c \
    tools/qrxppc/qrx_pef.c tools/qrxppc/qrx_ppc.c \
    tools/qrxppc/qrx_ppc_fpu.c "$MUSASHI/m68kcpu.c" \
    "$MUSASHI/softfloat/softfloat.c" "$GEN/m68kops.c" \
    tools/qrxppc/qrx-68k.c \
    -lm -o "$OUT/QRX68KRun.elf"

echo "  HUNK QRX68KRun"
hunk_out="$("$ELF2HUNK" "$OUT/QRX68KRun.elf" "$OUT/QRX68KRun" 2>&1)"
printf '%s\n' "$hunk_out"
if printf '%s\n' "$hunk_out" | grep -q 'relocation(s) failed'; then
    echo "elf2hunk failed for QRX68KRun" >&2
    exit 1
fi
if ! file "$OUT/QRX68KRun" | grep -q 'AmigaOS loadseg()ble executable/binary'; then
    echo "QRX68KRun is not a loadseg()ble Hunk" >&2
    exit 1
fi

if [ -d "$TREE" ]; then
    mkdir -p "$TREE/C"
    cp "$OUT/QRXPPC" "$TREE/C/QRXPPC"
    cp "$OUT/QRXAppRun" "$TREE/C/QRXAppRun"
    cp "$OUT/QRX68KRun" "$TREE/C/QRX68KRun"
    cp "$OUT/QRXMach" "$TREE/C/QRXMach"
    cp "$OUT/QRXMachTest" "$TREE/C/QRXMachTest"
    echo "  installed QRXPPC, QRXAppRun, QRX68KRun, QRXMach and QRXMachTest in $TREE/C"
fi

# ---------------------------------------------------------------- host test
# The memory layout follows the block that was actually allocated, so the
# fallback (8/4 MiB), the default sizing (16 MiB) and the ERR_MEM refusal are
# checked on the host as part of every build.
echo "  TEST m68k heap"
bash "$REPO/tools/qrxppc/test_m68k_heap.sh" "$OUT/hosttest"

echo "Done: $OUT/QRXPPC"
