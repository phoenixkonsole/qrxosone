#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
ARCH=$(uname -m)
OS=$(uname -s)
if [ "$OS" != Darwin ] || [ "$ARCH" != arm64 ]; then
  echo "ERROR: test-m1.sh requires an Apple Silicon Mac (Darwin/arm64)" >&2
  exit 2
fi
cmake -S . -B build-m1 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-m1 --parallel
ctest --test-dir build-m1 --output-on-failure
./build-m1/qrx68100_vm --jit
printf '\nARM64 JIT unit test asserts successful native block execution.\n'
printf 'Further M1 benchmarks and AROS boot tests are NOT part of v0.2.\n'
