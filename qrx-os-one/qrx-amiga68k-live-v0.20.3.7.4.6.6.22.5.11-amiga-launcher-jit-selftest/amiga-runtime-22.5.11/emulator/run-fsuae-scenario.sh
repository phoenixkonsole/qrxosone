#!/usr/bin/env bash
# QRX OS One - run one FS-UAE scenario on Deadwood AROS m68k.
#   run-fsuae-scenario.sh <aros-system-dir> <rom> <ext-rom> <scenario-dir> <out-dir> [cpu] [timeout-s]
# <scenario-dir>/overlay/   copied over the AROS system tree (programs, libs, data)
# <scenario-dir>/User-Startup   executed at the end of boot; must write QRXOUT:DONE
# The guest writes results/screens (QRXScreenGrab PPM) to QRXOUT: = <out-dir>.
# Deadwood AROS occasionally hangs in FS-UAE while the Workbench/RTG screen is
# opened (the display stays one flat grey colour).  The harness prepends
# "Echo >QRXOUT:BOOTED" to User-Startup and samples the X display every 10 s:
# when BOOTED is missing after QRX_BOOT_TMO s (default 240) or the display shows
# at most 3 colours while nothing is written to the output directory for
# QRX_HANG_TMO s (default 120), the emulator is restarted,
# at most QRX_BOOT_RETRIES times (default 2).  Each restart is logged.
# QRX_FSUAE_JIT=1 enables the FS-UAE 68k JIT (host-side emulator setting; the
# QRX86 x86 JIT inside QRX stays quarantined).  QRX_FSUAE_EXTRA adds raw lines.
# Speed (22.5.12): the 68040/060 MMU emulation is off by default (AROS does not use
# it; 1.5x faster CPU emulation); QRX_FSUAE_MMU=1 restores it.  FS-UAE 3.1.66's JIT
# crashes with the Deadwood AROS ROM on this host ("JIT: Can't handle access"), so
# QRX_FSUAE_JIT stays 0 by default for the stock binary. For the validated
# patched emulator/profile, source tools/qrx86/fsuae/enable.sh; see its README.
# QRX_FSUAE_BIN selects an explicit emulator executable.
# Warp mode is off by default since 22.5.12 (QRX_FSUAE_WARP=1 turns it on): with
# warp the emulated clock (vblank, timer.device) ran 54x ahead of the emulated CPU
# for qrx-win32, which breaks programs that pace work by time (PrBoom's screen
# melt), and the CPU itself was slower (BusyBox loop 63-72 s with warp, 31 s
# without; Doom 282 k -> 498 k x86 instructions/s).
set -Eeuo pipefail
qrx_tmp_base="${QRX_SCENARIO_TMPDIR:-/var/tmp}"
# bash reads scripts while running: run from a private copy so edits cannot break active runs
if [[ -z "${QRX_SCN_SELFCOPY:-}" ]]; then
  c="$(mktemp "$qrx_tmp_base/qrx-scn-harness.XXXXXX")"; cp "$0" "$c"
  QRX_SCN_SELFCOPY="$c" exec bash "$c" "$@"
fi
SYS="$1"; ROM="$2"; EXT="$3"; SCN="$4"; OUT="$5"; CPU="${6:-68060}"; TMO="${7:-900}"
W="$(mktemp -d "$qrx_tmp_base/qrx-scn.XXXXXX")"; trap 'rm -rf "$W" "$QRX_SCN_SELFCOPY"' EXIT
mkdir -p "$OUT" "$W/home"
cp -a "$SYS" "$W/sys"
[[ -d "$SCN/overlay" ]] && cp -a "$SCN/overlay/." "$W/sys/"
{ echo 'Echo "booted" >QRXOUT:BOOTED'; cat "$SCN/User-Startup"; } > "$W/sys/S/User-Startup"
case "$CPU" in 68060|68040) model=A4000/040;; 68030) model=A4000;; *) model=A1200/020;; esac
mb=16384; z3=1048576; [[ $CPU == 68020 ]] && { mb=0; z3=262144; }
cat > "$W/cfg.fs-uae" <<CFG
[fs-uae]
amiga_model = $model
cpu = $CPU
kickstart_file = $ROM
kickstart_ext_file = $EXT
chip_memory = 2048
motherboard_ram = $mb
zorro_iii_memory = $z3
graphics_card = uaegfx
graphics_memory = 262144
uae_gfxcard_size = 256
hard_drive_0 = $W/sys
hard_drive_0_label = AROS
hard_drive_0_priority = 6
hard_drive_1 = $OUT
hard_drive_1_label = QRXOUT
hard_drive_1_priority = -128
floppy_drive_count = 0
jit_compiler = ${QRX_FSUAE_JIT:-0}
sound_card = none
automatic_input_grab = 0
warp_mode = ${QRX_FSUAE_WARP:-0}
base_dir = $W/home
log_flush = 1
$( [[ "${QRX_FSUAE_MMU:-0}" == 1 ]] || echo "uae_mmu_model = 0" )
${QRX_FSUAE_EXTRA:-}
CFG
cp "$W/cfg.fs-uae" "$OUT/fs-uae.cfg"
BT="${QRX_BOOT_TMO:-240}"; tries=$(( ${QRX_BOOT_RETRIES:-2} + 1 )); attempt=0
while :; do
attempt=$((attempt+1)); rm -f "$OUT/BOOTED" "$OUT/DONE"
d=$(( 200 + RANDOM % 200 )); Xvfb ":$d" -screen 0 1280x1024x24 >"$OUT/xvfb.log" 2>&1 & xp=$!; sleep 2
if ! kill -0 "$xp" 2>/dev/null; then
  echo "Xvfb failed; see $OUT/xvfb.log" >&2
  echo DISPLAY_FAILED > "$OUT/STATUS"
  exit 1
fi
HOME="$W/home" DISPLAY=":$d" SDL_AUDIODRIVER=dummy "${QRX_FSUAE_BIN:-fs-uae}" "$W/cfg.fs-uae" > "$OUT/fs-uae.stdout" 2>&1 & fp=$!
t=0; hung=0; flat=0; HT="${QRX_HANG_TMO:-120}"
while (( t < TMO )); do
  [[ -f "$OUT/DONE" ]] && break; kill -0 $fp 2>/dev/null || break
  (( t >= BT )) && [[ ! -f "$OUT/BOOTED" ]] && { hung=1; break; }
  if (( t % 10 == 0 && t >= 30 )); then
    nc=$(DISPLAY=":$d" import -window root ppm:- 2>/dev/null | convert ppm:- -sample 25% ppm:- 2>/dev/null | identify -format %k ppm:- 2>/dev/null || echo 99)
    # a flat display alone is normal when no screen is open: also require that
    # nothing was written to the output directory meanwhile
    sig=$(ls -l --time-style=+%s "$OUT" 2>/dev/null | md5sum)
    if (( nc <= 3 )) && [[ "$sig" == "${lastsig:-}" ]]; then flat=$((flat+10)); else flat=0; fi
    lastsig=$sig
    (( flat >= HT )) && { hung=1; break; }
  fi
  sleep 2; t=$((t+2))
done
sleep 2; kill $fp 2>/dev/null || true; sleep 1; kill -9 $fp 2>/dev/null || true; kill $xp 2>/dev/null || true
(( hung && attempt < tries )) || break
echo "attempt $attempt: AROS hung at t=${t}s (BOOTED missing or flat display for ${flat}s), restarting the emulator" | tee -a "$OUT/BOOT-RETRIES.txt"
done
find "$W/home" -name 'fs-uae.log*' -exec cp {} "$OUT/" \; 2>/dev/null || true
for f in "$OUT"/*.ppm; do [[ -f "$f" ]] && convert "$f" "${f%.ppm}.png" && rm -f "$f"; done
rm -f "$OUT"/*.uaem
st=TIMEOUT; [[ -f "$OUT/DONE" ]] && st=COMPLETED; echo "$st" > "$OUT/STATUS"; echo "scenario $(basename "$SCN") on $CPU: $st after ${t}s"
