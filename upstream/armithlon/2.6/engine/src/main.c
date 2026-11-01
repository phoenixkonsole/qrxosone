#include "qrx68100.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* MOVEQ #42,D0; MOVEQ #-5,D1; NOP; TRAP #15 (foundation monitor). */
    uint8_t program[] = { 0x70,0x2a, 0x72,0xfb, 0x4e,0x71, 0x4e,0x4f };
    QRXRAM r = { program, sizeof(program) };
    QRXCPU oracle = { 0 }, translated = { 0 };
    oracle.sr = translated.sr = 0x2010; /* X preserved. */
    int steps = qrx_run_interpreter(&oracle, &r, 100);
    if (steps != 4 || oracle.d[0] != 42 || oracle.d[1] != 0xfffffffbu ||
        oracle.pc != 8 || oracle.sr != 0x2018 || !oracle.halted) {
        fputs("FAIL: portable 68k oracle\n", stderr);
        return 1;
    }
    printf("Interpreter: PASS (D0=%u D1=%d SR=%04x PC=%u)\n", oracle.d[0], (int32_t)oracle.d[1], oracle.sr, oracle.pc);
    if (qrx_try_jit_microblock(&translated, &r)) {
        if (memcmp(&translated, &oracle, sizeof(oracle)) != 0) {
            fputs("FAIL: ARM64 JIT state mismatch\n", stderr);
            return 2;
        }
        puts("ARM64 JIT: PASS (exact CPU state for smoke microblock)");
    } else {
        puts("ARM64 JIT: SKIPPED (non-AArch64 host or JIT mapping unavailable)");
    }
    return 0;
}
