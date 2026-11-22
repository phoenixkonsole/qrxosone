/* Arithlon 22.5.13 - Motorola 68040 CPU (integer unit + FPU) for x86-64 hosts.
 *
 * In the spirit of Amithlon: 68k code runs on an x86 machine without any Amiga
 * chipset emulation.  Guest memory is one flat 4 GiB host reservation (guest
 * address A lives at mem + A, big endian), so both the interpreter and the
 * dynamic recompiler (jit_x64.c) reach guest memory with one host access.
 *
 * The interpreter (m68k.c) is the reference: the JIT translates the frequent
 * instructions to x86-64 code and hands every other instruction to m68k_step().
 * Both share this state layout; the JIT addresses its fields with offsetof(). */
#ifndef ARITHLON_M68K_H
#define ARITHLON_M68K_H
#include <stdint.h>
#include <stddef.h>

#define M68K_OK          0
#define M68K_FAULT_ILL   1   /* illegal / unsupported instruction */
#define M68K_FAULT_MEM   2   /* access outside mapped guest memory */
#define M68K_FAULT_DIV0  3   /* integer division by zero */
#define M68K_FAULT_TRAP  4   /* TRAP/CHK/TRAPV the host did not handle */

typedef struct m68k m68k;
/* TRAP #vec (0-15): return 1 when handled; c->pc already points past the TRAP */
typedef int (*m68k_trap_fn)(m68k *c, int vec);

struct m68k {
    uint32_t d[8];
    uint32_t a[8];                  /* a[7] = user stack pointer (user mode only) */
    uint32_t pc;
    uint8_t x, n, z, v, c;          /* condition codes, each 0 or 1 */
    uint8_t pad_[3];
    double fp[8];                   /* FPU: IEEE double (not 80-bit extended) */
    uint32_t fpcr, fpsr, fpiar;
    uint8_t *mem;                   /* guest address 0 */
    uint32_t insn_pc;               /* address of the instruction being executed */
    int stop;                       /* set by hooks: leave the run loop */
    int fault;
    uint32_t fault_addr;
    char fault_msg[96];
    uint64_t icount;                /* instructions executed by the interpreter */
    m68k_trap_fn trap;
    void *user;
    void *jb;                       /* sigjmp_buf of the active run loop: faults leave through it */
};

void     m68k_init(m68k *c, uint8_t *mem);
/* one instruction; the caller has set c->jb (sigsetjmp) - a fault siglongjmps there */
void     m68k_step(m68k *c);
int      m68k_run(m68k *c);           /* interpreter loop until stop or fault; returns c->fault */
void     m68k_fault(m68k *c, int code, uint32_t addr, const char *msg) __attribute__((noreturn));
void     m68k_describe(const m68k *c, char *buf, size_t n);
uint32_t m68k_sr(const m68k *c);      /* CCR as the low byte of SR */

#endif
