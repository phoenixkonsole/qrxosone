#!/usr/bin/env bash
# QRX OS One 22.5.11 - host regression tests for the QRX86 interpreter/watchdog/trace path.
# HOST-ONLY: proves portable C semantics with the host GCC. It is NOT evidence that the
# Deadwood m68k build behaves identically; target runs are recorded separately.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-$ROOT/diagnostics-v6.6/tools/qrx86_grt_exec.c}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cc -std=c99 -O2 -Wall -Wextra -Werror -o "$TMP/qrx86" "$SRC"
python3 -B "$ROOT/tests-22.5.11/qrxgrt_fixture.py" "$TMP" >/dev/null
pass=0; fail=0
ok(){ echo "PASS $1"; pass=$((pass+1)); }
ko(){ echo "FAIL $1: $2"; fail=$((fail+1)); }
run(){ local t="$1"; shift; local s e; s=$(date +%s%N); set +e; timeout 20 "$TMP/qrx86" "$@" > "$TMP/out" 2>&1; RC=$?; set -e; e=$(date +%s%N); MS=$(( (e-s)/1000000 )); }

run ret --jit=off "$TMP/ret.qrxgrt"
grep -q 'QRX86 EXEC PASS' "$TMP/out" && [[ $RC -eq 0 ]] && ok "basic DllMain return" || ko "basic DllMain return" "rc=$RC"

run count6 --jit=off "$TMP/count6.qrxgrt"
grep -q 'steps=6 ' "$TMP/out" && ok "watchdog counts every single guest instruction (6 insns -> steps=6)" || ko "instruction count" "$(grep -o 'steps=[0-9]*' "$TMP/out" | head -1)"

run repfull --jit=off "$TMP/repfull.qrxgrt"
grep -q 'eax=00000000' "$TMP/out" && ok "REP STOSB with ECX>1e6 completes architecturally (ECX=0)" || ko "REP completion" "$(grep -o 'eax=[0-9a-f]*' "$TMP/out" | head -1)"

run repbudget --jit=off --max-steps 100 "$TMP/repfull.qrxgrt"
grep -q 'QRX86 WATCHDOG' "$TMP/out" && [[ $RC -ne 0 ]] && ok "1,000,016 REP iterations exceed a 100-step budget (deterministic accounting)" || ko "REP budget accounting" "rc=$RC $(grep -o 'steps=[0-9]*' "$TMP/out"|head -1)"

run movr8 --jit=off "$TMP/movr8.qrxgrt"
grep -q 'eax=00001234' "$TMP/out" && ok "MOV r8,imm8 (B0-B7) incl. AH" || ko "MOV r8,imm8" "$(grep -E 'STOP|eax=' "$TMP/out"|head -1)"

grep -q '^static QRXVM v;' "$SRC" && ok "QRXVM (~1.7 MB) is not a stack local in main()" || ko "VM storage" "QRXVM still on process stack"

run repspin --jit=off --max-steps 20000 "$TMP/repspin.qrxgrt"
if grep -q 'QRX86 WATCHDOG' "$TMP/out" && [[ $RC -ne 0 && $MS -lt 2000 ]]; then ok "REP iterations are charged to the watchdog budget (${MS} ms)"; else ko "REP watchdog" "rc=$RC ms=$MS"; fi

run spin --jit=off --max-steps 1000 "$TMP/spin.qrxgrt"
grep -q 'QRX86 WATCHDOG: guest instruction budget reached (1000)' "$TMP/out" && [[ $RC -ne 0 ]] && ok "endless loop stopped by --max-steps" || ko "max-steps" "rc=$RC"

# The exact wintrace command line issued by QRXDesktop (interpreter-only, traced, 100000 budget).
run wintrace --jit=off --trace --max-steps 100000 "$TMP/spin.qrxgrt"
lines=$(grep -c '^QRX86 BLOCK' "$TMP/out" || true)
if [[ $RC -ne 124 && $lines -le 2000 ]] && grep -q 'TRACE: output limit reached' "$TMP/out" && grep -q 'JIT=off' "$TMP/out"; then ok "wintrace terminates (${MS} ms), trace capped at $lines block lines, JIT off"; else ko "wintrace" "rc=$RC lines=$lines"; fi

run wintrace-rep --jit=off --trace --max-steps 100000 "$TMP/repspin.qrxgrt"
[[ $RC -ne 124 && $MS -lt 5000 ]] && ok "wintrace on REP-heavy endless guest terminates (${MS} ms)" || ko "wintrace REP" "rc=$RC ms=$MS"

run jit --jit=on "$TMP/ret.qrxgrt"
grep -q 'native JIT quarantined' "$TMP/out" && grep -q 'JIT=off' "$TMP/out" && grep -q 'native-jit-runs=0' "$TMP/out" && ok "--jit=on stays quarantined (interpreter, 0 native runs)" || ko "jit quarantine" "$(head -3 "$TMP/out")"

run ud2 --jit=off "$TMP/ud2.qrxgrt"
grep -q 'QRX86 STOP' "$TMP/out" && [[ $RC -ne 0 ]] && ok "unsupported opcode fails closed" || ko "ud2" "rc=$RC"

run noarg
[[ $RC -eq 2 ]] && ok "usage without handoff returns 2" || ko "usage" "rc=$RC"
run badopt --bogus "$TMP/ret.qrxgrt"
[[ $RC -eq 2 ]] && ok "unknown option rejected" || ko "badopt" "rc=$RC"
printf 'garbage' > "$TMP/bad.qrxgrt"; run badmagic --jit=off "$TMP/bad.qrxgrt"
[[ $RC -ne 0 ]] && grep -q 'bad handoff magic' "$TMP/out" && ok "corrupt handoff rejected" || ko "bad magic" "rc=$RC"

echo "QRX86 interpreter host tests: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
