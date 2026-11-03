#ifndef QRX68100_JIT_H
#define QRX68100_JIT_H
/* QRX68100 68k -> AArch64 block JIT. Runs alongside Musashi on one context:
 * translated blocks work on the guest registers in pinned host registers, and
 * anything without a native translation goes to Musashi one instruction at a
 * time. Musashi stays the reference; tests/jit_diff_tests.c compares the two. */
#include "qrx68100_full.h"
#include <stdbool.h>

/* False on non-AArch64 hosts, when the code cache cannot be mapped, or when a
 * tracing option needs the full bus. start_instructions delays translation
 * until the ROM's early boot has finished writing the values the bus snoops. */
bool qrx_jit_init(QRXFullCPU *c, uint64_t start_instructions);
void qrx_jit_destroy(QRXFullCPU *c);
/* Counters since init; NULL-safe. Prints only with env QRX_JIT_STATS. */
void qrx_jit_report(const QRXFullCPU *c);
/* True while the CPU sits in STOP; for qrx_full_set_realtime. */
bool qrx_jit_cpu_stopped(void);
/* Test access: blocks translated and instructions run natively. */
void qrx_jit_counters(const QRXFullCPU *c, uint64_t *blocks, uint64_t *native,
                      uint64_t *interpreted);
#endif
