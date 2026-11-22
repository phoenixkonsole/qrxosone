#!/usr/bin/env bash
# Build ARMithlon on a Raspberry Pi (64-bit Raspberry Pi OS) or cross-build it on x86-64 Linux.
#   build-pi.sh            on the Pi: native build + ctest
#   build-pi.sh --cross    on x86-64 Ubuntu/Debian: AArch64 cross build, tests under qemu
# Packages on the Pi: cmake gcc python3 git libsdl2-dev libegl-dev libgl-dev libssl-dev (3.5: Trixie)
set -Eeuo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
ENGINE="$REPO/upstream/armithlon/2.6/engine"
OUT="${QRX_PI_BUILD:-$REPO/.build/armithlon-pi}"
CROSS=0; [[ "${1:-}" == --cross ]] && CROSS=1
bash "$REPO/upstream/armithlon/2.6/tools/fetch-musashi.sh"
args=(-S "$ENGINE" -B "$OUT" -DQRX_ENABLE_MUSASHI=ON -DCMAKE_BUILD_TYPE=Release)
[[ -f /usr/include/vulkan/vulkan.h ]] && args+=(-DQRX_VULKAN_INCLUDE=/usr/include)
if (( CROSS )); then
  args+=(-DCMAKE_TOOLCHAIN_FILE="$REPO/tools/armithlon/pi/toolchain-aarch64-linux.cmake" -DQRX_PI_CPU="${QRX_PI_CPU:-cortex-a72}")
fi
cmake "${args[@]}"
cmake --build "$OUT" --parallel "${QRX_BUILD_JOBS:-4}"
if (( CROSS )); then
  ctest --test-dir "$OUT" -j4 || true          # Python tests need the qemu wrappers:
  bash "$REPO/tools/armithlon/pi/ctest-qemu-python.sh" "$OUT" "${QRX_PI_CPU:-cortex-a72}"
else
  ctest --test-dir "$OUT" --output-on-failure -j4
fi
printf 'ARMithlon for the Raspberry Pi built in %s\n' "$OUT"
