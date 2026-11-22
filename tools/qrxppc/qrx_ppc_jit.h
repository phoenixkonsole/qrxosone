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
#define QRX_PPC_JIT_MAX_CHAIN 2

typedef struct {
    uint32_t pc;
    uint32_t insns;
    uint32_t code_offset;                /* normal C-entry, after chain entry */
    uint32_t chain_offset;               /* chain entry; no extra D2 frame */
    uint32_t code_len;
    uint32_t source_len;                 /* bytes compared for self-modification */
    uint8_t has_store;                   /* stores can invalidate chained code */
    uint8_t nchain;
    uint32_t chain_pc[QRX_PPC_JIT_MAX_CHAIN];
    uint32_t chain_patch[QRX_PPC_JIT_MAX_CHAIN]; /* absolute code-arena offset */
    uint8_t source[QRX_PPC_JIT_MAX_SOURCE];
} qrx_ppc_jit_block_t;

#define QRX_PPC_JIT_CACHE_SETS 8
#define QRX_PPC_JIT_CACHE_WAYS 2
#define QRX_PPC_JIT_CACHE_SIZE (QRX_PPC_JIT_CACHE_SETS * QRX_PPC_JIT_CACHE_WAYS)

/* Two-way set-associative target cache for indirect branches.  An entry names
 * a translated block and its chain entry, so a bclr/bcctr site can tail-jump
 * without patching: the target PC is only known at run time.  Both ways of a
 * set hold different targets, so two hot return sites with the same set index
 * no longer evict each other. */
typedef struct {
    uint32_t pc;
    uint32_t chain_offset;               /* absolute offset in the arena */
    uint8_t valid;
    uint8_t recent;                      /* last used way inside its set */
} qrx_ppc_jit_cache_t;

typedef struct {
    uint8_t *code;
    uint32_t code_size;
    uint32_t code_align;
    uint32_t code_used;
    uint32_t nblocks;
    uint32_t hits;
    uint32_t misses;
    uint32_t recompiles;
    uint32_t chain_insns;                /* instructions in the last native run */
    uint32_t chain_limit;                /* budget for chained blocks */
    qrx_ppc_jit_block_t blocks[QRX_PPC_JIT_MAX_BLOCKS];
    uint32_t cache_hits;                 /* indirect target-cache lookups */
    uint32_t cache_misses;
    qrx_ppc_jit_cache_t cache[QRX_PPC_JIT_CACHE_SIZE];
    uint32_t smc_lo;                     /* source range of the running block */
    uint32_t smc_hi;
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
/* Store helpers return 1 when the write touched the source bytes of the block
 * that is currently running; the generated code then leaves the block before
 * executing a stale following instruction. */
uint32_t qrx_ppc_jit_store8(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit, uint32_t ea,
                            uint32_t value);
uint32_t qrx_ppc_jit_store16(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit, uint32_t ea,
                             uint32_t value);
uint32_t qrx_ppc_jit_store32(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit, uint32_t ea,
                             uint32_t value);

/* Lazy CR0 helpers called by generated code.  The value helper returns the
 * CR0 nibble for an integer result; commit_cr0 merges it into cpu.cr. */
uint32_t qrx_ppc_jit_cr0_value(qrx_ppc_t *ppc, uint32_t value);
void qrx_ppc_jit_commit_cr0(qrx_ppc_t *ppc, uint32_t cr0);

/* Compare helpers write the selected CR field.  field_l contains the 3-bit
 * field number in bits 0..2 and the PPC L flag in bit 3. */
void qrx_ppc_jit_compare_s(qrx_ppc_t *ppc, uint32_t field_l, uint32_t a,
                           uint32_t b);
void qrx_ppc_jit_compare_u(qrx_ppc_t *ppc, uint32_t field_l, uint32_t a,
                           uint32_t b);

/* Conditional branch helper.  It updates PC and, when link is set, LR only
 * if the condition is true.  Returns 1 when the branch was taken so generated
 * code can tail-jump to the linked target or fallthrough. */
uint32_t qrx_ppc_jit_branch(qrx_ppc_t *ppc, uint32_t bo, uint32_t bi,
                            uint32_t target, uint32_t fallthrough,
                            uint32_t link);

/* Indirect branches (bclr/bcctr).  The target is LR or CTR exactly like the
 * interpreter's do_branch(); returns 1 when the branch was taken. */
uint32_t qrx_ppc_jit_branch_indirect(qrx_ppc_t *ppc, uint32_t bo, uint32_t bi,
                                     uint32_t from_ctr, uint32_t fallthrough,
                                     uint32_t link);

/* Indirect-chaining lookup.  Returns the chain entry address of a translated
 * block, or 0 so generated code falls back to the runner. */
uintptr_t qrx_ppc_jit_chain_target(qrx_ppc_jit_t *jit, uint32_t pc);

/* No-op on the host, CacheClearE/CacheClearU on AROS. */
void qrx_ppc_jit_flush(qrx_ppc_jit_t *jit);

#endif /* QRX_PPC_JIT_H */
