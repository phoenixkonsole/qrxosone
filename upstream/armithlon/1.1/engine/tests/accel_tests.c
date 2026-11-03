#include "qrx_demo.h"
#include "qrx86_gpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t rgba(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
int main(void){
    QRXAccelStream gpu={0};assert(qrx_accel_init(&gpu));
    QRXDemo *d=qrx_demo_create(NULL,NULL);assert(d);
    QRXMediaDevice *m=qrx_demo_media(d);
    m->accel=&gpu;
    /* Actual 68k CPU program produces 2D commands, 3D triangles, DMA and
     * PRESENT. Metal receives validated geometry rather than a CPU framebuffer. */
    assert(qrx_demo_frame(d));
    QRXAccelFrame *f=qrx_accel_take_latest(&gpu);assert(f);
    assert(f->sequence==1 && f->count>=17);
    assert(f->commands[0].kind==QRX_AC_CLEAR);
    assert(f->commands[0].color==0x00132349);
    unsigned foundTriangles=0,foundRect=0;
    for(unsigned i=0;i<f->count;i++){
        if(f->commands[i].kind==QRX_AC_TRIANGLE){
            foundTriangles++;
            assert(f->commands[i].vert[0].x>=150 && f->commands[i].vert[0].x<300);
            assert(f->commands[i].vert[2].b>0.8f);
        }
        if(f->commands[i].kind==QRX_AC_RECT)foundRect++;
    }
    assert(foundTriangles==1 && foundRect>=15);
    qrx_accel_frame_release(f);
    /* Drop old unpublished ready frames: only newest is rendered. */
    assert(qrx_demo_frame(d));assert(qrx_demo_frame(d));
    f=qrx_accel_take_latest(&gpu);assert(f&&f->sequence==3&&gpu.dropped==1);
    qrx_accel_frame_release(f);
    m->accel=NULL;qrx_demo_destroy(d);
    /* Win32 QRX86 HLE backend 0x00RRGGBB qsurf.pix -> actual GPU frame */
    uint32_t w32[4]={0x00112233,0x00445566,0x00abcdef,0x00eeccaa};
    assert(qrx86_gpu_present(&gpu,w32,2,2,2*4,QRX86_PIX_XRGB32,NULL));
    f=qrx_accel_take_latest(&gpu);assert(f&&f->count==2);
    assert(f->commands[1].kind==QRX_AC_IMAGE);
    assert(rgba(f->commands[1].image)==0x112233ff);
    assert(rgba(f->commands[1].image+12)==0xeeccaaff);
    qrx_accel_frame_release(f);
    uint16_t rgb565[2]={0xf800,0x07e0};
    assert(qrx86_gpu_present(&gpu,rgb565,2,1,4,QRX86_PIX_RGB565,NULL));
    f=qrx_accel_take_latest(&gpu);assert(f);
    assert(rgba(f->commands[1].image)==0xff0000ff);
    assert(rgba(f->commands[1].image+4)==0x00ff00ff);
    qrx_accel_frame_release(f);
    uint8_t palidx[2]={1,2};uint32_t pal[256]={0};pal[1]=0x00664422;pal[2]=0x00010203;
    assert(qrx86_gpu_present(&gpu,palidx,2,1,2,QRX86_PIX_PAL8,pal));
    f=qrx_accel_take_latest(&gpu);assert(f);
    assert(rgba(f->commands[1].image)==0x664422ff);
    qrx_accel_frame_release(f);
    assert(!qrx86_gpu_present(&gpu,palidx,800,2,2,QRX86_PIX_PAL8,pal));
    assert(!qrx_accel_take_latest(&gpu));
    /* Host-owned ARGB8 image snapshot: changes to caller RAM do not affect GPU. */
    uint8_t argb[4]={128,30,60,90};
    assert(qrx_accel_image(&gpu,0,0,1,1,argb,4));
    argb[1]=255;
    assert(qrx_accel_commit(&gpu));f=qrx_accel_take_latest(&gpu);assert(f);
    assert(rgba(f->commands[0].image)==0x1e3c5a80);
    qrx_accel_frame_release(f);
    /* Command and payload limits cannot be exceeded. */
    for(unsigned i=0;i<QRX_ACCEL_MAX_COMMANDS;i++)assert(qrx_accel_rect(&gpu,0,0,1,1,0));
    assert(!qrx_accel_rect(&gpu,0,0,1,1,0));
    qrx_accel_destroy(&gpu);
    puts("QRX 0.7: real 68k -> GPU command stream and QRX86 Win32 GDI/DirectDraw 8/16/32 upload PASS");
    return 0;
}
