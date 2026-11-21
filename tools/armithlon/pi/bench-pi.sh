#!/usr/bin/env bash
# Measurements of QRX ARMithlon on the Raspberry Pi itself (after setup-ubuntu-pi4.sh).
#   bench-pi.sh [--extract-only] [SECONDS]
# Takes both systems out of the newest image in dist/pi4 (no root needed: the FAT partition through
# mtools, the ext4 one through debugfs) into $QRX_TEST_DIR (default ~/qrx-test), then measures with
# the engine built in .build/armithlon-pi:
#   1. the JIT tests (68100, 68100EC64, QRX86 host JIT) with their times,
#   2. the 32-bit QRX OS One: SECONDS from the start with the screen in memory (QRX_LIO_TEST, no
#      display needed), instructions a second ([BENCH]), the boot watchdog's restarts,
#   3. QRX OS One 64 on 1 and 2 cores: instructions a second per core,
#   4. clock, temperature and throttling.
# Everything goes to $QRX_TEST_DIR/bench-<date>.txt as well.
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
T="${QRX_TEST_DIR:-$HOME/qrx-test}"
B="${QRX_BUILD:-$REPO/.build/armithlon-pi}"
EXTRACT_ONLY=0; [ "${1:-}" = --extract-only ] && { EXTRACT_ONLY=1; shift; }
SECS="${1:-60}"
WAIT="${QRX_BOOT_WAIT:-15}"   # seconds to the system disk before a restart, as boot_wait in qrx.txt
mkdir -p "$T"

# ---- both systems out of the image, once per image
IMG=$(ls -t "$REPO"/dist/pi4/*.img.xz | head -1)
STAMP="$T/.from-$(basename "$IMG")"
if [ ! -e "$STAMP" ]; then
  echo "taking the systems out of $(basename "$IMG")"
  raw="$T/image.img"
  xz -dc "$IMG" > "$raw"
  starts=$(sfdisk -d "$raw" | sed -n 's/.*start= *\([0-9]*\).*/\1/p')
  s1=$(echo "$starts" | sed -n 1p); s2=$(echo "$starts" | sed -n 2p)
  rm -rf "$T/boot" "$T/root"
  mkdir -p "$T/boot" "$T/root"
  mcopy -s -n -i "$raw@@$((s1 * 512))" ::/qrx64 ::/QRXOS64.img ::/qrx.txt "$T/boot/"
  dd if="$raw" of="$T/root.ext4" bs=512 skip="$s2" status=none
  debugfs -R "rdump /qrx $T/root" "$T/root.ext4" 2>/dev/null
  rm -f "$raw" "$T/root.ext4"
  touch "$STAMP"
fi
[ "$EXTRACT_ONLY" = 1 ] && { echo "systems in $T"; exit 0; }
[ -x "$B/qrx68100_full_run" ] || { echo "no engine in $B: run setup-ubuntu-pi4.sh first" >&2; exit 1; }

OUT="$T/bench-$(date +%Y%m%d-%H%M).txt"
exec > >(tee "$OUT") 2>&1
echo "QRX ARMithlon measurements, $(date), $(tr -d '\0' < /proc/device-tree/model 2>/dev/null)"
echo "kernel $(uname -r), engine $(git -C "$REPO" rev-parse --short HEAD), image $(basename "$IMG")"
clock() {
  local f t
  f=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
  t=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)
  echo "  clock $((f / 1000)) MHz, $((t / 1000)) C$(command -v vcgencmd >/dev/null && echo ", $(vcgencmd get_throttled 2>/dev/null)")"
}

echo; echo "1. JIT tests"; clock
ctest --test-dir "$B" -R "jit|ec64" 2>&1 | grep -E "Test +#|tests passed" || true

echo; echo "2. QRX OS One (32-bit), $SECS s, screen in memory"; clock
R="$T/root/qrx"; W="$T/run32"; rm -rf "$W"; mkdir -p "$W/apps" "$W/input"; cp -a "$R/System" "$W/sys"
cd "$R"   # as the session: the engine runs in /qrx
for try in 1 2 3 4 5 6; do
  rm -f "$W/sys/hostfs-boot-marker.txt"
  QRX_LIO_TEST="$W/input" QRX_ALSA_DEVICE=null QRX_NETCTL=/bin/true QRX_CPU_CORE=3 QRX_APPS_DIR="$W/apps" \
  QRX_FULL_ADF="$R/boot.adf" QRX_SCREEN_CAPTURE="$W/screen.ppm" QRX_SCREEN_CAPTURE_AT=$((SECS - 5)) \
    timeout $((SECS + 30)) taskset -c 0-2 "$B/qrx68100_full_run" --window --seconds "$SECS" \
    --rom "$R/aros-rom.bin" --ext-rom "$R/aros-ext.bin" --share "$W/sys" --share-rw > "$W/run.log" 2>&1 &
  pid=$!; n=0
  while [ ! -e "$W/sys/hostfs-boot-marker.txt" ] && kill -0 $pid 2>/dev/null && [ $n -lt $((WAIT * 5)) ]; do sleep 0.2; n=$((n + 1)); done
  if [ -e "$W/sys/hostfs-boot-marker.txt" ]; then echo "  system disk after $((n / 5)) s (start $try)"; wait $pid || true; break; fi
  echo "  start $try stalled before the system disk (the ROM's lost timer interrupt), again"
  kill $pid 2>/dev/null; wait $pid 2>/dev/null || true
done
grep -a "\[BENCH\]" "$W/run.log" | sed 's/^/  /' || true
[ -e "$W/screen.ppm" ] && echo "  the screen at $((SECS - 5)) s: $W/screen.ppm"

for cores in 1 2; do
  echo; echo "3. QRX OS One 64, $cores core(s), $SECS s"; clock
  K="$T/boot/qrx64"; W="$T/run64-$cores"; rm -rf "$W"; mkdir -p "$W"
  cp "$T/boot/QRXOS64.img" "$W/disk.img"
  set --
  while read -r m; do [ -n "$m" ] && set -- "$@" "$K/$m"; done < "$K/modules.txt"
  QRX_EC64_CPUS=3,2 taskset -c 0-1 "$B/qrx_ec64_run" --cores "$cores" --ram 1024 --disk "$W/disk.img" \
    --cmdline econsole-startup --seconds "$SECS" --stats --capture "$W/fb.ppm" --host-view 1024x768 \
    "$K/core.elf" "$@" > "$W/run.log" 2>&1 || true
  grep -a "\[EC64\] core\|all cores" "$W/run.log" | sed 's/^/  /' || true
  [ -e "$W/fb.ppm" ] && echo "  the screen at the end: $W/fb.ppm"
done
echo; clock
echo "written to $OUT"
