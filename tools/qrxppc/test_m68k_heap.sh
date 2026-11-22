#!/usr/bin/env bash
# Host test for the 68k MacOS runtime's memory layout.
#
# The runtime falls back to 8 and 4 MiB when the guest cannot give it 16 MiB.
# MemTop, BufPtr, the zone base and the NewPtr limit have to follow the block
# that was actually allocated, so this test loads the same program with an
# 8 MiB and a 4 MiB budget and checks every address it reports.  It needs no
# AROS SDK: the runtime and Musashi are plain C and build with the host cc.
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
MUSASHI="$REPO/upstream/armithlon/2.6/engine/third_party/Musashi"
OUT="${1:-$REPO/.build/qrxppc/hosttest}"
GEN="$OUT/m68kgen"

mkdir -p "$OUT"
bash "$REPO/tools/qrxppc/m68k_gen.sh" "$GEN"

echo "  CC  test_m68k_heap"
# m68kfpu.c is not linked on purpose: m68kcpu.c already carries the FPU glue,
# and linking both causes duplicate symbols.
cc -O2 -DM68K_ILLG_HAS_CALLBACK=1 \
   -I"$REPO/tools/qrxppc" -I"$MUSASHI" -I"$GEN" \
   "$REPO/tools/qrxppc/test_m68k_heap.c" "$REPO/tools/qrxppc/qrx_m68k.c" \
   "$REPO/tools/qrxppc/qrx_macapp.c" "$REPO/tools/qrxppc/qrx_pef.c" \
   "$REPO/tools/qrxppc/qrx_ppc.c" "$REPO/tools/qrxppc/qrx_ppc_fpu.c" \
   "$MUSASHI/m68kcpu.c" "$MUSASHI/softfloat/softfloat.c" \
   "$GEN/m68kops.c" -lm \
   -o "$OUT/test_m68k_heap"

"$OUT/test_m68k_heap"
