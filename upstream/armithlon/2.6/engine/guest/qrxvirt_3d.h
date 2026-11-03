#ifndef QRXVIRT_3D_GUEST_H
#define QRXVIRT_3D_GUEST_H
/* QRX3D v0.6: QRXvirt-only guest ABI, not a hardware Voodoo or Glide DLL.
 * Intended for 32-bit big-endian 68k code built against qrxvirt BSP.
 * DO NOT call on real Amiga or FS-UAE; unrecognized MMIO may bus-fault.
 * Vertex structs must live in directly-addressable QRX guest RAM.
 */
#include "qrxvirt_media.h"
#define QRXV3D_BASE      (QRXV_MMIO+0x500u)
#define QRXV3D_MAGIC     0x51523344u
#define QRXV3D_VERSION   0x00060000u
#define QRXV3D_CLEAR     1u
#define QRXV3D_TRIANGLE  2u
#define QRXV3D_SWAP      3u
#define QRXV3D_TEXTURE_BIND 4u
#define QRXV3D_DEPTH_TEST  1u
#define QRXV3D_DEPTH_WRITE 2u
#define QRXV3D_ALPHA_BLEND 4u
#define QRXV3D_TEXTURE 8u
#define QRXV3D_CULL_BACK 16u
/* Wire format: exactly six BE longwords per vertex. Coordinates and texture
 * coordinates are signed 16.16; z is 0..65536 (far=1), argb=0xAARRGGBB. */
typedef struct QRXV3DVertex {
    int32_t x16, y16, z16;
    uint32_t argb;
    int32_t s16, t16;
} QRXV3DVertex;
static inline uint32_t qrxv3d_present(void){
    qrxv_write32(QRXV3D_BASE+0x10,QRXV3D_SWAP);
    return qrxv_read32(QRXV3D_BASE+0x0c);
}
static inline int qrxv3d_available(void){
    return qrxv_read32(QRXV3D_BASE)==QRXV3D_MAGIC &&
           qrxv_read32(QRXV3D_BASE+4)==QRXV3D_VERSION;
}
static inline uint32_t qrxv3d_clear(uint32_t argb,uint32_t depth){
    qrxv_write32(QRXV3D_BASE+0x20,argb);
    qrxv_write32(QRXV3D_BASE+0x24,depth);
    qrxv_write32(QRXV3D_BASE+0x10,QRXV3D_CLEAR);
    return qrxv_read32(QRXV3D_BASE+0x0c);
}
static inline uint32_t qrxv3d_bind_argb32_texture(const void *pixels,uint32_t stride,uint32_t w,uint32_t h){
    qrxv_write32(QRXV3D_BASE+0x20,(uint32_t)(uintptr_t)pixels);
    qrxv_write32(QRXV3D_BASE+0x24,stride);
    qrxv_write32(QRXV3D_BASE+0x28,w);
    qrxv_write32(QRXV3D_BASE+0x2c,h);
    qrxv_write32(QRXV3D_BASE+0x10,QRXV3D_TEXTURE_BIND);
    return qrxv_read32(QRXV3D_BASE+0x0c);
}
static inline uint32_t qrxv3d_triangle(const QRXV3DVertex *tri,uint32_t flags){
    qrxv_write32(QRXV3D_BASE+0x20,(uint32_t)(uintptr_t)tri);
    qrxv_write32(QRXV3D_BASE+0x24,flags);
    qrxv_write32(QRXV3D_BASE+0x10,QRXV3D_TRIANGLE);
    return qrxv_read32(QRXV3D_BASE+0x0c);
}
#endif
