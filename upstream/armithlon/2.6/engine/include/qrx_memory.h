#ifndef QRX_MEMORY_H
#define QRX_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The QRX68100 has a 32-bit guest address bus. 'Unlimited' means no artificial
 * 8/256 MiB cap, NOT an address space beyond 4 GiB. The upper 256 MiB is
 * reserved for virtual board devices and future ROM apertures. 68040 software
 * may access the extended contiguous RAM above the legacy 24-bit area.
 * Sparse host mappings avoid eagerly touching the guest allocation. */
#define QRX_RAM_MIN_BYTES ((size_t)1 << 20)
#define QRX_RAM_MAX_BYTES (UINT64_C(0xf0000000)) /* 3840 MiB; <= UINT32_MAX */

typedef struct { void *addr; size_t bytes; bool mapped; } QRXHostMemory;
uint64_t qrx_host_memory_bytes(void);
uint64_t qrx_host_memory_available_bytes(void);
/* Effective physical allocation budget; also honors container limits on Linux. */
uint64_t qrx_effective_host_memory_bytes(void);
size_t qrx_ram_auto_bytes(void); /* floor(min(host RAM, limit)/4) to whole MiB */
bool qrx_ram_validate(uint64_t bytes);
bool qrx_ram_allocate(QRXHostMemory *mem, size_t bytes);
void qrx_ram_release(QRXHostMemory *mem);
#endif
