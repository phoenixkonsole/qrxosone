#!/usr/bin/env bash
# Explicit PE32 build. Needs a MinGW cross compiler; outputs outside tracked source.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${MINGW_CC:-i686-w64-mingw32-gcc}"
command -v "$CC" >/dev/null 2>&1 || {
    echo "Missing $CC. On macOS: brew install mingw-w64 (then retry)." >&2; exit 2;
}
mkdir -p "$ROOT/build-benchmarks"
"$CC" -O2 -Wall -Wextra -mwindows \
    -o "$ROOT/build-benchmarks/qrx-gl-pe32.exe" \
    "$ROOT/benchmarks/opengl-pe32/qrx_opengl_pe32_benchmark.c" \
    -lopengl32 -lgdi32 -luser32 -lkernel32 -lmsvcrt
file "$ROOT/build-benchmarks/qrx-gl-pe32.exe" 2>/dev/null || true
printf 'QRX86 PE32 OpenGL sample built: %s\n' "$ROOT/build-benchmarks/qrx-gl-pe32.exe"
