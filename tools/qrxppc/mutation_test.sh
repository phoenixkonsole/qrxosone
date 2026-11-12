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
    "rd_version_check": (
        "qrx_mix_status_t qrx_mix_rd_resolve(",
        "byte != QRX_MIX_RD_VERSION",
        "byte > QRX_MIX_RD_VERSION",
    ),
    "proc_rd_validation": (
        "qrx_mix_status_t qrx_mix_register_proc(",
        "if (rc != QRX_MIX_OK)\n        return rc;",
        "if (rc != QRX_MIX_OK && 0)\n        return rc;",
    ),
    "rd_invoke_isa": (
        "qrx_mix_status_t qrx_mix_rd_invoke(",
        "qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_PPC, &info)",
        "qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_68K, &info)",
    ),
    "nested_restore_off": (
        "qrx_mix_status_t qrx_mix_call(",
        "if (nested) {\n        p->cpu = saved;",
        "if (nested && 0) {\n        p->cpu = saved;",
    ),
    "procinfo_convention": (
        "qrx_mix_status_t qrx_mix_rd_procinfo_sig(",
        "if ((rd->proc_info & QRX_MIX_PI_CONVENTION_MASK) !=\n"
        "        QRX_MIX_PI_REGISTER_BASED)",
        "if ((rd->proc_info & QRX_MIX_PI_CONVENTION_MASK) ==\n"
        "        QRX_MIX_PI_REGISTER_BASED)",
    ),
    "procinfo_size_zero": (
        "qrx_mix_status_t qrx_mix_rd_procinfo_sig(",
        "if ((field & QRX_MIX_PI_PARAM_SIZE_MASK) == 0u)\n"
        "            return QRX_MIX_ERR_RD;",
        "if ((field & QRX_MIX_PI_PARAM_SIZE_MASK) == 4u)\n"
        "            return QRX_MIX_ERR_RD;",
    ),
    "proc_import_ppc_info": (
        "static qrx_mix_status_t mix_proc_import_sig(",
        "if (rc == QRX_MIX_OK)",
        "if (rc == QRX_MIX_OK && 0)",
    ),
    "proc_import_cached": (
        "static qrx_mix_status_t mix_run(",
        "if (mix->proc_sig_derived[slot]) {",
        "if (mix->proc_sig_derived[slot] && 0) {",
    ),
    "rd_invoke_procinfo": (
        "qrx_mix_status_t qrx_mix_rd_invoke(",
        "if (!sig) {",
        "if (!sig && 0) {",
    ),
    "rd_invoke_forced_pi": (
        "qrx_mix_status_t qrx_mix_rd_invoke(",
        "if (!sig) {",
        "if (1) {",
    ),
    "rd_call_procinfo": (
        "qrx_mix_status_t qrx_mix_rd_call(",
        "if (!sig) {",
        "if (!sig && 0) {",
    ),
    "index_procinfo": (
        "static qrx_mix_status_t mix_run(",
        "if (mix->import_desc_used[index]) {",
        "if (mix->import_desc_used[index] && 0) {",
    ),
    "call_raw_gate": (
        "qrx_mix_status_t qrx_mix_call(qrx_mix_t *mix, uint32_t entry,",
        "    } else if (!sig) {\n"
        "        return QRX_MIX_ERR_ARG;",
        "    } else if (0) {\n"
        "        return QRX_MIX_ERR_ARG;",
    ),
    "proc_raw_gate": (
        "qrx_mix_status_t qrx_mix_register_proc(qrx_mix_t *mix, uint32_t descriptor,",
        "    if (!sig)\n"
        "        return QRX_MIX_ERR_ARG;",
        "    if (!sig && 0)\n"
        "        return QRX_MIX_ERR_ARG;",
    ),
    "register_raw_gate": (
        "qrx_mix_status_t qrx_mix_register(qrx_mix_t *mix, unsigned index,",
        "            return QRX_MIX_ERR_SIG;\n"
        "    } else if (descriptor != QRX_MIX_RAW) {",
        "            return QRX_MIX_ERR_SIG;\n"
        "    } else if (1) {",
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

cat > "$TMP/driver_rd.py" <<'PYRD'
import sys

src, dst, test = sys.argv[1:4]
text = open(src, encoding="utf-8").read()
marker = "\nint main(void)\n"
pos = text.index(marker)
body = """
int main(void)
{
    memset(memory, 0, sizeof(memory));
    qrx_ppc_init(&ppc, memory, (uint32_t)sizeof(memory));
    TEST_CALL();
    printf("QRXPPC: %u failure(s)\\n", failures);
    return failures ? 1 : 0;
}
""".replace("TEST_CALL", test)
with open(dst, "w", encoding="utf-8") as out:
    out.write(text[:pos] + body)
PYRD

python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_rd.c" test_mixed_rd_errors
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_proc.c" test_mixed_proc_errors
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_nested.c" test_mixed_rd_nested
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_pi.c" test_mixed_rd_procinfo
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_pi_err.c" test_mixed_rd_procinfo_errors
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_piproc.c" test_mixed_proc_procinfo
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_piproc_err.c" test_mixed_proc_procinfo_errors
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_invoke_pi.c" test_mixed_rd_invoke_procinfo
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_invoke_pi_err.c" test_mixed_rd_invoke_procinfo_errors
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_rdcall_pi.c" test_mixed_rd_call_procinfo
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_rdcall_pi_err.c" test_mixed_rd_call_procinfo_errors
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_idxpi.c" test_mixed_import_procinfo
python3 "$TMP/driver_rd.py" "$SRC/qrx_ppc_test.c" "$TMP/driver_rawcall.c" test_mixed_errors

build_variant()
{
    local name="$1"
    local jit_source="$2"
    local driver="${3:-$TMP/driver.c}"
    local mix_source="${4:-$SRC/qrx_mix.c}"
    local elf="$TMP/$name.elf"
    local hunk="$TMP/$name.hunk"
    local hunk_out

    # Same flag as build.sh: tail calls would need a PC32 relocation, which
    # elf2hunk cannot express past 64 KiB of text.
    "$CC" -O2 -m68040 -fno-pic -fno-PIC -fno-optimize-sibling-calls \
        -I"$SRC" \
        "$SRC/qrx_ppc.c" "$SRC/qrx_ppc_fpu.c" \
        "$jit_source" "$mix_source" "$driver" -o "$elf" > "$TMP/$name.build.log" 2>&1
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

rd_mutant=rd_version_check
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$rd_mutant.c" "$rd_mutant"
build_variant "$rd_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_rd.c" "$TMP/$rd_mutant.c"
run_variant "$rd_mutant"
if grep -q '^mixed_rd_errors: FAIL$' "$TMP/$rd_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$rd_mutant"
elif grep -q '^mixed_rd_errors: PASS$' "$TMP/$rd_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$rd_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_errors result)\n' "$rd_mutant"
    tail -20 "$TMP/$rd_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

proc_mutant=proc_rd_validation
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$proc_mutant.c" "$proc_mutant"
build_variant "$proc_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_proc.c" "$TMP/$proc_mutant.c"
run_variant "$proc_mutant"
if grep -q '^mixed_proc_errors: FAIL$' "$TMP/$proc_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$proc_mutant"
elif grep -q '^mixed_proc_errors: PASS$' "$TMP/$proc_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$proc_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_proc_errors result)\n' "$proc_mutant"
    tail -20 "$TMP/$proc_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

invoke_mutant=rd_invoke_isa
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$invoke_mutant.c" "rd_invoke_isa"
build_variant "$invoke_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_nested.c" "$TMP/$invoke_mutant.c"
run_variant "$invoke_mutant"
if grep -q '^mixed_rd_nested: FAIL$' "$TMP/$invoke_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$invoke_mutant"
elif grep -q '^mixed_rd_nested: PASS$' "$TMP/$invoke_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$invoke_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_nested result)\n' "$invoke_mutant"
    tail -20 "$TMP/$invoke_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

restore_mutant=nested_restore_off
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$restore_mutant.c" "nested_restore_off"
build_variant "$restore_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_nested.c" "$TMP/$restore_mutant.c"
run_variant "$restore_mutant"
if grep -q '^mixed_rd_nested: FAIL$' "$TMP/$restore_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$restore_mutant"
elif grep -q '^mixed_rd_nested: PASS$' "$TMP/$restore_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$restore_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_nested result)\n' "$restore_mutant"
    tail -20 "$TMP/$restore_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi


pi_mutant=procinfo_convention
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$pi_mutant.c" "$pi_mutant"
build_variant "$pi_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_pi.c" "$TMP/$pi_mutant.c"
run_variant "$pi_mutant"
if grep -q '^mixed_rd_procinfo: FAIL$' "$TMP/$pi_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$pi_mutant"
elif grep -q '^mixed_rd_procinfo: PASS$' "$TMP/$pi_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$pi_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_procinfo result)\n' "$pi_mutant"
    tail -20 "$TMP/$pi_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

size_mutant=procinfo_size_zero
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$size_mutant.c" "$size_mutant"
build_variant "$size_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_pi_err.c" "$TMP/$size_mutant.c"
run_variant "$size_mutant"
if grep -q '^mixed_rd_procinfo_errors: FAIL$' "$TMP/$size_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$size_mutant"
elif grep -q '^mixed_rd_procinfo_errors: PASS$' "$TMP/$size_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$size_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_procinfo_errors result)\n' "$size_mutant"
    tail -20 "$TMP/$size_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

pip_mutant=proc_import_ppc_info
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$pip_mutant.c" "proc_import_ppc_info"
build_variant "$pip_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_piproc.c" "$TMP/$pip_mutant.c"
run_variant "$pip_mutant"
if grep -q '^mixed_proc_procinfo: FAIL$' "$TMP/$pip_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$pip_mutant"
elif grep -q '^mixed_proc_procinfo: PASS$' "$TMP/$pip_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$pip_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_proc_procinfo result)\n' "$pip_mutant"
    tail -20 "$TMP/$pip_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

pic_mutant=proc_import_cached
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$pic_mutant.c" "proc_import_cached"
build_variant "$pic_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_piproc_err.c" "$TMP/$pic_mutant.c"
run_variant "$pic_mutant"
if grep -q '^mixed_proc_procinfo_errors: FAIL$' "$TMP/$pic_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$pic_mutant"
elif grep -q '^mixed_proc_procinfo_errors: PASS$' "$TMP/$pic_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$pic_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_proc_procinfo_errors result)\n' "$pic_mutant"
    tail -20 "$TMP/$pic_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

rinv_mutant=rd_invoke_procinfo
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$rinv_mutant.c" "rd_invoke_procinfo"
build_variant "$rinv_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_invoke_pi_err.c" "$TMP/$rinv_mutant.c"
run_variant "$rinv_mutant"
if grep -q '^mixed_rd_invoke_procinfo_errors: FAIL$' "$TMP/$rinv_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$rinv_mutant"
elif grep -q '^mixed_rd_invoke_procinfo_errors: PASS$' "$TMP/$rinv_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$rinv_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_invoke_procinfo_errors result)\n' "$rinv_mutant"
    tail -20 "$TMP/$rinv_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

forced_mutant=rd_invoke_forced_pi
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$forced_mutant.c" "rd_invoke_forced_pi"
build_variant "$forced_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_invoke_pi.c" "$TMP/$forced_mutant.c"
run_variant "$forced_mutant"
if grep -q '^mixed_rd_invoke_procinfo: FAIL$' "$TMP/$forced_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$forced_mutant"
elif grep -q '^mixed_rd_invoke_procinfo: PASS$' "$TMP/$forced_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$forced_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_invoke_procinfo result)\n' "$forced_mutant"
    tail -20 "$TMP/$forced_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

call_mutant=rd_call_procinfo
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$call_mutant.c" "rd_call_procinfo"
build_variant "$call_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_rdcall_pi_err.c" "$TMP/$call_mutant.c"
run_variant "$call_mutant"
if grep -q '^mixed_rd_call_procinfo_errors: FAIL$' "$TMP/$call_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$call_mutant"
elif grep -q '^mixed_rd_call_procinfo_errors: PASS$' "$TMP/$call_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$call_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_rd_call_procinfo_errors result)\n' "$call_mutant"
    tail -20 "$TMP/$call_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

idx_mutant=index_procinfo
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$idx_mutant.c" "$idx_mutant"
build_variant "$idx_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_idxpi.c" "$TMP/$idx_mutant.c"
run_variant "$idx_mutant"
if grep -q '^mixed_import_procinfo: FAIL$' "$TMP/$idx_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$idx_mutant"
elif grep -q '^mixed_import_procinfo: PASS$' "$TMP/$idx_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$idx_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_import_procinfo result)\n' "$idx_mutant"
    tail -20 "$TMP/$idx_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

raw_mutant=register_raw_gate
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$raw_mutant.c" "$raw_mutant"
build_variant "$raw_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_idxpi.c" "$TMP/$raw_mutant.c"
run_variant "$raw_mutant"
if grep -q '^mixed_import_procinfo: FAIL$' "$TMP/$raw_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$raw_mutant"
elif grep -q '^mixed_import_procinfo: PASS$' "$TMP/$raw_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$raw_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_import_procinfo result)\n' "$raw_mutant"
    tail -20 "$TMP/$raw_mutant.guest.log" || true
    survivors=$((survivors + 1))
fi

procmutant=proc_raw_gate
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$procmutant.c" "$procmutant"
build_variant "$procmutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_piproc_err.c" "$TMP/$procmutant.c"
run_variant "$procmutant"
if grep -q '^mixed_proc_procinfo_errors: FAIL$' "$TMP/$procmutant.guest.log"; then
    printf 'KILLED   %s\n' "$procmutant"
elif grep -q '^mixed_proc_procinfo_errors: PASS$' "$TMP/$procmutant.guest.log"; then
    printf 'SURVIVED %s\n' "$procmutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_proc_procinfo_errors result)\n' "$procmutant"
    tail -20 "$TMP/$procmutant.guest.log" || true
    survivors=$((survivors + 1))
fi

rawcall_mutant=call_raw_gate
python3 "$TMP/mutate.py" "$SRC/qrx_mix.c" "$TMP/$rawcall_mutant.c" "$rawcall_mutant"
build_variant "$rawcall_mutant" "$SRC/qrx_ppc_jit.c" "$TMP/driver_rawcall.c" "$TMP/$rawcall_mutant.c"
run_variant "$rawcall_mutant"
if grep -q '^mixed_errors: FAIL$' "$TMP/$rawcall_mutant.guest.log"; then
    printf 'KILLED   %s\n' "$rawcall_mutant"
elif grep -q '^mixed_errors: PASS$' "$TMP/$rawcall_mutant.guest.log"; then
    printf 'SURVIVED %s\n' "$rawcall_mutant"
    survivors=$((survivors + 1))
else
    printf 'ERROR    %s (no mixed_errors result)\n' "$rawcall_mutant"
    tail -20 "$TMP/$rawcall_mutant.guest.log" || true
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

# The array holds the mutants that share the jit_widths_limit driver; the
# seventeen single-mutant blocks below build their own driver.
echo "mutation test PASS: $(( ${#mutants[@]} + 17 )) mutants killed"
shasum -a 256 "$STARTUP" "$TREE/C/QRXPPC"
rm -rf "$TMP"
