#!/usr/bin/env bash
# Source this file to select the patched emulator for QRX's scenario runner.
# usage: source tools/qrx86/fsuae/enable.sh /absolute/install/bin/fs-uae
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  echo 'Source this file so its environment applies to subsequent runs.' >&2
  exit 2
fi
qrx_fsuae_binary="${1:?absolute path to the patched fs-uae is required}"
if [[ $qrx_fsuae_binary != /* || ! -x $qrx_fsuae_binary ]]; then
  echo 'Expected an executable absolute FS-UAE path.' >&2
  return 2
fi
export QRX_FSUAE_BIN="$qrx_fsuae_binary" QRX_FSUAE_JIT=1 QRX_FSUAE_WARP=0
# Native-address translation must honor bank callbacks for the filesystem ROM.
# Byte/word/long data accesses retain their fast direct defaults.
# FPU instructions remain interpreted: upstream FPU JIT changes two pixels in
# the PrBoom control image; CPU-only JIT matches the interpreter exactly.
export QRX_FSUAE_EXTRA="${QRX_FSUAE_EXTRA:-}
uae_comp_trustnaddr = indirect
uae_compfpu = false
uae_cpu_speed = max
uae_cpu_cycle_exact = false
uae_blitter_cycle_exact = false
audio_driver = dummy"
unset qrx_fsuae_binary
