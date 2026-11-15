#!/usr/bin/env bash
# Generate Musashi's opcode table for the 68k MacOS runtime.
#
# Musashi answers A-line words through the illegal-instruction callback, so the
# 1010 handler in m68k_in.c has to call m68ki_illg_callback() before it raises
# the CPU exception.  The table is generated into the directory given as the
# first argument; caller and this script share it (guest build and host test).
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
MUSASHI="$REPO/upstream/armithlon/2.6/engine/third_party/Musashi"
GEN="${1:-$REPO/.build/qrxppc/m68kgen}"

if [ ! -f "$MUSASHI/m68k_in.c" ]; then
    echo "Musashi sources missing: $MUSASHI" >&2
    exit 1
fi

mkdir -p "$GEN"
cc -O1 -o "$GEN/m68kmake" "$MUSASHI/m68kmake.c"
python3 - "$MUSASHI/m68k_in.c" "$GEN/m68k_in_qrx.c" <<'PYEOF'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
old = """M68KMAKE_OP(1010, 0, ., .)
{
	m68ki_exception_1010();
}"""
new = """M68KMAKE_OP(1010, 0, ., .)
{
	/* QRX: an A-line word is a Toolbox trap, not a fault. */
	if (m68ki_illg_callback(REG_IR))
		return;
	m68ki_exception_1010();
}"""
if old not in text:
    raise SystemExit("m68k_in.c: A-line handler not found")
open(dst, "w").write(text.replace(old, new, 1))
PYEOF
"$GEN/m68kmake" "$GEN" "$GEN/m68k_in_qrx.c"
