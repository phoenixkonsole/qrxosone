#ifndef QRXVIRT_GUEST_MEDIA_H
#define QRXVIRT_GUEST_MEDIA_H
/* Guest-side m68k wire ABI. Use only inside a qrxvirt-aware QRX/AROS build.
   No classic Amiga chipset, SAGA or Voodoo registers are emulated here.
   Requires >= 4 MiB of RAM for the fixed 800x450 framebuffer. */
#include <stdint.h>
#define QRXV_GFX_FB      0x00200000u
#define QRXV_GFX_W       800u
#define QRXV_GFX_H       450u
#define QRXV_GFX_STRIDE  3200u
#define QRXV_MMIO        0x00ff0000u
#define QRXV_GFX         (QRXV_MMIO+0x100u)
#define QRXV_AUDIO       (QRXV_MMIO+0x200u)
#define QRXV_INPUT       (QRXV_MMIO+0x300u)
static inline uint32_t qrxv_read32(uint32_t addr) {
    return *(volatile const uint32_t *)(uintptr_t)addr;
}
static inline void qrxv_write32(uint32_t addr,uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)addr=val;
}
static inline void qrxv_rgb(uint32_t x,uint32_t y,uint8_t r,uint8_t g,uint8_t b) {
    if(x<QRXV_GFX_W && y<QRXV_GFX_H) {
        volatile uint32_t *fb=(volatile uint32_t *)(uintptr_t)QRXV_GFX_FB;
        fb[y*QRXV_GFX_W+x]=((uint32_t)r<<16)|((uint32_t)g<<8)|b;
    }
}
static inline void qrxv_present(void) { qrxv_write32(QRXV_GFX+0x14,1); }
static inline void qrxv_audio_s16(int16_t left,int16_t right) {
    qrxv_write32(QRXV_AUDIO+8,((uint32_t)(uint16_t)left<<16)|(uint16_t)right);
}
static inline uint32_t qrxv_input_count(void) {return qrxv_read32(QRXV_INPUT);}
static inline uint32_t qrxv_input_pop(void) {return qrxv_read32(QRXV_INPUT+4);}
static inline uint32_t qrxv_mouse_x(void) {return qrxv_read32(QRXV_INPUT+8);}
static inline uint32_t qrxv_mouse_y(void) {return qrxv_read32(QRXV_INPUT+12);}
static inline uint32_t qrxv_mouse_buttons(void) {return qrxv_read32(QRXV_INPUT+16);}
#endif
