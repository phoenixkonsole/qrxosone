#!/usr/bin/env bash
# QRX OS One 22.5.12 - host test suite for qrx86core / qrx-i386 / qrx-win32.
#   run-host-tests.sh <workdir> [busybox-i386] [reactos-system32-dir]
# Needs: gcc with -m32 support (gcc-multilib) and an x86 host that can execute
# i386 binaries (for the native side of the differential tests),
# i686-w64-mingw32-gcc for QRXClock, python3.  Host results are NOT a proof for
# the m68k target; see the FS-UAE scenario for that.
set -Euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WK="${1:?usage: run-host-tests.sh WORKDIR [busybox] [reactos-system32]}"; BB="${2:-}"; ROS="${3:-}"
mkdir -p "$WK"; cd "$WK"
pass=0; fail=0; skip=0
ok(){ echo "PASS $*"; pass=$((pass+1)); }
ko(){ echo "FAIL $*"; fail=$((fail+1)); }
sk(){ echo "SKIP $*"; skip=$((skip+1)); }

CF=(-std=gnu99 -O2 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation)
bash "$HERE/qmath/build-qmath.sh" gcc ar "$WK" >/dev/null || { ko "build qmath"; exit 1; }
gcc "${CF[@]}" -o qrx-i386 "$HERE"/core/qrx86core.c "$HERE"/linux/*.c "$WK/libqmath.a" -lm && ok "build qrx-i386" || { ko "build qrx-i386"; exit 1; }
gcc "${CF[@]}" -o qrx-win32 "$HERE"/core/qrx86core.c "$HERE"/win32/*.c "$WK/libqmath.a" -lm && ok "build qrx-win32" || { ko "build qrx-win32"; exit 1; }

# 1. integer differential test (several seeds)
for seed in 22512 1 2; do
  python3 "$HERE/tests/gen-diff-test.py" t$seed.S t$seed.meta $seed 12 >/dev/null
  gcc -m32 -nostdlib -static -o t$seed t$seed.S
  ./t$seed > nat$seed.bin && ./qrx-i386 ./t$seed > emu$seed.bin
  if python3 "$HERE/tests/compare-diff.py" t$seed.meta nat$seed.bin emu$seed.bin > cmp$seed.txt; then ok "integer differential seed $seed: $(tail -1 cmp$seed.txt)"; else ko "integer differential seed $seed"; head -20 cmp$seed.txt; fi
done

# 2. x87 differential test
gcc -m32 -static -O1 -mfpmath=387 -fno-pie -o fpu "$HERE/tests/fpu-diff.c" -lm
./fpu > fnat.txt && ./qrx-i386 ./fpu > femu.txt
if python3 "$HERE/tests/compare-fpu.py" fnat.txt femu.txt > fcmp.txt; then ok "x87 differential: $(tail -1 fcmp.txt)"; else ko "x87 differential"; head -20 fcmp.txt; fi

# 3. static glibc program
cat > hello.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int main(int argc, char **argv) { double s = 0; int i; char *m = malloc(100000);
  for (i = 1; i < 1000; i++) s += sin(i) / i;
  memset(m, 'x', 99999); m[99999] = 0;
  printf("hello argc=%d s=%.12f len=%zu sqrt2=%.15f\n", argc, s, strlen(m), sqrt(2.0)); return 3; }
EOF
gcc -m32 -static -O2 -o hello32 hello.c -lm
n="$(./hello32 a b)"; nrc=$?; e="$(./qrx-i386 ./hello32 a b)"; erc=$?
[[ "$n" == "$e" && $nrc == $erc ]] && ok "static glibc i386 program (output + exit code $erc)" || ko "static glibc program: '$n'/$nrc vs '$e'/$erc"

# 4. BusyBox applets (native vs emulated)
if [[ -n "$BB" && -x "$BB" ]]; then
  while IFS= read -r l; do
    [[ -z "$l" || "$l" == \#* ]] && continue
    n="$("$BB" sh -c "$l" 2>&1)"; e="$(timeout 120 ./qrx-i386 "$BB" sh -c "$l" 2>&1)"
    [[ "$n" == "$e" ]] && ok "busybox: $l" || { ko "busybox: $l"; echo " native: ${n:0:200}"; echo " emu   : ${e:0:200}"; }
  done < "$HERE/tests/busybox-cmds.txt"
else sk "busybox tests (no i386 busybox given)"; fi

# 5. Win32
if [[ -n "$ROS" && -f "$ROS/calc.exe" ]]; then
  cp "$ROS/calc.exe" "$ROS/winmine.exe" .
  out="$(./qrx-win32 -s "$HERE/../apps-22.5.12/x86/calc-test.txt" calc.exe 2>/dev/null)"; rc=$?
  echo "$out" | grep EXPECT
  [[ $rc == 0 ]] && echo "$out" | grep -q "expect_fail=0" && ok "ReactOS calc.exe scripted test" || ko "ReactOS calc.exe scripted test"
  ./qrx-win32 -s "$HERE/../apps-22.5.12/x86/winmine-test.txt" winmine.exe >/dev/null 2>&1 && [[ -s winmine-game.ppm && -s winmine-menu.ppm ]] && ok "ReactOS winmine.exe (menu + game snapshots)" || ko "ReactOS winmine.exe"
else sk "ReactOS calc/winmine (no System32 dir given)"; fi
# card games: program-supplied DLL (cards.dll), ANSI APIs, pattern brushes, clip exclusion
if [[ -n "$ROS" && -f "$ROS/sol.exe" && -f "$ROS/cards.dll" ]]; then
  cp "$ROS/sol.exe" "$ROS/spider.exe" "$ROS/cards.dll" .
  CK="python3 -B $HERE/tests/check-cards.py"
  ./qrx-win32 -s "$HERE/../apps-22.5.12/x86/sol-test.txt" sol.exe >/dev/null 2>&1 && $CK felt sol-dealt.ppm && $CK differ sol-dealt.ppm sol-drag.ppm \
    && ok "ReactOS sol.exe (cards.dll, deal + drag)" || ko "ReactOS sol.exe"
  ./qrx-win32 -s "$HERE/../apps-22.5.12/x86/spider-test.txt" spider.exe >/dev/null 2>&1 && $CK felt spider-game.ppm && $CK differ spider-dialog.ppm spider-game.ppm \
    && ok "ReactOS spider.exe (dialog + deal)" || ko "ReactOS spider.exe"
else sk "ReactOS sol/spider (no System32 dir given)"; fi
if command -v i686-w64-mingw32-gcc >/dev/null; then
  i686-w64-mingw32-gcc -O2 -mwindows -o qrxclock.exe "$HERE/win32/apps/qrxclock.c" -lgdi32
  ./qrx-win32 -s "$HERE/../apps-22.5.12/x86/clock-test.txt" qrxclock.exe >/dev/null 2>&1 && [[ -s clock-analog.ppm && -s clock-digital.ppm && -s clock-info.ppm ]] && ok "QRXClock (MinGW PE) analog/digital/info snapshots" || ko "QRXClock"
else sk "QRXClock (no mingw)"; fi

echo "QRX-X86-HOST-TESTS pass=$pass fail=$fail skip=$skip"
[[ $fail == 0 ]]
