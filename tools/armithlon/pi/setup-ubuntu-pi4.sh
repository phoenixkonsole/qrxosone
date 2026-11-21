#!/usr/bin/env bash
# QRX ARMithlon development and test setup for a Raspberry Pi 4 with Ubuntu Server 24.04 (64-bit).
# Installs what the engine needs to build and run on the Pi itself, builds it natively with all
# tests, prepares the measurements (tools/armithlon/pi/bench-pi.sh) and installs Claude Code, so a
# Claude session can work on the Pi through Remote Control.
#
#   bash setup-ubuntu-pi4.sh [CHECKOUT]     (CHECKOUT: the repository, default ~/qrxosone)
#
# Run it as your normal user; it asks for sudo where it installs packages. It changes no boot
# settings; the performance settings it suggests at the end are for you to decide.
set -Eeuo pipefail
REPO="${1:-$HOME/qrxosone}"
BRANCH=claude/qrx-22.5.11

say() { printf '\n== %s\n' "$*"; }
[ "$(uname -m)" = aarch64 ] || { echo "This is for a 64-bit Pi (aarch64); this machine is $(uname -m)." >&2; exit 1; }
. /etc/os-release
[ "${VERSION_ID:-}" = 24.04 ] || echo "Warning: made for Ubuntu 24.04, this is ${PRETTY_NAME:-unknown}."
grep -q "Raspberry Pi 4" /proc/device-tree/model 2>/dev/null || echo "Warning: not a Raspberry Pi 4 ($(tr -d '\0' < /proc/device-tree/model 2>/dev/null))."

say "Packages"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  git ca-certificates curl xz-utils build-essential cmake ninja-build python3 python3-pil python3-numpy \
  libasound2-dev libegl-dev libopengl-dev libgl-dev libgles-dev libssl-dev linux-libc-dev \
  mesa-utils mesa-utils-bin alsa-utils evtest fonts-inter \
  dosfstools mtools e2fsprogs fdisk \
  linux-tools-raspi linux-tools-common htop
# the engine on the console: the screen (video, render), keyboard and mouse (input), sound (audio)
sudo usermod -aG video,render,input,audio "$USER"

say "Repository: $REPO ($BRANCH)"
if [ ! -d "$REPO/.git" ]; then
  # a private repository asks for your GitHub user and a token here
  git clone --branch "$BRANCH" https://github.com/phoenixkonsole/qrxosone.git "$REPO"
else
  git -C "$REPO" fetch origin "$BRANCH" && git -C "$REPO" checkout "$BRANCH" && git -C "$REPO" pull --ff-only
fi

say "Engine: native build for the Cortex-A72 with all tests"
cd "$REPO"
bash upstream/armithlon/2.6/tools/fetch-musashi.sh
cmake -S upstream/armithlon/2.6/engine -B .build/armithlon-pi -G Ninja -DQRX_ENABLE_MUSASHI=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-mcpu=cortex-a72" -DQRX_LINUX_FRONTEND=kms
cmake --build .build/armithlon-pi
ctest --test-dir .build/armithlon-pi --output-on-failure -j4 | tail -5

say "Both QRX systems from the newest image in dist/pi4 (for bench-pi.sh)"
bash tools/armithlon/pi/bench-pi.sh --extract-only

say "Claude Code"
if ! command -v claude >/dev/null 2>&1; then
  curl -fsSL https://claude.ai/install.sh | bash
fi

cat <<EOF

Done. Log out and in once (the new groups).

For a Claude session on this Pi:
  cd $REPO && claude remote-control
The session then shows up in the Claude Code app; there Claude can build, test and measure here.

Measurements (no display needed for most):
  bash $REPO/tools/armithlon/pi/bench-pi.sh

Optional, for steadier numbers (your decision, it changes the boot settings):
  - all cores at the highest clock:   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
  - core 3 kept free for the emulation: add "isolcpus=3 nohz_full=3 rcu_nocbs=3" to /boot/firmware/cmdline.txt, reboot
  - cooling: the Pi 4 throttles at 80 C without a heat sink or fan (vcgencmd measure_temp)
EOF
