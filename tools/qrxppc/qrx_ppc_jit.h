/*
 * QRXPPC P1.4 Tier 1 - portable PPC32 -> m68k basic-block compiler.
 *
 * The compiler translates a straight-line prefix of supported integer
 * instructions into m68k code.  Generated code keeps the guest register file
 * in qrx_ppc_t and uses D0/D1/A0 as scratch registers.  D2 holds a pending CR0
 * nibble; it is committed to cpu.cr at block boundaries and before checked
 * memory accesses.  Memory accesses call the small checked helpers below.
 * Unsupported instructions are left for the interpreter, so the interpreter
 * remains the semantic oracle.
 *
 * The code arena is supplied by the caller.  On AROS it must be executable
 * memory (AllocVec); qrx_ppc_jit_flush() performs the required 68040 cache
 * maintenance after every new block.
 */
#ifndef QRX_PPC_JIT_H
#define QRX_PPC_JIT_H

#include "qrx_ppc.h"

#define QRX_PPC_JIT_MAX_BLOCKS 24
#define QRX_PPC_JIT_MAX_SOURCE 128

typedef struct {
    uint32_t pc;
    uint32_t insns;
    uint32_t code_offset;
    uint32_t code_len;
    uint32_t source_len;                 /* bytes compared for self-modification */
    uint8_t source[QRX_PPC_JIT_MAX_SOURCE];
} qrx_ppc_jit_block_t;

typedef struct {
    uint8_t *code;
    uint32_t code_size;
    uint32_t code_align;
    uint32_t code_used;
    uint32_t nblocks;
    uint32_t hits;
    uint32_t misses;
    uint32_t recompiles;
    qrx_ppc_jit_block_t blocks[QRX_PPC_JIT_MAX_BLOCKS];
} qrx_ppc_jit_t;

void qrx_ppc_jit_init(qrx_ppc_jit_t *jit, void *code, uint32_t code_size);
void qrx_ppc_jit_reset(qrx_ppc_jit_t *jit);

/*
 * Execute the block at ppc->cpu.pc if it can be translated, otherwise return
 * 0 so the caller can execute one instruction with qrx_ppc_step().  A return
 * value of 1 means the native block ran; ppc->status, ppc->cpu.pc and
 * ppc->insns are updated exactly like the interpreter.
 */
int qrx_ppc_jit_run_block(qrx_ppc_jit_t *jit, qrx_ppc_t *ppc,
                          uint32_t budget);

qrx_ppc_status_t qrx_ppc_run_jit(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit,
                                 uint32_t max_insns);

/* Compile-time byte self-test; returns the number of mismatches (0 = good). */
unsigned qrx_ppc_jit_emit_selftest(void);

/* Checked memory helpers called by generated code. */
uint32_t qrx_ppc_jit_load8(qrx_ppc_t *ppc, uint32_t ea);
uint32_t qrx_ppc_jit_load16(qrx_ppc_t *ppc, uint32_t ea);
uint32_t qrx_ppc_jit_load16s(qrx_ppc_t *ppc, uint32_t ea);
uint32_t qrx_ppc_jit_load32(qrx_ppc_t *ppc, uint32_t ea);
void qrx_ppc_jit_store8(qrx_ppc_t *ppc, uint32_t ea, uint32_t value);
void qrx_ppc_jit_store16(qrx_ppc_t *ppc, uint32_t ea, uint32_t value);
void qrx_ppc_jit_store32(qrx_ppc_t *ppc, uint32_t ea, uint32_t value);

/* Lazy CR0 helpers called by generated code.  The value helper returns the
 * CR0 nibble for an integer result; commit_cr0 merges it into cpu.cr. */
uint32_t qrx_ppc_jit_cr0_value(qrx_ppc_t *ppc, uint32_t value);
void qrx_ppc_jit_commit_cr0(qrx_ppc_t *ppc, uint32_t cr0);

/* No-op on the host, CacheClearE/CacheClearU on AROS. */
void qrx_ppc_jit_flush(qrx_ppc_jit_t *jit);

#endif /* QRX_PPC_JIT_H */
