#!/usr/bin/env bash
# QRX OS One 22.5.11 - unattended FS-UAE target test on Deadwood AROS m68k.
#
#   run-fsuae-tests.sh <aros-system-dir> <rom.bin> <ext.bin> <qrx-hunk-dir> <report-dir> [cpu-list]
#
# Boots the Deadwood AROS amiga-m68k system tree (directory hard drive, AROS
# ROM + extended ROM built from the same commit) on an emulated, fully
# expanded Amiga 4000 (2 MB Chip, 16 MB motherboard Fast, 1 GB Zorro III
# Fast, uaegfx RTG with 256 MB VRAM), runs QRX:S/TestAmigaRun, the JIT
# self-test (static + EXECUTE) and a memory/RTG probe from a generated
# S:User-Startup and writes everything into a host directory mounted as
# QRXOUT:.  FS-UAE runs headless under Xvfb; the harness stops it when
# QRXOUT:DONE appears or the timeout expires.
set -Eeuo pipefail
SYS="${1:?aros system dir}"; ROM="${2:?rom}"; EXT="${3:?ext rom}"; QRXC="${4:?dir with QRX 22.5.11 HUNKs}"; REP="${5:?report dir}"
CPUS="${6:-68040 68060 68030 68020}"
TIMEOUT="${QRX_FSUAE_TIMEOUT:-900}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command -v fs-uae >/dev/null || { echo "fs-uae not installed"; exit 2; }
command -v Xvfb >/dev/null || { echo "Xvfb not installed"; exit 2; }
mkdir -p "$REP"
for cpu in $CPUS; do
  W="$REP/work-$cpu"; rm -rf "$W"; mkdir -p "$W/out" "$W/home"
  # private copy of the system tree so tests never mutate the build output
  cp -a "$SYS" "$W/sys"
  mkdir -p "$W/sys/QRX/C" "$W/sys/QRX/S" "$W/sys/QRX/Tests/Amiga"
  for n in QRXAmigaRun amigarun amigacheck qrx-jit-selftest; do cp "$QRXC/hunk/$n" "$W/sys/QRX/C/"; done
  for n in QRXHunkTest QRXHunkTestNeedLib; do cp "$QRXC/hunk/$n" "$W/sys/QRX/Tests/Amiga/"; done
  cp "$QRXC"/fixtures/* "$W/sys/QRX/Tests/Amiga/"
  cp "$HERE/../target/TestAmigaRun" "$W/sys/QRX/S/TestAmigaRun"
  mkdir -p "$W/sys/QRX/Tests/QRX86"
  python3 -B "$HERE/../../tests-22.5.11/qrxgrt_fixture.py" "$W/sys/QRX/Tests/QRX86" >/dev/null
  [[ -n "${QRX_EXTRA_C:-}" ]] && cp ${QRX_EXTRA_C} "$W/sys/QRX/C/" || true
  cp "$HERE/User-Startup.qrxtest" "$W/sys/S/User-Startup"
  case "$cpu" in
    68060) model=A4000/040; cpuopt="cpu = 68060";;
    68040) model=A4000/040; cpuopt="cpu = 68040";;
    68030) model=A4000; cpuopt="cpu = 68030";;
    68020) model=A1200/020; cpuopt="cpu = 68020";;
  esac
  zmem=1048576; mb=16384; [[ $cpu == 68020 ]] && { mb=0; zmem=262144; }   # A1200: no motherboard fast, 24-bit-safe Z3 window via uae
  cat > "$W/qrx.fs-uae" <<CFG
[fs-uae]
amiga_model = $model
$cpuopt
kickstart_file = $ROM
kickstart_ext_file = $EXT
chip_memory = 2048
motherboard_ram = $mb
zorro_iii_memory = $zmem
graphics_card = uaegfx
graphics_memory = 262144
hard_drive_0 = $W/sys
hard_drive_0_label = AROS
hard_drive_0_priority = 6
hard_drive_1 = $W/out
hard_drive_1_label = QRXOUT
hard_drive_1_priority = -128
floppy_drive_count = 0
jit_compiler = 0
accuracy = 1
sound_card = none
automatic_input_grab = 0
fullscreen = 0
warp_mode = 1
base_dir = $W/home
log_flush = 1
CFG
  echo "[$cpu] booting FS-UAE ($model, chip 2MB, mb-fast ${mb}KB, Z3 ${zmem}KB, uaegfx 256MB)"
  export HOME="$W/home"
  dnum=$(( 90 + RANDOM % 100 ))
  Xvfb ":$dnum" -screen 0 1280x1024x24 >/dev/null 2>&1 & xpid=$!
  echo ":$dnum" > "$W/display"; sleep 2
  DISPLAY=":$dnum" SDL_AUDIODRIVER=dummy fs-uae "$W/qrx.fs-uae" > "$W/fs-uae.stdout" 2>&1 &
  pid=$!
  t=0
  while (( t < TIMEOUT )); do
    [[ -f "$W/out/DONE" ]] && break
    if [[ -f "$W/out/SNAP-DESKTOP" && ! -f "$W/out/desktop-$cpu.png" ]]; then
      sleep 5; DISPLAY="$(cat "$W/display" 2>/dev/null)" import -window root "$W/out/desktop-$cpu.png" 2>/dev/null || true
    fi
    kill -0 $pid 2>/dev/null || break
    sleep 5; t=$((t+5))
  done
  sleep 3
  DISPLAY=":$dnum" import -window root "$W/out/final-$cpu.png" 2>/dev/null || true
  kill $pid 2>/dev/null || true; sleep 2; kill -9 $pid 2>/dev/null || true; kill $xpid 2>/dev/null || true
  status=TIMEOUT; [[ -f "$W/out/DONE" ]] && status=COMPLETED
  echo "[$cpu] $status after ${t}s"
  cp -a "$W/out" "$REP/results-$cpu"
  cp "$W/fs-uae.stdout" "$REP/results-$cpu/fs-uae.stdout" 2>/dev/null || true
  find "$W/home" -name 'fs-uae.log*' -exec cp {} "$REP/results-$cpu/" \; 2>/dev/null || true
  echo "$status" > "$REP/results-$cpu/STATUS"
  rm -rf "$W/sys"
done
python3 -B "$HERE/check-results.py" "$REP" $CPUS
