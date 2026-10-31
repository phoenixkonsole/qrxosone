#!/usr/bin/env bash
# Compare against the unchanged core at the audited base commit, not a second copy of new code.
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(git -C "$HERE" rev-parse --show-toplevel)"
WK="${1:?usage: run-jit-oracle-tests.sh NEW-WORKDIR}"
CC="${QRX_M68K_CC:-m68k-linux-gnu-gcc}"
AR="${QRX_M68K_AR:-m68k-linux-gnu-ar}"
OC="${QRX_M68K_OBJCOPY:-m68k-linux-gnu-objcopy}"
NM="${QRX_M68K_NM:-m68k-linux-gnu-nm}"
QEMU="${QRX_M68K_QEMU:-qemu-m68k}"
OPT="${QRX_JIT_TEST_OPT:--O2}"
case "$OPT" in -O2|-O3|-Os) ;; *) echo "unsupported test optimization" >&2;exit 2;; esac
read -r -a EXTRA <<< "${QRX_JIT_TEST_CFLAGS:-}"
BASE=25a6057a00db7201a323db80fc40e66cd459cba1
mkdir "$WK";WK="$(cd "$WK" && pwd)";mkdir "$WK/core"
REL="${HERE#"$REPO"/}/core/qrx86core.c"
git -C "$REPO" show "$BASE:$REL" > "$WK/core/qrx86core.c"
cp "$HERE/core/qrx86core.h" "$WK/core/"
ln -s "$HERE/qmath" "$WK/qmath"
printf '%s\n' "$BASE" > "$WK/REFERENCE-COMMIT"
bash "$HERE/qmath/build-qmath.sh" "$CC" "$AR" "$WK" -m68020 > "$WK/build.log" 2>&1
for cpu in 20 60; do
  "$CC" -O2 -m680"$cpu" -c "$WK/core/qrx86core.c" -o "$WK/reference-$cpu.o" >> "$WK/build.log" 2>&1
  "$NM" -g --defined-only "$WK/reference-$cpu.o" | awk '$3 ~ /^x86_/ { print $3, "oracle_" $3 }' > "$WK/symbols"
  "$OC" --redefine-syms="$WK/symbols" "$WK/reference-$cpu.o"
  extra=();[[ "$cpu" == 60 ]] && extra+=("$HERE/tests/m68k-linux-68060-support.c" -fno-builtin)
  "$CC" "$OPT" -std=gnu99 -m680"$cpu" -static -DQRX_JIT_POSIX -DQRX_JIT_ORACLE -DQRX_JIT_NATIVE_LOADS=1 "${EXTRA[@]}" \
    "$HERE/tests/jit-differential.c" "$HERE/tests/jit-abi.S" "${extra[@]}" "$WK/reference-$cpu.o" "$WK/libqmath.a" -lm \
    -o "$WK/tests-$cpu" >> "$WK/build.log" 2>&1
done
for cpu in 20 30 40 60; do
  binary="$WK/tests-20";[[ "$cpu" == 60 ]] && binary="$WK/tests-60"
  "$QEMU" -cpu m680"$cpu" "$binary" "$cpu" > "$WK/m680$cpu.log" 2>&1
  cat "$WK/m680$cpu.log"
done
echo "PASS unchanged-baseline oracle on all four m68k CPU profiles"
