/*
 * QRXPPC - portable PPC32 interpreter core.
 *
 * This is the P1 reference core from docs/QRXPPC_ROADMAP.md.  It deliberately
 * has no MacOS Toolbox, PEF loader, MMU or JIT yet: those are later layers.
 * The core is written in plain C so the same code can run in the ARMithlon
 * 68k guest and in a host-side differential test.
 */
#ifndef QRX_PPC_H
#define QRX_PPC_H

#include <stdint.h>
#include <stddef.h>

#define QRX_PPC_DEFAULT_MEM_SIZE (256u * 1024u)

typedef enum {
    QRX_PPC_OK = 0,
    QRX_PPC_TRAP,       /* sc or a trapping tw/twi */
    QRX_PPC_HALT,       /* pc reached 0 */
    QRX_PPC_FAULT,      /* memory or alignment fault */
    QRX_PPC_ILLEGAL     /* unimplemented/unknown instruction */
} qrx_ppc_status_t;

typedef struct {
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t lr;
    uint32_t ctr;
    uint32_t cr;
    uint32_t xer;
    uint32_t msr;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t fpscr;
    uint64_t fpr[32];   /* P1.3 FPRs, raw IEEE double bits */
} qrx_ppc_state_t;

typedef struct {
    uint8_t *data;
    uint32_t size;
} qrx_ppc_mem_t;

/* Optional guest address policy. A NULL callback retains flat memory. JIT
 * execution falls back to the interpreter while a callback is installed. */
#define QRX_PPC_ACCESS_READ 1u
#define QRX_PPC_ACCESS_WRITE 2u
#define QRX_PPC_ACCESS_EXEC 4u
typedef int (*qrx_ppc_access_fn)(void *ctx, uint32_t addr, uint32_t size,
                                 unsigned access);

typedef struct {
    qrx_ppc_state_t cpu;
    qrx_ppc_mem_t mem;
    qrx_ppc_access_fn access;
    void *access_ctx;
    uint32_t insns;
    uint32_t fault_addr;
    qrx_ppc_status_t status;
} qrx_ppc_t;

void qrx_ppc_init(qrx_ppc_t *ppc, uint8_t *mem, uint32_t size);
qrx_ppc_status_t qrx_ppc_step(qrx_ppc_t *ppc);
qrx_ppc_status_t qrx_ppc_run(qrx_ppc_t *ppc, uint32_t max_insns);

#endif /* QRX_PPC_H */
