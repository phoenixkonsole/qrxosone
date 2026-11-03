#include "qrx86_gpu.h"
#include "qrxmedia.h"
#include "qrx86_ipc.h"
#include <stdlib.h>
/* QRX86's own Win32 HLE already produces qsurf/DirectDraw surface pixels.
 * This adapter supports a windowless native CPU-vs-GPU display test and the
 * opt-in backend integration patch. It never treats x86 pointers as host pointers. */
bool qrx86_gpu_present(QRXAccelStream *gpu,const void *pixels,uint32_t w,uint32_t h,
                       uint32_t pitch,unsigned format,const uint32_t *palette){
    if(!gpu||!pixels||!w||!h||w>QRX_GFX_WIDTH||h>QRX_GFX_HEIGHT)return false;
    if(format<QRX86_PIX_XRGB32||format>QRX86_PIX_PAL8)return false;
    const size_t bpp=format==QRX86_PIX_XRGB32?4:format==QRX86_PIX_RGB565?2:1;
    if(pitch<(size_t)w*bpp||(format==QRX86_PIX_PAL8&&!palette))return false;
    uint8_t *argb=malloc((size_t)w*h*4);
    if(!argb)return false;
    const uint8_t *src=pixels;
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
        uint32_t rgb=0;
        const uint8_t *p=src+(size_t)y*pitch+(size_t)x*bpp;
        if(format==QRX86_PIX_XRGB32){
            uint32_t n;__builtin_memcpy(&n,p,4);rgb=n&0xffffffu;
        }else if(format==QRX86_PIX_RGB565){
            uint16_t n;__builtin_memcpy(&n,p,2);
            unsigned r=((n>>11)&31u),g=((n>>5)&63u),b=n&31u;
            rgb=((r*255u/31u)<<16)|((g*255u/63u)<<8)|(b*255u/31u);
        }else rgb=palette[*p]&0xffffffu;
        uint8_t *d=argb+((size_t)y*w+x)*4;
        d[0]=255;d[1]=(uint8_t)(rgb>>16);d[2]=(uint8_t)(rgb>>8);d[3]=(uint8_t)rgb;
    }
    /* Do not replace a ready GPU frame on invalid input. A fully validated
     * surface can be serialized into the same Metal/QRXGPU stream as 68k. */
    bool ok=qrx_accel_clear(gpu,0x00000000u,65535u)&&
            qrx_accel_image(gpu,0,0,w,h,argb,(size_t)w*4);
    free(argb);
    return ok&&qrx_accel_commit(gpu);
}

/* Optional opt-in host backend glue. When no renderer is attached, headless
 * QRX86 behaves exactly as before (so existing tests are unaffected). */
static QRXAccelStream *s_qrx86_target;
void qrx86_gpu_attach(QRXAccelStream *gpu){s_qrx86_target=gpu;}
void qrx86_gpu_backend_present(const uint32_t *pixels,int width,int height){
    if(!pixels||width<=0||height<=0 || width>800||height>450)return;
    if(s_qrx86_target){
        (void)qrx86_gpu_present(s_qrx86_target,pixels,(uint32_t)width,(uint32_t)height,
                            (uint32_t)width*4,QRX86_PIX_XRGB32,NULL);
    }else {
        const char *path=getenv("ARMITHLON_QRX86_SOCKET");
        if(path && *path)(void)qrx86_ipc_send(path,pixels,(uint32_t)width,(uint32_t)height);
    }
}
