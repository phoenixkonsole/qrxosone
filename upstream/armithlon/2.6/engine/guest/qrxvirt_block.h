#ifndef QRXVIRT_BLOCK_GUEST_H
#define QRXVIRT_BLOCK_GUEST_H
/* QRX68100/AROS guest-side v0.1 raw block controller register contract.
 * 68040 must map FF000700 as uncacheable MMIO, not 24-bit Amiga alias.
 * This small guest header is NOT a complete AROS device driver or DOS volume. */
#include <stdint.h>
#define QRX_GUEST_BLOCK_BASE UINT32_C(0xFF000700)
#define QRX_GUEST_BLOCK_MAGIC UINT32_C(0x51525842)
#define QRX_GUEST_SECTOR_SIZE 512u
#define QRX_GUEST_BLK_CMD_READ 1u
#define QRX_GUEST_BLK_CMD_WRITE 2u
#define QRX_GUEST_BLK_OK 0u
#define QRX_GUEST_BLK_ERROR_RANGE 1u
#define QRX_GUEST_BLK_ERROR_IO 2u
#define QRX_GUEST_BLK_ERROR_RO 3u
#ifdef __m68k__
static inline int qrx_guest_block_io(uint32_t lba,uint32_t guest_dma,uint32_t command){
    volatile uint32_t *reg=(volatile uint32_t*)(uintptr_t)QRX_GUEST_BLOCK_BASE;
    if(reg[0]!=QRX_GUEST_BLOCK_MAGIC)return -1;
    reg[2]=lba;
    reg[3]=guest_dma;
    reg[4]=command;
    return (int)reg[5];
}
#endif
#endif
