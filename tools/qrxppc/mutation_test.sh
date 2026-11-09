#!/usr/bin/env bash
# QRXPPC mutation test for the byte/halfword memory-check helpers.
#
# The generated guest test uses a logical memory limit below the backing
# 256-KiB array.  Removing a range or alignment check is therefore safe: the
# mutant can touch the backing array, but the test must still report a fault.
#
# A seventh mutant restricts the indirect target cache to its first way, which
# turns it back into a direct-mapped cache; jit_chain_cache_2way must notice.
set -Eeuo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/tools/qrxppc"
SDK="$REPO/.build/qrx-sdk/bin"
CC="$SDK/aros-gcc"
ELF2HUNK="$SDK/elf2hunk"
HARNESS="$REPO/.build/m1-merged/qrx68100_full_run"
TREE="$REPO/.build/bundle/SystemFull"
STARTUP="$TREE/S/Startup-Sequence"
GUEST_LOG="$TREE/C/ppc.log"
BUNDLE="$REPO/.build/bundle/ARMithlon.app/Contents/Resources"
EXPECTED_STARTUP="05101fb8c9f85a6d2e01345919e8da54937071feb9922200400c4e43027ed5cb"
SECONDS_TO_RUN=25

TMP="$(mktemp -d "${TMPDIR:-/tmp}/qrxppc-mutation.XXXXXX")"
BACKUP_STARTUP="$TMP/Startup-Sequence.prod"
BACKUP_QRXPPC="$TMP/QRXPPC.prod"

cleanup()
{
    if [ -f "$BACKUP_STARTUP" ]; then
        cp -p "$BACKUP_STARTUP" "$STARTUP"
    fi
    if [ -f "$BACKUP_QRXPPC" ]; then
        cp -p "$BACKUP_QRXPPC" "$TREE/C/QRXPPC"
    fi
    rm -f "$GUEST_LOG"
    rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

if [ ! -x "$CC" ] || [ ! -x "$ELF2HUNK" ] || [ ! -x "$HARNESS" ]; then
    echo "QRXPPC toolchain or harness missing" >&2
    exit 1
fi
if [ ! -f "$STARTUP" ] || [ ! -f "$TREE/C/QRXPPC" ]; then
    echo "guest bundle missing" >&2
    exit 1
fi
if [ "$(shasum -a 256 "$STARTUP" | awk '{print $1}')" != "$EXPECTED_STARTUP" ]; then
    echo "Startup-Sequence is not at the production hash" >&2
    exit 1
fi

cp -p "$STARTUP" "$BACKUP_STARTUP"
cp -p "$TREE/C/QRXPPC" "$BACKUP_QRXPPC"
printf 'Echo "QRXPPC mutation test" >C:ppc.log\nC:QRXPPC >>C:ppc.log\n' > "$STARTUP"
mkdir -p /tmp/qrx-apps-empty
rm -f "$GUEST_LOG"

cat > "$TMP/mutate.py" <<'PY'
import sys

MUTATIONS = {
    "load8_range": (
        "uint32_t qrx_ppc_jit_load8(",
        "if (ea >= ppc->mem.size) {",
        "if (ea > ppc->mem.size) {",
    ),
    "load16_range": (
        "static int jit_load16(",
        "if ((ea & 1u) || ea >= ppc->mem.size ||\n"
        "        ppc->mem.size - ea < 2u) {",
        "if ((ea & 1u) || ea > ppc->mem.size) {",
    ),
    "load16_align": (
        "static int jit_load16(",
        "if ((ea & 1u) || ea >= ppc->mem.size ||\n"
        "        ppc->mem.size - ea < 2u) {",
        "if (ea >= ppc->mem.size ||\n"
        "        ppc->mem.size - ea < 2u) {",
    ),
    "store8_range": (
        "uint32_t qrx_ppc_jit_store8(",
        "if (ea >= ppc->mem.size) {",
        "if (ea > ppc->mem.size) {",
    ),
    "store16_range": (
        "uint32_t qrx_ppc_jit_store16(",
        "if ((ea & 1u) || ea >= ppc->mem.size ||\n"
        "        ppc->mem.size - ea < 2u) {",
        "if ((ea & 1u) || ea > ppc->mem.size) {",
    ),
    "smc_overlap_off": (
        "static int jit_smc_overlap(",
        "return jit && ea < jit->smc_hi && ea + width > jit->smc_lo;",
        "return 0;",
    ),
    "cache_lookup_1way": (
        "uintptr_t qrx_ppc_jit_chain_target(",
        "    for (i = 0; i < QRX_PPC_JIT_CACHE_WAYS; i++) {\n"
        "        if (set[i].valid && set[i].pc == pc &&",
        "    for (i = 0; i < 1u; i++) {\n"
        "        if (set[i].valid && set[i].pc == pc &&",
    ),
    "store16_align": (
        "uint32_t qrx_ppc_jit_store16(",
        "if ((ea & 1u) || ea >= ppc->mem.size ||\n"
        "        ppc->mem.size - ea < 2u) {",
        "if (ea >= ppc->mem.size ||\n"
        "        ppc->mem.size - ea < 2u) {",
    ),
}


def function_span(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return start, pos + 1
    raise ValueError("unterminated function: " + signature)


def main():
    src, dst, name = sys.argv[1:4]
    signature, old, new = MUTATIONS[name]
    text = open(src, encoding="utf-8").read()
    start, end = function_span(text, signature)
    body = text[start:end]
    if body.count(old) != 1:
        raise SystemExit("mutation target count is not 1: " + name)
    text = text[:start] + body.replace(old, new, 1) + text[end:]
    with open(dst, "w", encoding="utf-8") as out:
        out.write(text)


if __name__ == "__main__":
    main()
PY

cat > "$TMP/driver.py" <<'PY'
import sys

src, dst, test = sys.argv[1:4]
text = open(src, encoding="utf-8").read()
marker = "\nint main(void)\n"
pos = text.index(marker)
body = """
int main(void)
{
    jit_code = (uint8_t *)AllocVec(JIT_CODE_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!jit_code) {
        printf("FAIL jit code allocation\\n");
        return 1;
    }
    qrx_ppc_jit_init(&jit, jit_code, JIT_CODE_SIZE);
    TEST_CALL();
    FreeVec(jit_code);
    jit_code = NULL;
    printf("QRXPPC: %u failure(s)\\n", failures);
    return failures ? 1 : 0;
}
""".replace("TEST_CALL", test)
with open(dst, "w", encoding="utf-8") as out:
    out.write(text[:pos] + body)
PY

python3 "$TMP/driver.py" "$SRC/qrx_ppc_test.c" "$TMP/driver.c" test_jit_widths_limit
python3 "$TMP/driver.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_cache.c" test_jit_chain_cache_2way
python3 "$TMP/driver.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_smc.c" test_jit_smc_midblock

build_variant()
{
    local name="$1"
    local jit_source="$2"
    local driver="${3:-$TMP/driver.c}"
    local elf="$TMP/$name.elf"
    local hunk="$TMP/$name.hunk"
    local hunk_out

    "$CC" -O2 -m68040 -fno-pic -fno-PIC -I"$SRC" \
        "$SRC/qrx_ppc.c" "$SRC/qrx_ppc_fpu.c" \
        "$jit_source" "$driver" -o "$elf" > "$TMP/$name.build.log" 2>&1
    hunk_out="$("$ELF2HUNK" "$elf" "$hunk" 2>&1)"
    if printf '%s\n' "$hunk_out" | grep -q 'relocation(s) failed'; then
        echo "elf2hunk failed for $name" >&2
        return 1
    fi
    if ! file "$hunk" | grep -q 'AmigaOS loadseg()ble executable/binary'; then
        echo "$name is not a loadseg()ble Hunk" >&2
        return 1
    fi
    cp "$hunk" "$TREE/C/QRXPPC"
}

run_variant()
{
    local name="$1"
    local run_rc

    rm -f "$GUEST_LOG"
    set +e
    QRX_FULL_ADF="$BUNDLE/boot.adf" "$HARNESS" --cpu 68040 \
        --rom "$BUNDLE/aros-rom.bin" --ext-rom "$BUNDLE/aros-ext.bin" \
        --share "$TREE" --share-rw --apps /tmp/qrx-apps-empty \
        --seconds "$SECONDS_TO_RUN" > "$TMP/$name.harness.log" 2>&1
    run_rc=$?
    set -e
    if [ ! -f "$GUEST_LOG" ]; then
        echo "$name: no guest log (harness RC $run_rc)" >&2
        tail -8 "$TMP/$name.harness.log" >&2 || true
        return 1
    fi
    cp "$GUEST_LOG" "$TMP/$name.guest.log"
    rm -f "$GUEST_LOG"
}

build_variant baseline "$SRC/qrx_ppc_jit.c"
run_variant baseline
if ! grep -q '^jit_widths_limit: PASS$' "$TMP/baseline.guest.log" ||
   ! grep -q '^QRXPPC: 0 failure(s)$' "$TMP/baseline.guest.log"; then
    echo "baseline failed:" >&2
    cat "$TMP/baseline.guest.log" >&2
    exit 1
fi
echo "baseline: PASS"

mutants=(
    load8_range
    load16_range
    load16_align
    store8_range
    store16_range
    store16_align
)

survivors=0
for name in "${mutants[@]}"; do
    python3 "$TMP/mutate.py" "$SRC/qrx_ppc_jit.c" "$TMP/$name.c" "$name"
    build_variant "$name" "$TMP/$name.c"
    run_variant "$name"
    if grep -q '^jit_widths_limit: FAIL$' "$TMP/$name.guest.log"; then
        printf 'KILLED   %s\n' "$name"
    elif grep -q '^jit_widths_limit: PASS$' "$TMP/$name.guest.log"; then
        printf 'SURVIVED %s\n' "$name"
        survivors=$((survivors + 1))
    else
        printf 'ERROR    %s (no jit_widths_limit result)\n' "$name"
        tail -20 "$TMP/$name.guest.log" || true
        survivors=$((survivors + 1))
    fi
done

cache_mutant=cache_lookup_1way
python3 "$TMP/mutate.py" "$SRC/qrx_ppc_jit.c" "$TMP/$cache_mutant.c" "$cache_mutant"
build_variant "$cache_mutant" "$TMP/$cache_mutant.c" "$TMP/driver_cache.c"
run_variant "$cache_mutant"
if grep -q '^jit_chain_cache_2way: FAIL$' "$TMP/$cache_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$cache_mutant"
elif grep -q '^jit_chain_cache_2way: PASS$' "$TMP/$cache_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$cache_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no jit_chain_cache_2way result)\n' "$cache_mutant"
    tail -20 "$TMP/$cache_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

smc_mutant=smc_overlap_off
python3 "$TMP/mutate.py" "$SRC/qrx_ppc_jit.c" "$TMP/$smc_mutant.c" "$smc_mutant"
build_variant "$smc_mutant" "$TMP/$smc_mutant.c" "$TMP/driver_smc.c"
run_variant "$smc_mutant"
if grep -q '^jit_smc_midblock: FAIL$' "$TMP/$smc_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$smc_mutant"
elif grep -q '^jit_smc_midblock: PASS$' "$TMP/$smc_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$smc_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no jit_smc_midblock result)\n' "$smc_mutant"
    tail -20 "$TMP/$smc_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

cp -p "$BACKUP_QRXPPC" "$TREE/C/QRXPPC"
cp -p "$BACKUP_STARTUP" "$STARTUP"
rm -f "$GUEST_LOG"
trap - EXIT HUP INT TERM

if [ "$survivors" -ne 0 ]; then
    echo "mutation test FAILED: $survivors mutant(s) survived" >&2
    exit 1
fi

echo "mutation test PASS: $(( ${#mutants[@]} + 2 )) mutants killed"
shasum -a 256 "$STARTUP" "$TREE/C/QRXPPC"
rm -rf "$TMP"
