#ifndef QRXHOSTFS_GUEST_H
#define QRXHOSTFS_GUEST_H
/* AROS m68k board-specific hardware protocol, not a DOS filesystem handler.
 * Intended for QRX 68040 guest only; querying this on real Amiga faults. */
#include <stdint.h>
#include <stddef.h>
#ifndef QRXV_HFS_BASE
#define QRXV_HFS_BASE ((uintptr_t)0xff000800UL)
#endif
#define QRXV_HFS_MAGIC 0x51525846u
#define QRXV_HFS_FEATURES 0x04u
#define QRXV_HFS_NAME 0x08u
#define QRXV_HFS_DATA 0x0cu
#define QRXV_HFS_OFFSET 0x10u
#define QRXV_HFS_LENGTH 0x14u
#define QRXV_HFS_COMMAND 0x18u
#define QRXV_HFS_STATUS 0x1cu
#define QRXV_HFS_RESULT 0x20u
#define QRXV_HFS_SIZE 0x24u
#define QRXV_HFS_LIST 1u
#define QRXV_HFS_STAT 2u
#define QRXV_HFS_READ 3u
#define QRXV_HFS_WRITE 4u
#define QRXV_HFS_DELETE 5u
#define QRXV_HFS_TRUNCATE 6u
static inline uint32_t qrxf_rd(unsigned reg){
#ifdef __m68k__
    uint32_t value;
    /* GCC otherwise folds a masked volatile read into byte-sized BTST. */
    __asm__ volatile("move.l (%1),%0" : "=d"(value) : "a"(QRXV_HFS_BASE+reg) : "memory");
    return value;
#else
    return *(volatile uint32_t*)(QRXV_HFS_BASE+reg);
#endif
}
static inline void qrxf_wr(unsigned reg,uint32_t v){*(volatile uint32_t*)(QRXV_HFS_BASE+reg)=v;}
/* guest pointers must be actual board DMA addresses: m68k MMU remapping
 * cannot be enabled for these buffers until translation is implemented. */
static inline uint32_t qrxf_request(unsigned opcode,const char *name,void *data,uint32_t ofs,uint32_t count){
    qrxf_wr(QRXV_HFS_NAME,(uint32_t)(uintptr_t)name);
    qrxf_wr(QRXV_HFS_DATA,(uint32_t)(uintptr_t)data);
    qrxf_wr(QRXV_HFS_OFFSET,ofs);
    qrxf_wr(QRXV_HFS_LENGTH,count);
    __asm__ volatile("" ::: "memory");
    qrxf_wr(QRXV_HFS_COMMAND,opcode);
    __asm__ volatile("" ::: "memory");
    return qrxf_rd(QRXV_HFS_STATUS);
}
#endif
