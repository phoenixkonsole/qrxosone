#ifndef QRX68100_PARALLEL_H
#define QRX68100_PARALLEL_H
/* QRX68100 parallel *fast path*, ABI 1.6: only ADDQ.L/SUBQ.L #n,Dx ; BRA.S -4.
 * Both CPUs already have independently initialized Musashi contexts.
 * No Musashi calls and no shared-memory writes occur on worker threads.
 * On AArch64, the arithmetic loop is emitted as actual native instructions.
 * Unsupported opcodes return 0; caller can use the serial Musashi fallback.
 */
#include "qrx68100_dual.h"
#include <stdint.h>

/* Return 1 if real ARM64 JIT ran in two threads, 2 if portable C executed in
 * two threads, 0 if not eligible (no state changed), -1 if resource failure.
 * iterations is an EXACT count of 2-op guest loops, NOT emulated clock cycles.
 * All QRX guest memory changes must be made via the QRX bus while idle.
 */
int qrx_dual_parallel_loops(QRXDual *d, uint32_t iterations);
void qrx_dual_parallel_release(QRXDual *d);
int qrx_dual_native_supported(void);
#endif
