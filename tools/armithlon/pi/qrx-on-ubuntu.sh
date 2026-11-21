#!/usr/bin/env bash
# QRX OS One on the Pi's screen from Ubuntu Server 24.04 (after setup-ubuntu-pi4.sh): the engine
# built in .build/armithlon-pi takes the display (DRM/KMS), keyboard, mouse (evdev) and sound
# (ALSA) directly, as on the QRX image, with no desktop in between.
#   qrx-on-ubuntu.sh [32|64] [CORES] [RAM-MB]     32 = QRX OS One (default), 64 = QRX OS One 64
# Run it on the Pi's own console (logged in on tty1..6), or from SSH / a Claude session: then it
# moves to a free console with openvt (asks for sudo) and switches the screen there; the engine
# still runs as you. Ctrl+Alt+F1..F6 switch consoles; shutting QRX OS One down ends it, from SSH
# also: pkill -f qrx68100_full_run (or qrx_ec64_run).
# Your QRX data stays in $QRX_DATA (default ~/qrx-data): the 32-bit system volume (System/), Apps/,
# the QRX OS One 64 disk (QRXOS64.img), and the logs. The first start copies them from the systems
# taken out of the image (bench-pi.sh --extract-only). QRX_PERF_MAP=1 is passed on (perf).
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
B="${QRX_BUILD:-$REPO/.build/armithlon-pi}"
T="${QRX_TEST_DIR:-$HOME/qrx-test}"
D="${QRX_DATA:-$HOME/qrx-data}"
SYS="${1:-32}" CORES="${2:-2}" RAM="${3:-1024}"
[ "$SYS" = 32 ] || [ "$SYS" = 64 ] || { echo "qrx-on-ubuntu.sh [32|64] [CORES] [RAM-MB]" >&2; exit 1; }
[ -x "$B/qrx68100_full_run" ] || { echo "no engine in $B: run setup-ubuntu-pi4.sh first" >&2; exit 1; }
[ -d "$T/root/qrx" ] || bash "$REPO/tools/armithlon/pi/bench-pi.sh" --extract-only

# not on a console of this machine: onto a free one, as this user (QRX_LIO_TEST: no screen, a test)
if [ -z "${QRX_LIO_TEST:-}" ] && ! tty 2>/dev/null | grep -q '^/dev/tty[0-9]'; then
  [ -z "${QRX_ON_VT:-}" ] || { echo "openvt gave no console" >&2; exit 1; }
  echo "starting QRX OS One on a free console of the Pi (openvt)"
  exec sudo openvt -s -w -- sudo -u "$USER" env HOME="$HOME" QRX_BUILD="$B" QRX_TEST_DIR="$T" \
    QRX_DATA="$D" QRX_PERF_MAP="${QRX_PERF_MAP:-}" QRX_ON_VT=1 bash "$0" "$@"
fi
for g in video render input audio; do
  id -nG | tr ' ' '\n' | grep -qx $g || echo "warning: you are not in group $g (setup-ubuntu-pi4.sh adds it; log in again)"
done

R="$T/root/qrx" K="$T/boot/qrx64"
mkdir -p "$D/Apps"
[ -d "$D/System" ] || cp -a "$R/System" "$D/System"
# QRX's network and system settings drive the QRX image's own scripts; on Ubuntu they stay off
export QRX_NETCTL=/bin/false

if [ "$SYS" = 64 ]; then
  [ -f "$D/QRXOS64.img" ] || cp "$T/boot/QRXOS64.img" "$D/QRXOS64.img"
  mods=()
  while read -r m; do [ -n "$m" ] && mods+=("$K/$m"); done < "$K/modules.txt"
  # the guest cores on the last host cores, the window and devices on the first ones
  cpus=$(seq 3 -1 $((4 - CORES)) | paste -sd,)
  cd "$R"
  echo "QRX OS One 64: $CORES cores on host cores $cpus, $RAM MiB, log $D/qrx64.log"
  QRX_EC64_CPUS="$cpus" taskset -c 0-$((3 - CORES < 0 ? 0 : 3 - CORES)) "$B/qrx_ec64_run" --window \
    --cores "$CORES" --ram "$RAM" --disk "$D/QRXOS64.img" --disk-write --cmdline econsole-startup \
    "$K/core.elf" "${mods[@]}" > "$D/qrx64.log" 2>&1
  exit $?
fi

# the 32-bit system, with the session's boot watchdog: the ROM sometimes loses a timer interrupt
# before the first floppy read (docs/armithlon/raspberry-pi-image.md); without the system volume
# after QRX_BOOT_WAIT seconds (default 15) it starts again, up to 6 times
cd "$R"
wait_s="${QRX_BOOT_WAIT:-15}"
marker="$D/System/hostfs-boot-marker.txt"
for try in 1 2 3 4 5 6; do
  rm -f "$marker"
  QRX_APPS_DIR="$D/Apps" QRX_FULL_ADF="$R/boot.adf" QRX_CPU_CORE=3 \
    taskset -c 0-2 "$B/qrx68100_full_run" --window --rom "$R/aros-rom.bin" --ext-rom "$R/aros-ext.bin" \
    --share "$D/System" --share-rw > "$D/qrx32.log" 2>&1 &
  pid=$!; n=0
  while [ ! -e "$marker" ] && kill -0 $pid 2>/dev/null && [ $n -lt $((wait_s * 5)) ]; do sleep 0.2; n=$((n + 1)); done
  if [ -e "$marker" ] || ! kill -0 $pid 2>/dev/null || [ $try = 6 ]; then break; fi
  echo "start $try: no system volume after $wait_s s, starting again" >> "$D/qrx32-watchdog.log"
  kill $pid; wait $pid 2>/dev/null || true
done
wait $pid
