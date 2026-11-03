#ifndef QRX68100_H
#define QRX68100_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint16_t sr;
    uint8_t halted;
    uint8_t reserved;
    uint32_t usp;
    uint32_t ssp;
    uint32_t vbr;
    uint8_t fault;
    uint8_t padding[3];
} QRXCPU;

typedef struct {
    uint8_t *data;
    size_t size;
} QRXRAM;

/* The educational bootstrap deliberately recognizes only a small 68k subset.
   This is not yet an AROS-booting emulator or 68080-compatible core. */
bool qrx_step(QRXCPU *cpu, const QRXRAM *ram);
int qrx_run_interpreter(QRXCPU *cpu, const QRXRAM *ram, uint32_t max_steps);
/* Returns false if no eligible basic block or on a non-AArch64 host. */
bool qrx_try_jit_microblock(QRXCPU *cpu, const QRXRAM *ram);
#endif
