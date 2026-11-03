#include "qrx_demo.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DEMO_RAM_SIZE (8u*1024u*1024u)
#define DEMO_PC 0x1000u
#define DEMO_PCM 0x00180000u
#define DEMO_VERTEX 0x001a0000u
struct QRXDemo {
    QRXVM vm;
    QRXMediaDevice *media;
    uint8_t *ram;
    uint32_t frames;
    uint32_t last_input;
    float pitch;
    double phase;
};
static void be16(uint8_t *p,uint32_t addr,uint16_t v) {
    p[addr]=(uint8_t)(v>>8);p[addr+1]=(uint8_t)v;
}
static void be32(uint8_t *p,uint32_t addr,uint32_t v) {
    be16(p,addr,(uint16_t)(v>>16));be16(p,addr+2,(uint16_t)v);
}
QRXDemo *qrx_demo_create(void (*present)(void *,const uint8_t *,uint32_t,uint32_t,uint32_t),void *ctx) {
    QRXDemo *d=calloc(1,sizeof(*d));
    if(!d)return NULL;
    d->ram=calloc(DEMO_RAM_SIZE,1);
    d->media=calloc(1,sizeof(*d->media));
    if(!d->ram||!d->media){qrx_demo_destroy(d);return NULL;}
    qrx_vm_init(&d->vm,d->ram,DEMO_RAM_SIZE);
    qrx_media_init(d->media,present,ctx);
    qrx_vm_bind_media(&d->vm,d->media);
    d->pitch=440.f;
    be32(d->ram,0,0x00100000);
    be32(d->ram,4,DEMO_PC);
    return d;
}
void qrx_demo_destroy(QRXDemo *d) {
    if(!d)return;
    qrx_vm_destroy(&d->vm);
    qrx_glide_shutdown(d->media);free(d->media);free(d->ram);free(d);
}
QRXMediaDevice *qrx_demo_media(QRXDemo *d){return d?d->media:NULL;}
uint32_t qrx_demo_frames(QRXDemo *d){return d?d->frames:0;}
uint32_t qrx_demo_last_input(QRXDemo *d){return d?d->last_input:0;}
/* Assemble a real Motorola 68k MMIO program in guest RAM. Each command is
 * MOVE.L #reg,A0 / MOVE.L #value,(A0), handled by QRX68100's instruction
 * decoder. The native host never directly paints any demo pixels. */
typedef struct {uint8_t *ram;uint32_t pc;bool good;} Program;
static void emit(Program *p,uint32_t reg,uint32_t val){
    if(p->pc+12u>0x3000u){p->good=false;return;}
    be16(p->ram,p->pc,0x207c);be32(p->ram,p->pc+2,reg);p->pc+=6;
    be16(p->ram,p->pc,0x20bc);be32(p->ram,p->pc+2,val);p->pc+=6;
}
static void clear(Program *p,uint32_t color){
    emit(p,QRX_GPU_BASE+0x20,color);
    emit(p,QRX_GPU_BASE+0x10,QRX_GPU_CMD_CLEAR);
}
static void rect(Program *p,int x,int y,int w,int h,uint32_t color){
    if(x<0){w+=x;x=0;}
    if(y<0){h+=y;y=0;}
    if(x>=(int)QRX_GFX_WIDTH||y>=(int)QRX_GFX_HEIGHT||w<=0||h<=0)return;
    if(x+w>(int)QRX_GFX_WIDTH)w=(int)QRX_GFX_WIDTH-x;
    if(y+h>(int)QRX_GFX_HEIGHT)h=(int)QRX_GFX_HEIGHT-y;
    emit(p,QRX_GPU_BASE+0x20,((uint32_t)x<<16)|(uint32_t)y);
    emit(p,QRX_GPU_BASE+0x24,((uint32_t)w<<16)|(uint32_t)h);
    emit(p,QRX_GPU_BASE+0x28,color);
    emit(p,QRX_GPU_BASE+0x10,QRX_GPU_CMD_FILL);
}
bool qrx_demo_frame(QRXDemo *d) {
    if(!d)return false;
    uint32_t ev=0,count=0,mx=0,my=0,btn=0;
    qrx_vm_read(&d->vm,QRX_MEDIA_INPUT_BASE,4,&count);
    for(uint32_t i=0;i<count && i<QRX_INPUT_QUEUE_SIZE;i++){
        qrx_vm_read(&d->vm,QRX_MEDIA_INPUT_BASE+4,4,&ev);
        d->last_input=ev;
        if((ev>>24)==QRX_INPUT_KEY_DOWN)d->pitch=220.f+(float)((ev>>16)&0xff)*18.f;
    }
    qrx_vm_read(&d->vm,QRX_MEDIA_INPUT_BASE+8,4,&mx);
    qrx_vm_read(&d->vm,QRX_MEDIA_INPUT_BASE+12,4,&my);
    qrx_vm_read(&d->vm,QRX_MEDIA_INPUT_BASE+16,4,&btn);

    /* 16.666 ms of sample data, PCM stereo big-endian in guest RAM. The
     * processor writes the DMA command below. AHI integration is pending. */
    for(unsigned i=0;i<800;i++){
        double w=sin(d->phase)*0.10;
        double second=sin(d->phase*1.5)*0.04;
        int16_t l=(int16_t)((w+second)*32767.0),r=(int16_t)((w-second)*32767.0);
        uint32_t frame=((uint32_t)(uint16_t)l<<16)|(uint16_t)r;
        be32(d->ram,DEMO_PCM+i*4u,frame);
        d->phase+=6.2831853071795864769*(double)d->pitch/(double)QRX_AUDIO_RATE;
        if(d->phase>6.2831853071795864769)d->phase-=6.2831853071795864769;
    }
    Program p={d->ram,DEMO_PC,true};
    clear(&p,0x00132349u);
    rect(&p,0,0,QRX_GFX_WIDTH,6,0x0037b3eeu);
    rect(&p,0,QRX_GFX_HEIGHT-6,QRX_GFX_WIDTH,6,0x0037b3eeu);
    for(unsigned i=0;i<13;i++) {
        int yy=105+(int)(60.0*sin((double)d->frames*0.06+i*0.48));
        rect(&p,50+(int)i*53,yy,14,140,0x002790edu);
    }
    /* v0.6: guest RAM contains a 3D triangle in the QRX3D BE vertex format;
     * real emulated 68k opcodes submit the command via QRX3D MMIO. */
    int tx=210+(int)(55.0*sin((double)d->frames*0.035));
    int ty=145;
    const int vx[3]={tx,tx+190,tx+80},vy[3]={ty,ty+20,ty+170};
    const uint32_t colors[3]={0xffff5544u,0xff44ffaa,0xff498dff};
    for(unsigned j=0;j<3;j++){
        uint32_t addr=DEMO_VERTEX+j*24u;
        be32(d->ram,addr,(uint32_t)(vx[j]*65536));
        be32(d->ram,addr+4,(uint32_t)(vy[j]*65536));
        be32(d->ram,addr+8,32768u);
        be32(d->ram,addr+12,colors[j]);
        be32(d->ram,addr+16,(j==1)?65536u:0u);
        be32(d->ram,addr+20,(j==2)?65536u:0u);
    }
    emit(&p,QRX_3D_BASE+0x20,DEMO_VERTEX);
    emit(&p,QRX_3D_BASE+0x24,0);
    emit(&p,QRX_3D_BASE+0x10,QRX_3D_CMD_TRIANGLE);
    int cx=(int)mx,cy=(int)my;
    if(cx<0||cx>=(int)QRX_GFX_WIDTH)cx=(int)QRX_GFX_WIDTH/2;
    if(cy<0||cy>=(int)QRX_GFX_HEIGHT)cy=(int)QRX_GFX_HEIGHT/2;
    rect(&p,cx-14,cy-1,29,3,btn?0x00ff668bu:0x006cdc8bu);
    rect(&p,cx-1,cy-14,3,29,btn?0x00ff668bu:0x006cdc8bu);
    emit(&p,QRX_MEDIA_AUDIO_BASE+0x14,DEMO_PCM);
    emit(&p,QRX_MEDIA_AUDIO_BASE+0x18,800);
    emit(&p,QRX_MEDIA_AUDIO_BASE+0x1c,1);
    emit(&p,QRX_GPU_BASE+0x10,QRX_GPU_CMD_PRESENT);
    emit(&p,QRX_IO_BASE+0x1c,1);
    if(!p.good || !qrx_vm_reset(&d->vm))return false;
    if(qrx_vm_run(&d->vm,400)!=0 || d->vm.cpu.fault)return false;
    if(d->media->gpu_error!=QRX_GPU_ERR_NONE||d->media->glide_error!=QRX_3D_OK||d->media->audio_dma_error)return false;
    d->frames++;
    return true;
}
