#!/usr/bin/env bash
# Explicit native execution in a throwaway FS-UAE machine. Never autostart on a user's Amiga.
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ROOT="$REPO/qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest"
TOOLS="${1:?usage: run-aros-jit-tests.sh TOOLCHAIN-BIN NEW-WORKDIR [CPU ...]}"
WK="${2:?NEW-WORKDIR required}";shift 2
[[ $# -gt 0 ]] || set -- 20 30 40 60
OPT="${QRX_JIT_TEST_OPT:--O2}"
case "$OPT" in -O2|-O3|-Os) ;; *) echo "unsupported test optimization" >&2;exit 2;; esac
if [[ "${QRX_JIT_FSUAE_FAST:-0}" == 1 ]]; then
  export QRX_FSUAE_EXTRA="${QRX_FSUAE_EXTRA:-}
uae_cpu_speed = max
uae_cpu_cycle_exact = false
uae_blitter_cycle_exact = false
warp_mode = 1"
fi
mkdir "$WK";WK="$(cd "$WK" && pwd)";mkdir "$WK/tmp" "$WK/scenario" "$WK/scenario/overlay" "$WK/scenario/overlay/C"
TOOLS="$(cd "$TOOLS" && pwd)"
CC="$TOOLS/aros-gcc";E2H="$TOOLS/elf2hunk"
AR="${QRX_AROS_AR:-m68k-linux-gnu-ar}"
SDK="$REPO/upstream/aros-deadwood/m68k-build-5b5fd4c"
# Verify all retained ROM, SDK and ISO inputs before booting.
(cd "$SDK" && sha256sum -c SHA256SUMS) > "$WK/input-checksums.log"
SYS="${QRX_AROS_SYSTEM:-}"
if [[ -z "$SYS" ]]; then
  xz -dc "$SDK/aros-amiga-m68k-5b5fd4c.iso.xz" > "$WK/aros.iso"
  mkdir "$WK/system"
  "${QRX_7Z:-7z}" x -y "$WK/aros.iso" "-o$WK/system" > "$WK/extract.log"
  SYS="$WK/system"
fi
bash "$ROOT/x86-runtime-22.5.12/qmath/build-qmath.sh" "$CC" "$AR" "$WK/qmath" -m68020 -fno-pic -fno-PIC -fno-optimize-sibling-calls > "$WK/build.log" 2>&1
for cpu in "$@"; do
  case "$cpu" in 20|30|40|60) ;; *) echo "Unsupported CPU $cpu" >&2;exit 2;; esac
  "$CC" -m680"$cpu" "$OPT" -std=gnu99 -fno-pic -fno-PIC -fno-optimize-sibling-calls -Wno-volatile-register-var \
    -I"$ROOT/x86-runtime-22.5.12/core" -o "$WK/probe-$cpu.elf" "$REPO/tools/qrx86/sdk-abi-probe.c" >> "$WK/build.log" 2>&1
  "$E2H" "$WK/probe-$cpu.elf" "$WK/scenario/overlay/C/sdk-abi-probe"
  "$CC" -m680"$cpu" "$OPT" -std=gnu99 -fno-pic -fno-PIC -fno-optimize-sibling-calls -Wno-volatile-register-var \
    -o "$WK/jit-tests-$cpu.elf" "$ROOT/x86-runtime-22.5.12/tests/jit-differential.c" \
    "$ROOT/x86-runtime-22.5.12/tests/jit-abi.S" "$WK/qmath/libqmath.a" -lm >> "$WK/build.log" 2>&1
  "$E2H" "$WK/jit-tests-$cpu.elf" "$WK/scenario/overlay/C/qrx-jit-tests"
  cat > "$WK/scenario/User-Startup" <<SCN
FailAt 1000
C:sdk-abi-probe >QRXOUT:abi.log
Echo "abi rc=\$RC" >QRXOUT:abi.rc
C:qrx-jit-tests $cpu >QRXOUT:unit.log
Echo "unit rc=\$RC" >QRXOUT:unit.rc
Echo "done" >QRXOUT:DONE
SCN
  QRX_SCENARIO_TMPDIR="$WK/tmp" QRX_BOOT_RETRIES=0 bash "$ROOT/amiga-runtime-22.5.11/emulator/run-fsuae-scenario.sh" \
    "$SYS" "$SDK/aros-amiga-m68k-rom.bin" "$SDK/aros-amiga-m68k-ext.bin" \
    "$WK/scenario" "$WK/m680$cpu" "680$cpu" "${QRX_JIT_TEST_TIMEOUT:-900}"
  [[ "$(cat "$WK/m680$cpu/STATUS")" == COMPLETED ]]
  grep -q 'QRX SDK ABI PASS' "$WK/m680$cpu/abi.log"
  grep -q 'PASS native m68k JIT differential/safety suite' "$WK/m680$cpu/unit.log"
  grep -q 'unit rc=0' "$WK/m680$cpu/unit.rc"
  echo "PASS AROS/FS-UAE 680$cpu (emulator only)"
done
