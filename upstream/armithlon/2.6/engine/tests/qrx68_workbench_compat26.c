#define _POSIX_C_SOURCE 200809L
#include "qrx68_hle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Calls the very same negative LVO trap decoded after a real 68k JSR.
 * This tests ABI dispatch without pretending to be a complete HUNK app. */
static void wr16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void wr32(uint8_t *p,uint32_t v){wr16(p,(uint16_t)(v>>16));wr16(p+2,(uint16_t)v);}
static uint32_t rd32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int call(QRX68 *r,unsigned lib,unsigned lvo){
    QRXCPU *c=&r->vm.cpu;
    c->pc=QRX68_TRAP_BASE | (lib<<12) | (lvo/6u);
    c->a[7]=0x27000u;
    wr32(r->memory+0x27000u,0x28000u);
    int result=qrx68_boundary(r);
    if(result==1){assert(c->pc==0x28000u && c->a[7]==0x27004u);}
    return result;
}
static void set_newwindow(QRX68 *r,uint32_t p,int x,int y,int w,int h,uint32_t idcmp,uint32_t gadget){
    memset(r->memory+p,0,48);
    wr16(r->memory+p,(uint16_t)x);wr16(r->memory+p+2,(uint16_t)y);
    wr16(r->memory+p+4,(uint16_t)w);wr16(r->memory+p+6,(uint16_t)h);
    wr32(r->memory+p+10,idcmp);wr32(r->memory+p+18,gadget);
}
int main(void){
    QRX68 r;assert(qrx68_init(&r,QRX68_DEFAULT_RAM,NULL,NULL));
    assert(qrx68_enable_gui(&r));
    QRXCPU *c=&r.vm.cpu;
    strcpy((char*)r.memory+0x25080,"intuition.library");
    c->a[1]=0x25080;c->d[0]=0;
    assert(call(&r,1,552)==1 && c->d[0]==QRX68_INTUITION_BASE);
    strcpy((char*)r.memory+0x250a0,"graphics.library");
    c->a[1]=0x250a0;c->d[0]=0;
    assert(call(&r,1,552)==1 && c->d[0]==QRX68_GFX_BASE);
    c->d[0]=UINT32_MAX;assert(call(&r,1,330)==1);
    assert(c->d[0]==16u);
    const uint32_t mask=1u<<16;
    c->d[0]=mask;assert(call(&r,1,318)==2); /* genuine cooperative Wait */
    c->a[1]=r.task_ptr;c->d[0]=mask;assert(call(&r,1,324)==1);
    c->d[0]=mask;assert(call(&r,1,318)==1 && c->d[0]==mask);
    assert(!(r.signal_pending&mask));
    assert(rd32(r.memory+r.task_ptr+26)==0); /* tc_SigRecvd */
    c->d[0]=16;assert(call(&r,1,336)==1);
    c->a[1]=0;assert(call(&r,1,294)==1 && c->d[0]==r.task_ptr);
    assert(call(&r,1,120)==1 && r.defer_count==1);
    assert(call(&r,1,126)==1 && r.defer_count==0);

    assert(call(&r,1,666)==1);uint32_t p1=c->d[0];assert(p1);
    assert(call(&r,1,666)==1);uint32_t p2=c->d[0];assert(p2&&p2!=p1);
    const uint32_t message=0x25000u;
    wr32(r.memory+message+14,p2);wr16(r.memory+message+18,20);
    c->a[0]=p1;c->a[1]=message;assert(call(&r,1,366)==1);
    c->a[0]=p1;assert(call(&r,1,384)==1 && c->d[0]==message);
    c->a[0]=p1;assert(call(&r,1,372)==1 && c->d[0]==message);
    c->a[1]=message;assert(call(&r,1,378)==1);
    c->a[0]=p2;assert(call(&r,1,372)==1 && c->d[0]==message);
    c->a[1]=message;assert(call(&r,1,378)==1);
    c->a[0]=p1;assert(call(&r,1,672)==1);
    c->a[0]=p2;assert(call(&r,1,672)==1);

    const uint32_t gad=0x26000u;
    wr32(r.memory+gad,0);
    wr16(r.memory+gad+4,12);wr16(r.memory+gad+6,15);
    wr16(r.memory+gad+8,30);wr16(r.memory+gad+10,20);
    const uint32_t flags=QRX68_IDCMP_GADGETDOWN|QRX68_IDCMP_GADGETUP|
        QRX68_IDCMP_MOUSEBUTTONS|QRX68_IDCMP_CLOSEWINDOW|QRX68_IDCMP_MENUPICK;
    set_newwindow(&r,0x24000u,8,8,128,100,flags,gad);
    c->a[0]=0x24000u;assert(call(&r,4,204)==1);uint32_t win=c->d[0];assert(win);
    assert(qrx68_open_window_count(&r)==1);
    assert(rd32(r.memory+win+62)==gad); /* Window.FirstGadget */
    uint32_t port=r.windows[0].userport;assert(port);
    assert(qrx68_post_event(&r,0,QRX68_IDCMP_MOUSEBUTTONS,104,0,25,30));
    c->a[0]=port;assert(call(&r,1,372)==1);
    uint32_t im=c->d[0];assert(rd32(r.memory+im+20)==QRX68_IDCMP_GADGETDOWN);
    assert(rd32(r.memory+im+28)==gad);
    c->a[1]=im;assert(call(&r,1,378)==1);
    assert(qrx68_post_event(&r,0,QRX68_IDCMP_MOUSEBUTTONS,232,0,25,30));
    c->a[0]=port;assert(call(&r,1,372)==1);
    im=c->d[0];assert(rd32(r.memory+im+20)==QRX68_IDCMP_GADGETUP);
    assert(rd32(r.memory+im+28)==gad);
    c->a[1]=im;assert(call(&r,1,378)==1);
    assert(r.gadget_picks==1);
    c->a[0]=gad;c->a[1]=win;c->a[2]=0;
    assert(call(&r,4,174)==1 && (r.memory[gad+12]&2u)); /* disabled bit 0x200 */
    assert(qrx68_post_event(&r,0,QRX68_IDCMP_MOUSEBUTTONS,104,0,25,30));
    c->a[0]=port;assert(call(&r,1,372)==1);
    im=c->d[0];assert(rd32(r.memory+im+20)==QRX68_IDCMP_MOUSEBUTTONS);
    c->a[1]=im;assert(call(&r,1,378)==1);
    c->a[0]=gad;c->a[1]=win;c->a[2]=0;
    assert(call(&r,4,186)==1 && !(r.memory[gad+12]&2u));

    const uint32_t menu=0x26500u,item=0x26600u;
    memset(r.memory+menu,0,32);memset(r.memory+item,0,34);
    wr16(r.memory+menu+4,0);wr16(r.memory+menu+8,55);
    wr32(r.memory+menu+18,item);
    c->a[0]=win;c->a[1]=menu;assert(call(&r,4,264)==1);
    assert(rd32(r.memory+win+28)==menu); /* Window.MenuStrip */
    /* Right mouse opens the virtual menu. Release over the first item. */
    assert(qrx68_post_event(&r,0,QRX68_IDCMP_MOUSEBUTTONS,105,0,15,12));
    assert(qrx68_post_event(&r,0,QRX68_IDCMP_MOUSEBUTTONS,233,0,15,25));
    c->a[0]=port;assert(call(&r,1,372)==1);
    im=c->d[0];assert(rd32(r.memory+im+20)==QRX68_IDCMP_MENUPICK);
    assert(rd32(r.memory+im+24)==0); /* menu 0 / item 0 */
    c->a[1]=im;assert(call(&r,1,378)==1);
    assert(r.menu_picks==1);
    c->a[0]=win;assert(call(&r,4,54)==1);

    /* Per-window opaque surfaces and primitive z-order: uncovered pixels
       must reappear after the upper window closes. */
    uint32_t rp=r.windows[0].rastport;
    c->a[1]=rp;c->d[0]=4;assert(call(&r,3,342)==1);
    c->a[1]=rp;c->d[0]=20;c->d[1]=20;c->d[2]=30;c->d[3]=30;
    assert(call(&r,3,306)==1);
    assert(r.framebuffer[(8+22)*QRX68_FB_WIDTH+(8+22)]==4);
    set_newwindow(&r,0x24100u,20,20,64,60,QRX68_IDCMP_CLOSEWINDOW,0);
    c->a[0]=0x24100u;assert(call(&r,4,204)==1);uint32_t win2=c->d[0];
    uint32_t rp2=r.windows[1].rastport;
    c->a[1]=rp2;c->d[0]=2;assert(call(&r,3,342)==1);
    c->a[1]=rp2;c->d[0]=0;c->d[1]=0;c->d[2]=50;c->d[3]=50;
    assert(call(&r,3,306)==1);
    assert(r.framebuffer[30*QRX68_FB_WIDTH+30]==2);
    c->a[0]=win2;assert(call(&r,4,72)==1);
    assert(r.framebuffer[30*QRX68_FB_WIDTH+30]==4);
    c->a[0]=win;c->d[0]=8;c->d[1]=8;c->d[2]=140;c->d[3]=112;
    assert(call(&r,4,486)==1);
    assert(r.windows[0].width==140 && rd32(r.memory+win+62)==gad);
    assert(r.framebuffer[30*QRX68_FB_WIDTH+30]==4);
    c->a[0]=win;assert(call(&r,4,72)==1);
    assert(qrx68_open_window_count(&r)==0);

    /* Modern V36+ WA_* TagItem based window opening. */
    const uint32_t tags=0x26800u;
    wr32(r.memory+tags+0,UINT32_C(0x80000064)); /* WA_Left */
    wr32(r.memory+tags+4,8);
    wr32(r.memory+tags+8,UINT32_C(0x80000065)); /* WA_Top */
    wr32(r.memory+tags+12,8);
    wr32(r.memory+tags+16,UINT32_C(0x80000066)); /* WA_Width */
    wr32(r.memory+tags+20,60);
    wr32(r.memory+tags+24,UINT32_C(0x80000067)); /* WA_Height */
    wr32(r.memory+tags+28,50);
    wr32(r.memory+tags+32,0);wr32(r.memory+tags+36,0);
    c->a[0]=0;c->a[1]=tags;
    assert(call(&r,4,606)==1 && c->d[0]);
    assert(r.windows[0].width==60 && r.windows[0].height==50);
    c->a[0]=c->d[0];assert(call(&r,4,72)==1);

    /* CLI/Process metadata works; real CreateProc remains unsupported. */
    strcpy((char*)r.memory+0x25080,"dos.library");
    c->a[1]=0x25080;c->d[0]=0;
    assert(call(&r,1,552)==1 && c->d[0]==QRX68_DOS_BASE);
    memcpy(r.memory+0x25000,"MyProgram",10);
    c->d[1]=0x25000u;assert(call(&r,2,570)==1 && c->d[0]==1);
    c->d[1]=0x25100u;c->d[2]=64;
    assert(call(&r,2,576)==1 && !strcmp((char*)r.memory+0x25100,"MyProgram"));
    c->d[1]=0x25180u;c->d[2]=64;
    assert(call(&r,2,564)==1 && !strcmp((char*)r.memory+0x25180,"HOST:"));
    assert(call(&r,2,492)==1 && c->d[0]==0); /* bare HUNK has no CLI */
    assert(call(&r,2,138)==-1); /* CreateProc unsupported: fail closed */
    assert(r.unsupported==1 && r.failed_lib==2 && r.failed_lvo==138);
    qrx68_destroy(&r);

    /* dos.library/Exit is a non-returning HLE call, not a fake RTS. */
    assert(qrx68_init(&r,QRX68_DEFAULT_RAM,NULL,NULL));
    r.dos_open_count=1;c=&r.vm.cpu;c->d[1]=5;
    assert(call(&r,2,144)==-1 && r.complete && !r.failed);
    qrx68_destroy(&r);
    puts("PASS QRX68 Exec signals/message-port/reply, Intuition gadget/menu, layered framebuffer, invalid DOS access fail closed");
    return 0;
}
