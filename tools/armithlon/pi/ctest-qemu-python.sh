#!/usr/bin/env bash
# Run ARMithlon's Python integration tests against an AArch64 cross build on an x86-64
# host: each AArch64 executable gets a same-named wrapper that starts it under
# qemu-aarch64 with the chosen Raspberry Pi CPU model.  On a real Pi just run ctest.
#   ctest-qemu-python.sh BUILD-DIR [cortex-a72|cortex-a76]
set -Eeuo pipefail
B="$(cd "$1" && pwd)"; CPU="${2:-cortex-a72}"
W="$B/qemu-wrappers-$CPU"
rm -rf "${W:?}"; mkdir -p "$W"
for f in "$B"/*; do
  [[ -f "$f" && -x "$f" ]] || continue
  head -c 20 "$f" | od -An -tx1 | tr -d ' \n' | grep -q '^7f454c46020101' || continue   # ELF64 LE
  printf '#!/bin/sh\nexec qemu-aarch64 -cpu %s -L /usr/aarch64-linux-gnu "%s" "$@"\n' "$CPU" "$f" > "$W/$(basename "$f")"
  chmod +x "$W/$(basename "$f")"
done
pass=0; fail=0
while IFS= read -r line; do
  name="${line%%|*}"; cmd="${line#*|}"
  cmd="${cmd//$B\//$W/}"
  if (cd "$B" && eval "$cmd") > "$W/$name.log" 2>&1; then pass=$((pass+1)); echo "PASS $name"
  else fail=$((fail+1)); echo "FAIL $name (log: $W/$name.log)"; fi
done < <(ctest --test-dir "$B" -N -V 2>/dev/null | awk '
  /^ *[0-9]+: Test command:/ { sub(/^ *[0-9]+: Test command: /,""); cmd=$0; next }
  /^ *Test +#[0-9]+: / { sub(/^ *Test +#[0-9]+: /,""); if (cmd ~ /python3/) print $0 "|" cmd; cmd="" }')
echo "QEMU-PYTHON-TESTS cpu=$CPU pass=$pass fail=$fail"
