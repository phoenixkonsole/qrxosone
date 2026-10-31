#!/usr/bin/env bash
# Explicit experimental QRX86 x86->m68k loop backend. No FS-UAE configuration changes.
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$(cd "${1:?usage: build-native-loop-jit.sh TOOLCHAIN-BIN NEW-OUTDIR [CPU]}" && pwd)"
OUT="$(realpath -m -- "${2:?NEW-OUTDIR required}")"
CPU="${3:-60}"
case "$CPU" in 20|30|40|60) ;; *) echo 'CPU must be 20, 30, 40 or 60' >&2;exit 2;; esac
[[ ! -e "$OUT" ]] || { echo "Output already exists: $OUT" >&2;exit 2; }
mapfile -t FEATURES < "$REPO/tools/qrx86/native-loop-flags.txt"
FLAGS="-m680$CPU -Wno-volatile-register-var ${FEATURES[*]}"
ROOT="$REPO/qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest"
cd "$ROOT"
QRX_AROS_CC="$TOOLS/aros-gcc" QRX_ELF2HUNK="$TOOLS/elf2hunk" \
QRX_AROS_AR="${QRX_AROS_AR:-m68k-linux-gnu-ar}" QRX_AROS_CFLAGS="$FLAGS" \
    bash x86-runtime-22.5.12/build-target.sh "$OUT"
printf '%s\n' "$FLAGS" > "$OUT/QRX86_NATIVE_LOOP_FLAGS.txt"
cat > "$OUT/QRX86_NATIVE_LOOP_USAGE.txt" <<'USAGE'
Experimental QRX86 loop JIT. The ordinary source defaults remain unchanged.
Use the CPU-specific hunk/qrx-win32 built here. Before invoking it on AROS:
SetEnv QRX86_JIT experimental
SetEnv QRX86_JIT_LOOPS_ONLY 1
SetEnv QRX86_JIT_BACKEDGE_ONLY 1
SetEnv QRX86_JIT_THRESHOLD 64
Expected feature mask: 4194111; max_ops=64; code_cap=16384.
FS-UAE stays on the separately qualified fixed test profile.
Performance must be compared with matching game configuration and captured output.
USAGE
