#include "qrxmedia.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RAMSZ (8u*1024u*1024u)
#define VTX 0x18000u
#define TEX 0x19000u
struct sink{unsigned frames;uint32_t color;};
static uint32_t rgba(const uint8_t *p){return ((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static const uint8_t *at(const uint8_t *ram,unsigned x,unsigned y){return ram+QRX_GFX_FRAMEBUFFER+(size_t)y*QRX_GFX_PITCH+(size_t)x*4u;}
static void present(void *ctx,const uint8_t *p,uint32_t w,uint32_t h,uint32_t pitch){
    struct sink *s=ctx; assert(w==800&&h==450&&pitch==3200);s->color=rgba(p+50u*pitch+50u*4u);s->frames++;
}
static void be16(uint8_t *p,size_t off,uint16_t n){p[off]=(uint8_t)(n>>8);p[off+1]=(uint8_t)n;}
static void be32(uint8_t *p,size_t off,uint32_t n){be16(p,off,(uint16_t)(n>>16));be16(p,off+2,(uint16_t)n);}
static void vtx(uint8_t *ram,unsigned index,int32_t x,int32_t y,uint32_t z,uint32_t color,uint32_t u,uint32_t v){
    unsigned off=VTX+index*24;
    be32(ram,off,(uint32_t)(x*65536));be32(ram,off+4,(uint32_t)(y*65536));be32(ram,off+8,z);
    be32(ram,off+12,color);be32(ram,off+16,u);be32(ram,off+20,v);
}
static void write(QRXVM *v,uint32_t offset,uint32_t value){assert(qrx_vm_write(v,QRX_3D_BASE+offset,4,value));}
static uint32_t cmd(QRXVM *v,uint32_t opcode){uint32_t status=99;write(v,0x10,opcode);assert(qrx_vm_read(v,QRX_3D_BASE+0x0c,4,&status));return status;}
static void triangle(uint8_t *ram,uint32_t z,uint32_t rgb){
    vtx(ram,0,40,40,z,rgb,0,0);vtx(ram,1,140,40,z,rgb,65536,0);vtx(ram,2,40,140,z,rgb,0,65536);
}
static void emit(uint8_t *ram,size_t *pc,uint32_t addr,uint32_t val){
    be16(ram,*pc,0x207c);be32(ram,*pc+2,addr);*pc+=6;
    be16(ram,*pc,0x20bc);be32(ram,*pc+2,val);*pc+=6;
}
int main(void){
    uint8_t *ram=calloc(1,RAMSZ);QRXMediaDevice *media=calloc(1,sizeof(*media));
    assert(ram&&media);QRXVM vm;struct sink sink={0};
    qrx_vm_init(&vm,ram,RAMSZ);qrx_media_init(media,present,&sink);qrx_vm_bind_media(&vm,media);
    uint32_t v=0;assert(qrx_vm_read(&vm,QRX_3D_BASE,4,&v)&&v==QRX_3D_MAGIC);
    assert(qrx_vm_read(&vm,QRX_IO_BASE+8,4,&v)&&(v&QRX_MEDIA_FEATURE_3D));
    write(&vm,0x20,0x00001020);write(&vm,0x24,65535);assert(cmd(&vm,QRX_3D_CMD_CLEAR)==0);
    assert(rgba(at(ram,0,0))==0x001020);assert(media->glide_depth[0]==65535);
    triangle(ram,32768u,0xffff0000);
    write(&vm,0x20,VTX);write(&vm,0x24,QRX_3D_DEPTH_TEST|QRX_3D_DEPTH_WRITE);
    assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==0);
    assert(rgba(at(ram,50,50))==0xff0000);
    assert(rgba(at(ram,200,200))==0x001020);
    triangle(ram,49152u,0xff00ff00);
    assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==0); /* behind, occluded */
    assert(rgba(at(ram,50,50))==0xff0000);
    triangle(ram,16384u,0xff00ff00);
    assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==0); /* in front, green */
    assert(rgba(at(ram,50,50))==0x00ff00);
    triangle(ram,0u,0x80ff0000);
    write(&vm,0x24,QRX_3D_ALPHA_BLEND);
    assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==0);
    uint32_t c=rgba(at(ram,50,50));assert(((c>>16)&255)>100 && ((c>>8)&255)>100); /* mixed */
    /* ARGB8 texture, 2x2 nearest, untextured RGB ignored when texture flag set. */
    be32(ram,TEX,0xff3377aa);be32(ram,TEX+4,0xff3377aa);
    be32(ram,TEX+8,0xff3377aa);be32(ram,TEX+12,0xff3377aa);
    write(&vm,0x20,TEX);write(&vm,0x24,8);write(&vm,0x28,2);write(&vm,0x2c,2);
    assert(cmd(&vm,QRX_3D_CMD_TEXTURE)==0);
    write(&vm,0x20,VTX);write(&vm,0x24,QRX_3D_TEXTURE);
    assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==0);
    assert(rgba(at(ram,50,50))==0x3377aa);
    write(&vm,0x20,0xffffffffu);assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==QRX_3D_ERR_BOUNDS);
    write(&vm,0x20,QRX_GFX_FRAMEBUFFER);assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==QRX_3D_ERR_BOUNDS);
    write(&vm,0x20,VTX);write(&vm,0x24,0x80000000u);
    assert(cmd(&vm,QRX_3D_CMD_TRIANGLE)==QRX_3D_ERR_ARGUMENT);
    write(&vm,0x20,0xffffff00u);write(&vm,0x24,8);write(&vm,0x28,2);write(&vm,0x2c,2);
    assert(cmd(&vm,QRX_3D_CMD_TEXTURE)==QRX_3D_ERR_BOUNDS);
    write(&vm,0x20,0);write(&vm,0x28,0);write(&vm,0x2c,0);assert(cmd(&vm,QRX_3D_CMD_TEXTURE)==0);
    assert(cmd(&vm,QRX_3D_CMD_SWAP)==0 && sink.frames==1);
    assert(cmd(&vm,99)==QRX_3D_ERR_COMMAND);
    assert(!qrx_vm_write(&vm,QRX_3D_BASE+0x21,4,7));
    /* Actual emulated 68k writes v0.6 GPU command registers through CPU bus. */
    be32(ram,0,0x100000u);be32(ram,4,0x1000u);
    size_t pc=0x1000u;
    emit(ram,&pc,QRX_3D_BASE+0x20,0x00223344u);
    emit(ram,&pc,QRX_3D_BASE+0x24,65535u);
    emit(ram,&pc,QRX_3D_BASE+0x10,QRX_3D_CMD_CLEAR);
    emit(ram,&pc,QRX_3D_BASE+0x10,QRX_3D_CMD_SWAP);
    emit(ram,&pc,QRX_IO_BASE+0x1c,1);
    assert(qrx_vm_reset(&vm));assert(qrx_vm_run(&vm,80)==0);
    assert(sink.frames==2 && sink.color==0x223344);
    qrx_glide_shutdown(media);qrx_vm_destroy(&vm);free(media);free(ram);
    puts("QRX3D: clear, Gouraud triangle, depth, alpha, texture, bounds, 68k MMIO and swap PASS");
    return 0;
}
