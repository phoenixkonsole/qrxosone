#!/usr/bin/env bash
# QRX OS One - run one FS-UAE scenario on Deadwood AROS m68k.
#   run-fsuae-scenario.sh <aros-system-dir> <rom> <ext-rom> <scenario-dir> <out-dir> [cpu] [timeout-s]
# <scenario-dir>/overlay/   copied over the AROS system tree (programs, libs, data)
# <scenario-dir>/User-Startup   executed at the end of boot; must write QRXOUT:DONE
# The guest writes results/screens (QRXScreenGrab PPM) to QRXOUT: = <out-dir>.
set -Eeuo pipefail
SYS="$1"; ROM="$2"; EXT="$3"; SCN="$4"; OUT="$5"; CPU="${6:-68060}"; TMO="${7:-900}"
W="$(mktemp -d /var/tmp/qrx-scn.XXXXXX)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$OUT" "$W/home"
cp -a "$SYS" "$W/sys"
[[ -d "$SCN/overlay" ]] && cp -a "$SCN/overlay/." "$W/sys/"
cp "$SCN/User-Startup" "$W/sys/S/User-Startup"
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
jit_compiler = 0
sound_card = none
automatic_input_grab = 0
warp_mode = 1
base_dir = $W/home
log_flush = 1
CFG
d=$(( 200 + RANDOM % 200 )); Xvfb ":$d" -screen 0 1280x1024x24 >/dev/null 2>&1 & xp=$!; sleep 2
HOME="$W/home" DISPLAY=":$d" SDL_AUDIODRIVER=dummy fs-uae "$W/cfg.fs-uae" > "$OUT/fs-uae.stdout" 2>&1 & fp=$!
t=0; while (( t < TMO )); do [[ -f "$OUT/DONE" ]] && break; kill -0 $fp 2>/dev/null || break; sleep 2; t=$((t+2)); done
sleep 2; kill $fp 2>/dev/null || true; sleep 1; kill -9 $fp 2>/dev/null || true; kill $xp 2>/dev/null || true
find "$W/home" -name 'fs-uae.log*' -exec cp {} "$OUT/" \; 2>/dev/null || true
for f in "$OUT"/*.ppm; do [[ -f "$f" ]] && convert "$f" "${f%.ppm}.png" && rm -f "$f"; done
rm -f "$OUT"/*.uaem
st=TIMEOUT; [[ -f "$OUT/DONE" ]] && st=COMPLETED; echo "$st" > "$OUT/STATUS"; echo "scenario $(basename "$SCN") on $CPU: $st after ${t}s"
