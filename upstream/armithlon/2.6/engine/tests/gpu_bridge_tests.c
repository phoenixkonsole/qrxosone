#include "qrxmedia.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct capture {unsigned count; unsigned char first[4];};
static void present(void *p,const uint8_t *fb,uint32_t w,uint32_t h,uint32_t pitch) {
    struct capture *c=p;
    assert(w==800 && h==450 && pitch==3200);
    c->count++;
    memcpy(c->first,fb,4);
}
static void reg(QRXVM *v,uint32_t offset,uint32_t value) {
    assert(qrx_vm_write(v,QRX_GPU_BASE+offset,4,value));
}
static unsigned char *pixel(uint8_t *mem,uint32_t x,uint32_t y) {
    return mem+QRX_GFX_FRAMEBUFFER+(size_t)y*QRX_GFX_PITCH+(size_t)x*4;
}
static void word(uint8_t *mem,size_t at,uint16_t v){mem[at]=(uint8_t)(v>>8);mem[at+1]=(uint8_t)v;}
static void longword(uint8_t *mem,size_t at,uint32_t v){word(mem,at,(uint16_t)(v>>16));word(mem,at+2,(uint16_t)v);}
static void write_from_68k(uint8_t *ram,size_t *pc,uint32_t address,uint32_t value) {
    /* MOVE.L #address,A0; MOVE.L #value,(A0) - real CPU bus dispatch. */
    word(ram,*pc,0x207c);longword(ram,*pc+2,address);*pc+=6;
    word(ram,*pc,0x20bc);longword(ram,*pc+2,value);*pc+=6;
}
int main(void) {
    const size_t ram_len=8u*1024u*1024u;
    uint8_t *ram=calloc(1,ram_len); assert(ram);
    QRXMediaDevice *media=calloc(1,sizeof(*media)); assert(media);
    QRXVM vm;struct capture cap={0};
    qrx_vm_init(&vm,ram,ram_len);qrx_media_init(media,present,&cap);qrx_vm_bind_media(&vm,media);
    uint32_t v=0;
    assert(qrx_vm_read(&vm,QRX_GPU_BASE,4,&v) && v==QRX_GPU_MAGIC);
    assert(qrx_vm_read(&vm,QRX_IO_BASE+8,4,&v) && (v&QRX_MEDIA_FEATURE_GPU2D));
    reg(&vm,0x20,0x00112233u);reg(&vm,0x10,QRX_GPU_CMD_CLEAR);
    assert(qrx_vm_read(&vm,QRX_GPU_BASE+0xc,4,&v) && v==0);
    assert(pixel(ram,0,0)[1]==0x11 && pixel(ram,799,449)[3]==0x33);
    reg(&vm,0x20,(10u<<16)|20u);reg(&vm,0x24,(5u<<16)|6u);
    reg(&vm,0x28,0x00abcdefu);reg(&vm,0x10,QRX_GPU_CMD_FILL);
    assert(pixel(ram,10,20)[1]==0xab && pixel(ram,14,25)[3]==0xef);
    assert(pixel(ram,9,20)[1]==0x11);
    reg(&vm,0x20,(799u<<16)|449u);reg(&vm,0x24,(20u<<16)|20u);
    reg(&vm,0x28,0x00010203u);reg(&vm,0x10,QRX_GPU_CMD_FILL);
    assert(pixel(ram,799,449)[3]==3 && pixel(ram,798,449)[3]==0x33);
    reg(&vm,0x20,0);reg(&vm,0x24,5);reg(&vm,0x10,QRX_GPU_CMD_FILL); /* width 0 invalid */
    assert(qrx_vm_read(&vm,QRX_GPU_BASE+0xc,4,&v) && v==QRX_GPU_ERR_BAD_ARGUMENT);
    reg(&vm,0x10,99);
    assert(qrx_vm_read(&vm,QRX_GPU_BASE+0xc,4,&v) && v==QRX_GPU_ERR_BAD_COMMAND);
    assert(!qrx_vm_write(&vm,QRX_GPU_BASE+0x21,4,1));
    /* Source bytes 00 78 56 34, blitted by a bounded guest address. */
    longword(ram,0x11000,0x00785634u);
    reg(&vm,0x20,0x11000);reg(&vm,0x24,4);
    reg(&vm,0x28,(15u<<16)|25u);reg(&vm,0x2c,(1u<<16)|1u);
    reg(&vm,0x10,QRX_GPU_CMD_BLIT);
    assert(pixel(ram,15,25)[1]==0x78 && pixel(ram,15,25)[3]==0x34);
    /* Invalid guest pointer and overlapping framebuffer rejected with no copy. */
    reg(&vm,0x20,0xffffff00u);reg(&vm,0x10,QRX_GPU_CMD_BLIT);
    assert(qrx_vm_read(&vm,QRX_GPU_BASE+0xc,4,&v) && v==QRX_GPU_ERR_BOUNDS);
    reg(&vm,0x20,QRX_GFX_FRAMEBUFFER);reg(&vm,0x10,QRX_GPU_CMD_BLIT);
    assert(qrx_vm_read(&vm,QRX_GPU_BASE+0xc,4,&v) && v==QRX_GPU_ERR_BOUNDS);
    reg(&vm,0x10,QRX_GPU_CMD_PRESENT);
    assert(cap.count==1 && cap.first[1]==0x11);

    /* PCM16BE DMA: copy two stereo frames from guest RAM, preserve polarity. */
    longword(ram,0x12000,0x7fff8000u);longword(ram,0x12004,0x0001ffffu);
    assert(qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+0x14,4,0x12000u));
    assert(qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+0x18,4,2));
    assert(qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+0x1c,4,1));
    assert(qrx_vm_read(&vm,QRX_MEDIA_AUDIO_BASE+0x20,4,&v) && v==0);
    int16_t pcm[4]={0};
    assert(qrx_media_pull_audio(media,pcm,2)==2);
    assert(pcm[0]==32767 && pcm[1]==-32768 && pcm[2]==1 && pcm[3]==-1);
    assert(qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+0x14,4,0x7fffffff));
    assert(qrx_vm_write(&vm,QRX_MEDIA_AUDIO_BASE+0x1c,4,1));
    assert(qrx_vm_read(&vm,QRX_MEDIA_AUDIO_BASE+0x20,4,&v) && v==1);

    /* Cross CPU boundary: an actual 68k instruction stream writes GPU regs. */
    longword(ram,0,0x100000);longword(ram,4,0x1000);
    size_t pc=0x1000;
    write_from_68k(ram,&pc,QRX_GPU_BASE+0x20,0x00ff0000u);
    write_from_68k(ram,&pc,QRX_GPU_BASE+0x10,QRX_GPU_CMD_CLEAR);
    write_from_68k(ram,&pc,QRX_GPU_BASE+0x10,QRX_GPU_CMD_PRESENT);
    write_from_68k(ram,&pc,QRX_IO_BASE+0x1c,1);
    assert(qrx_vm_reset(&vm));
    assert(qrx_vm_run(&vm,24)==0);
    assert(cap.count==2 && cap.first[1]==0xff && cap.first[2]==0x00);
    qrx_vm_destroy(&vm);free(media);free(ram);
    puts("GPU mailbox + 68k guest program + framebuffer DMA + PCM DMA: PASS");
    return 0;
}
