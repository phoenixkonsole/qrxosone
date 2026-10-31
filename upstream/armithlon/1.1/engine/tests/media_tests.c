#include "qrxmedia.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct capture {uint32_t presents; uint8_t pixel[4];};
static void on_present(void *ctx,const uint8_t *pixels,uint32_t width,uint32_t height,uint32_t pitch) {
    struct capture *c=ctx;
    assert(width==800 && height==450 && pitch==3200);
    memcpy(c->pixel,pixels,4);
    c->presents++;
}
int main(void) {
    const size_t ram_len=8u*1024u*1024u;
    uint8_t *ram=calloc(ram_len,1);assert(ram);
    QRXVM vm;
    QRXMediaDevice *media=calloc(1,sizeof(*media));assert(media);
    struct capture cap={0};
    qrx_vm_init(&vm,ram,ram_len);
    qrx_media_init(media,on_present,&cap);
    qrx_vm_bind_media(&vm,media);
    uint32_t value=0;
    assert(qrx_vm_read(&vm,QRX_IO_BASE+8,4,&value));
    assert((value&(QRX_MEDIA_FEATURE_GFX|QRX_MEDIA_FEATURE_AUDIO|QRX_MEDIA_FEATURE_INPUT))==
            (QRX_MEDIA_FEATURE_GFX|QRX_MEDIA_FEATURE_AUDIO|QRX_MEDIA_FEATURE_INPUT));
    assert(qrx_vm_read(&vm,QRX_MEDIA_GFX_BASE,4,&value) && value==800);
    assert(qrx_vm_read(&vm,QRX_MEDIA_GFX_BASE+4,4,&value) && value==450);
    assert(qrx_vm_read(&vm,QRX_MEDIA_GFX_BASE+12,4,&value) && value==QRX_GFX_FRAMEBUFFER);
    assert(qrx_vm_write(&vm,QRX_GFX_FRAMEBUFFER,4,0x00112233u));
    assert(qrx_vm_write(&vm,QRX_MEDIA_GFX_BASE+0x14,4,1));
    assert(cap.presents==1 && cap.pixel[0]==0 && cap.pixel[1]==0x11 && cap.pixel[2]==0x22 && cap.pixel[3]==0x33);
    assert(qrx_vm_read(&vm,QRX_MEDIA_GFX_BASE+0x18,4,&value) && value==1);
    assert(!qrx_vm_write(&vm,QRX_MEDIA_GFX_BASE+0x14,4,2));
    assert(!qrx_vm_read(&vm,QRX_MEDIA_GFX_BASE+1,4,&value));
    assert(!qrx_vm_read(&vm,QRX_MEDIA_GFX_BASE+0x10,2,&value));
    assert(!qrx_vm_write(&vm,QRX_MEDIA_GFX_BASE+0x10,4,0));
    assert(!qrx_vm_read(&vm,QRX_IO_BASE+0x3fc,4,&value));
    assert(qrx_vm_read(&vm,QRX_GPU_BASE,4,&value) && value==QRX_GPU_MAGIC);
    assert(qrx_vm_read(&vm,QRX_GPU_BASE+4,4,&value) && value==QRX_GPU_ABI_VERSION);
    assert(!qrx_vm_read(&vm,QRX_IO_BASE+0x800,4,&value));
    assert(qrx_media_push_input(media,0x010C0001u));
    qrx_media_set_mouse(media,33,44,1);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE,4,&value) && value==1);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+4,4,&value) && value==0x010C0001u);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+4,4,&value) && value==0);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+8,4,&value) && value==33);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+12,4,&value) && value==44);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+16,4,&value) && value==1);
    for(unsigned i=0;i<QRX_INPUT_QUEUE_SIZE;i++) assert(qrx_media_push_input(media,i+1));
    assert(!qrx_media_push_input(media,0x777));
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+0x14,4,&value) && value==1);
    for(unsigned i=0;i<QRX_INPUT_QUEUE_SIZE;i++) {
        assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+4,4,&value) && value==i+1);
    }
    assert(qrx_vm_read(&vm,QRX_MEDIA_AUDIO_BASE,4,&value) && value==48000);
    assert(qrx_vm_read(&vm,QRX_MEDIA_AUDIO_BASE+4,4,&value) && value==2);
    assert(qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+8,4,0x7fff8000u));
    int16_t pcm[12];
    assert(qrx_media_pull_audio(media,pcm,6)==1);
    assert(pcm[0]==32767 && pcm[1]==-32768);
    for(int i=2;i<12;i++)assert(pcm[i]==0);
    assert(qrx_vm_read(&vm,QRX_MEDIA_AUDIO_BASE+0x0c,4,&value)&&value==0);
    assert(!qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+8,2,0));
    /* reset must retain the device but invalidate JIT. */
    assert(qrx_build_boot_monitor(ram,ram_len));
    assert(qrx_vm_reset(&vm));
    assert(vm.media==media);
    assert(qrx_vm_read(&vm,QRX_MEDIA_INPUT_BASE+8,4,&value) && value==33);
    qrx_vm_destroy(&vm);
    /* Insufficient frame RAM must not cause out of bounds reads. */
    uint8_t small[1024]={0};
    qrx_vm_init(&vm,small,sizeof(small));
    qrx_vm_bind_media(&vm,media);
    assert(!qrx_vm_write(&vm,QRX_MEDIA_GFX_BASE+0x14,4,1));
    qrx_vm_destroy(&vm);
    free(media);free(ram);
    puts("QRX media MMIO: framebuffer, input FIFO, audio FIFO, reset, bounds: PASS");
    return 0;
}
