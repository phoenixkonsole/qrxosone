#ifndef QRXVIRT_GUEST_GPU_H
#define QRXVIRT_GUEST_GPU_H
/* QRXGPU 0.5: stable guest-side MMIO interface; only valid on QRXvirt.
 * Compile for 68k big-endian with AROS SDK. Never run on real Amiga
 * hardware: the QRX GPU registers do not exist there.
 * The 68k side must serialize access to the one-slot command mailbox.
 */
#include "qrxvirt_media.h"
#define QRXV_GPU (QRXV_MMIO+0x400u)
#define QRXV_GPU_MAGIC 0x51524735u
#define QRXV_GPU_VERSION 0x00050000u
#define QRXV_GPU_CAPS_FILL 1u
#define QRXV_GPU_CAPS_BLIT 2u
#define QRXV_GPU_CAPS_PRESENT 4u
#define QRXV_GPU_CMD_CLEAR 1u
#define QRXV_GPU_CMD_FILL 2u
#define QRXV_GPU_CMD_BLIT 3u
#define QRXV_GPU_CMD_PRESENT 4u
static inline int qrxv_gpu_available(void){
    return qrxv_read32(QRXV_MMIO)==0x51525831u &&
           qrxv_read32(QRXV_GPU)==QRXV_GPU_MAGIC &&
           qrxv_read32(QRXV_GPU+4)==QRXV_GPU_VERSION;
}
static inline uint32_t qrxv_gpu_issue(uint32_t op){
    qrxv_write32(QRXV_GPU+0x10,op);
    return qrxv_read32(QRXV_GPU+0x0c);
}
static inline uint32_t qrxv_gpu_clear(uint32_t rgb){
    qrxv_write32(QRXV_GPU+0x20,rgb);
    return qrxv_gpu_issue(QRXV_GPU_CMD_CLEAR);
}
static inline uint32_t qrxv_gpu_fill(uint16_t x,uint16_t y,
        uint16_t w,uint16_t h,uint32_t rgb){
    qrxv_write32(QRXV_GPU+0x20,((uint32_t)x<<16)|y);
    qrxv_write32(QRXV_GPU+0x24,((uint32_t)w<<16)|h);
    qrxv_write32(QRXV_GPU+0x28,rgb);
    return qrxv_gpu_issue(QRXV_GPU_CMD_FILL);
}
static inline uint32_t qrxv_gpu_blit(uint32_t src,uint32_t stride,
        uint16_t x,uint16_t y,uint16_t w,uint16_t h){
    qrxv_write32(QRXV_GPU+0x20,src);
    qrxv_write32(QRXV_GPU+0x24,stride);
    qrxv_write32(QRXV_GPU+0x28,((uint32_t)x<<16)|y);
    qrxv_write32(QRXV_GPU+0x2c,((uint32_t)w<<16)|h);
    return qrxv_gpu_issue(QRXV_GPU_CMD_BLIT);
}
static inline uint32_t qrxv_gpu_present(void){
    return qrxv_gpu_issue(QRXV_GPU_CMD_PRESENT);
}
/* AHI receives stereo signed PCM16 interleaved on m68k; guest buffer contents
 * must be BIG-ENDIAN stereo, physically mapped in QRX RAM, 4-byte aligned.
 * 1-4096 frames/transfer. Single producer only. */
static inline uint32_t qrxv_audio_pcm16_dma(uint32_t guest_addr,uint32_t frames){
    qrxv_write32(QRXV_AUDIO+0x14,guest_addr);
    qrxv_write32(QRXV_AUDIO+0x18,frames);
    qrxv_write32(QRXV_AUDIO+0x1c,1);
    return qrxv_read32(QRXV_AUDIO+0x20);
}
#endif
