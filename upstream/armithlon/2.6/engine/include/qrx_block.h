#ifndef QRX_BLOCK_H
#define QRX_BLOCK_H
#include "qrxvirt.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
/* QRX virtual-board block v0.1: explicit DMA sector controller, 512-byte sectors.
 * MMIO at qrxvirt+0x700 or full 68040 alias FF000700, aligned 32-bit only.
 * Sector 0 raw data, not an AmigaDOS RDB/FFS implementation. */
#define QRX_BLOCK_BASE (QRX_IO_BASE + 0x700u)
#define QRX_BLOCK_MAGIC (QRX_BLOCK_BASE + 0x00u) /* QRXB */
#define QRX_BLOCK_SECTORS (QRX_BLOCK_BASE + 0x04u)
#define QRX_BLOCK_LBA (QRX_BLOCK_BASE + 0x08u)
#define QRX_BLOCK_DMA (QRX_BLOCK_BASE + 0x0cu)
#define QRX_BLOCK_COMMAND (QRX_BLOCK_BASE + 0x10u) /* 1=READ, 2=WRITE */
#define QRX_BLOCK_STATUS (QRX_BLOCK_BASE + 0x14u) /* 0=success, 1=invalid range, 2=IO error, 3=readonly */
#define QRX_BLOCK_READONLY (QRX_BLOCK_BASE + 0x18u)
#define QRX_BLOCK_SECTOR_SIZE 512u
#define QRX_BLOCK_MAX_SECTORS 65536u
struct QRXBlock {
    FILE *file;
    uint32_t sectors,lba,dma,status;
    bool readonly;
};
typedef struct QRXBlock QRXBlock;
bool qrx_block_mount(QRXBlock *b,const char *path,uint32_t sectors,bool create,bool readonly);
void qrx_block_unmount(QRXBlock *b);
void qrx_block_attach(QRXVM *vm,QRXBlock *b);
bool qrx_block_read(QRXVM *vm,uint32_t addr,unsigned width,uint32_t *out);
bool qrx_block_write(QRXVM *vm,uint32_t addr,unsigned width,uint32_t val);
#endif
