#ifndef QRXVIRT_H
#define QRXVIRT_H
#include "qrx68100.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* QRX virtual board ABI v1: an invented, chipset-less board, NOT SAGA. */
#define QRX_IO_BASE          UINT32_C(0x00ff0000)
#define QRX_IO_LENGTH        UINT32_C(0x00000900)
#define QRX_IO_MAGIC         UINT32_C(0x51525831) /* QRX1 */
#define QRX_IO_VERSION       UINT32_C(0x00010000)
#define QRX_FEATURE_UART     UINT32_C(1)
#define QRX_FEATURE_TIMER    UINT32_C(2)
#define QRX_FEATURE_SHUTDOWN UINT32_C(4)
#define QRX_FEATURE_BLOCK    UINT32_C(8)
#define QRX_FEATURE_HOSTFS   UINT32_C(16)
#define QRX_FEATURE_RAMSIZE  UINT32_C(32)
#define QRX_FEATURE_HOSTFS2  UINT32_C(64)
/* 32-bit guests read each word separately; reports bytes, not host pointers. */
#define QRX_IO_RAM_BYTES_LO (QRX_IO_BASE + UINT32_C(0x34))
#define QRX_IO_RAM_BYTES_HI (QRX_IO_BASE + UINT32_C(0x38))
#define QRX_IO_LOW_ALIAS    (QRX_IO_BASE + UINT32_C(0x3c))
struct QRXMediaDevice;
struct QRXBlock;
struct QRXHostFS;
struct QRXShell;
#define QRX_VCPU_CAPACITY 2
#define QRX_VCPU_ONLINE   1

#define QRX_SR_S 0x2000u
#define QRX_CCR_X 0x10u
#define QRX_CCR_N 0x08u
#define QRX_CCR_Z 0x04u
#define QRX_CCR_V 0x02u
#define QRX_CCR_C 0x01u

typedef void (*QRXConsoleFn)(void *ctx, uint8_t character);
typedef uint32_t (*QRXClockFn)(void *ctx); /* milliseconds, wraps after ~49 days */
typedef struct {
    struct QRXMediaDevice *media;
    struct QRXBlock *block;
    struct QRXHostFS *hostfs;
    struct QRXHostFS *hostfs2; /* optional second share, guest name TEST: */
    void (*dma_ram_write)(void *context,uint32_t addr,unsigned width);
    void *dma_opaque;
    struct QRXShell *system; /* optional QRX 1.8 firmware peripheral */
    QRXCPU cpu;
    QRXRAM ram;
    QRXConsoleFn console;
    QRXClockFn clock_ms;
    void *host_ctx;
    uint32_t last_exception;
    uint64_t steps;
    uint64_t ram_write_epoch; /* future JIT cache invalidation hook */
    bool stopped;
    bool jit_enabled; /* opt-in; default OFF; verified native-M1 gate required */
    void *jit_code;
    size_t jit_size;
    uint32_t jit_pc;
    uint32_t jit_next_pc;
    uint32_t jit_count;
    uint64_t jit_epoch;
    uint64_t jit_compiled;
    uint64_t jit_executed;
} QRXVM;

void qrx_vm_init(QRXVM *vm, void *ram_data, size_t ram_size);
void qrx_vm_destroy(QRXVM *vm); /* release executable code before disposing VM */
/* Optional AArch64 native JIT for whitelisted straight-line blocks. */
bool qrx_jit_vm_block(QRXVM *vm, uint64_t remaining_steps);
void qrx_jit_release(QRXVM *vm);
void qrx_vm_set_host(QRXVM *vm, QRXConsoleFn console, QRXClockFn clock_ms, void *ctx);
bool qrx_vm_reset(QRXVM *vm); /* loads SSP and PC from vector table */
/* Bus access never yields host pointers to the guest. Word/long are big-endian. */
bool qrx_vm_read(QRXVM *vm, uint32_t addr, unsigned size, uint32_t *out);
bool qrx_vm_write(QRXVM *vm, uint32_t addr, unsigned size, uint32_t value);
/* Executes one guest instruction, returns false on unhandled fault, STOP or shutdown. */
bool qrx_vm_step(QRXVM *vm);
/* 0 = stopped/shutdown, 1 = step budget, -1 = VM fault */
int qrx_vm_run(QRXVM *vm, uint64_t budget);
/* Exception delivery; simplified 68000-style SR/PC frame for initial qrxvirt BSP. */
bool qrx_vm_exception(QRXVM *vm, unsigned vector, uint32_t return_pc);

/* tiny boot monitor: test-specific ROM with QRX UART output and board shutdown */
bool qrx_build_boot_monitor(uint8_t *ram, size_t size);
#endif
