#include "qrxmedia.h"
#include <limits.h>
#include <string.h>

/* QRXGPU virtual 2D accelerator. The host applies validated commands to the
 * guest's shared framebuffer. On macOS the existing Metal front-end uploads
 * this framebuffer; these primitives are NOT yet Metal compute/draw calls.
 * Nothing here accepts host pointers, shader programs or native API handles.
 */
static bool framebuffer(const QRXVM *v) {
    return v && v->ram.data &&
        (uint64_t)QRX_GFX_FRAMEBUFFER + (uint64_t)QRX_GFX_PITCH*QRX_GFX_HEIGHT <= v->ram.size;
}
static bool addr_safe(const QRXVM *v, uint32_t addr, uint32_t stride,
                      uint32_t w, uint32_t h) {
    if (!v || !v->ram.data || !w || !h || stride < (uint64_t)w*4u) return false;
    uint64_t end=(uint64_t)addr+(uint64_t)(h-1u)*stride+(uint64_t)w*4u;
    if (end>v->ram.size || end>QRX_IO_BASE || (addr & 3u)) return false;
    uint64_t fb_begin=QRX_GFX_FRAMEBUFFER;
    uint64_t fb_end=fb_begin+(uint64_t)QRX_GFX_PITCH*QRX_GFX_HEIGHT;
    return end<=fb_begin || addr>=fb_end; /* no aliasing across rows */
}
static void fill_pixel(uint8_t *p, uint32_t rgb) {
    p[0]=0; p[1]=(uint8_t)(rgb>>16);p[2]=(uint8_t)(rgb>>8);p[3]=(uint8_t)rgb;
}
static uint32_t execute(QRXVM *v,uint32_t cmd) {
    QRXMediaDevice *m=v->media;
    if (!framebuffer(v))return QRX_GPU_ERR_NO_FB;
    uint8_t *fb=v->ram.data+QRX_GFX_FRAMEBUFFER;
    uint32_t *a=m->gpu_args;
    if (cmd==QRX_GPU_CMD_CLEAR && m->accel)
        return qrx_accel_clear(m->accel,a[0],65535)?QRX_GPU_ERR_NONE:QRX_GPU_ERR_NO_FB;
    if (cmd==QRX_GPU_CMD_CLEAR) {
        for(uint32_t y=0;y<QRX_GFX_HEIGHT;y++)for(uint32_t x=0;x<QRX_GFX_WIDTH;x++)
            fill_pixel(fb+(size_t)y*QRX_GFX_PITCH+(size_t)x*4u,a[0]);
        return QRX_GPU_ERR_NONE;
    }
    if (cmd==QRX_GPU_CMD_FILL || cmd==QRX_GPU_CMD_BLIT) {
        uint32_t xy=a[cmd==QRX_GPU_CMD_FILL?0:2];
        uint32_t wh=a[cmd==QRX_GPU_CMD_FILL?1:3];
        uint32_t x=xy>>16,y=xy&65535u,w=wh>>16,h=wh&65535u;
        if (!w || !h) return QRX_GPU_ERR_BAD_ARGUMENT;
        if (x>=QRX_GFX_WIDTH || y>=QRX_GFX_HEIGHT) return QRX_GPU_ERR_BOUNDS;
        /* Validate full source region BEFORE clipping, and reject frame buffer
         * aliasing. No unchecked copies or guest-to-host code execution. */
        if (cmd==QRX_GPU_CMD_BLIT && !addr_safe(v,a[0],a[1],w,h))
            return QRX_GPU_ERR_BOUNDS;
        uint32_t cw=w>QRX_GFX_WIDTH-x?QRX_GFX_WIDTH-x:w;
        uint32_t ch=h>QRX_GFX_HEIGHT-y?QRX_GFX_HEIGHT-y:h;
        if(m->accel){
            if(cmd==QRX_GPU_CMD_FILL)
                return qrx_accel_rect(m->accel,x,y,cw,ch,a[2])?QRX_GPU_ERR_NONE:QRX_GPU_ERR_NO_FB;
            return qrx_accel_image(m->accel,x,y,cw,ch,v->ram.data+a[0],a[1])?QRX_GPU_ERR_NONE:QRX_GPU_ERR_NO_FB;
        }
        for(uint32_t row=0;row<ch;row++) {
            uint8_t *dst=fb+(size_t)(y+row)*QRX_GFX_PITCH+(size_t)x*4u;
            if (cmd==QRX_GPU_CMD_BLIT)
                memcpy(dst,v->ram.data+(size_t)a[0]+(size_t)row*a[1],(size_t)cw*4u);
            else
                for(uint32_t col=0;col<cw;col++)fill_pixel(dst+(size_t)col*4u,a[2]);
        }
        return QRX_GPU_ERR_NONE;
    }
    if (cmd==QRX_GPU_CMD_PRESENT) {
        return qrx_media_write(v,QRX_MEDIA_GFX_BASE+0x14,4,1) ? QRX_GPU_ERR_NONE : QRX_GPU_ERR_NO_FB;
    }
    return QRX_GPU_ERR_BAD_COMMAND;
}
bool qrx_gpu_read(QRXVM *v,uint32_t addr,unsigned width,uint32_t *value) {
    if (!v || !v->media || !value || width!=4 || (addr&3u) || addr<QRX_GPU_BASE) return false;
    QRXMediaDevice *m=v->media;
    switch (addr-QRX_GPU_BASE) {
        case 0: *value=QRX_GPU_MAGIC;return true;
        case 4: *value=QRX_GPU_ABI_VERSION;return true;
        case 8: *value=QRX_GPU_CAP_FILL|QRX_GPU_CAP_BLIT|QRX_GPU_CAP_PRESENT;return true;
        case 0xc: *value=m->gpu_error;return true;
        case 0x10: *value=m->gpu_last_cmd;return true;
        case 0x14: *value=m->gpu_success_count;return true;
        default:
            if(addr>=QRX_GPU_BASE+0x20 && addr<QRX_GPU_BASE+0x38){
                *value=m->gpu_args[(addr-(QRX_GPU_BASE+0x20))/4];return true;
            }
            return false;
    }
}
bool qrx_gpu_write(QRXVM *v,uint32_t addr,unsigned width,uint32_t value) {
    if (!v || !v->media || width!=4 || (addr&3u) || addr<QRX_GPU_BASE) return false;
    QRXMediaDevice *m=v->media;
    if(addr>=QRX_GPU_BASE+0x20 && addr<QRX_GPU_BASE+0x38){
        m->gpu_args[(addr-(QRX_GPU_BASE+0x20))/4]=value;
        return true;
    }
    if(addr==QRX_GPU_BASE+0x10) {
        m->gpu_last_cmd=value;
        m->gpu_error=execute(v,value);
        if(m->gpu_error==QRX_GPU_ERR_NONE)m->gpu_success_count++;
        return true; /* guest checks explicit error register; no CPU bus fault */
    }
    return false;
}
