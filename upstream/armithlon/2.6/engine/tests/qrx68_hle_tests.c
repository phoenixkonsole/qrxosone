#define _POSIX_C_SOURCE 200809L
#include "qrx68_hle.h"
#include "qrx_hunk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A REAL AmigaDOS HUNK assembled as raw 68k opcodes. Uses SysBase at 4.w,
   JSR -552(A6) OpenLibrary, DOS Write -48, and RTS. No host-side fake jump. */
typedef struct { uint8_t bytes[2048];size_t pos; } Bin;
static void u16(Bin *b,uint16_t v){b->bytes[b->pos++]=v>>8;b->bytes[b->pos++]=v;}
static void u32(Bin *b,uint32_t v){u16(b,v>>16);u16(b,v);}
static void append(uint8_t *a,unsigned *pos,uint32_t v){
    a[(*pos)++]=(uint8_t)(v>>24);a[(*pos)++]=(uint8_t)(v>>16);
    a[(*pos)++]=(uint8_t)(v>>8);a[(*pos)++]=(uint8_t)v;
}
static void patch32(Bin *b,size_t at,uint32_t v){
    b->bytes[at]=v>>24;b->bytes[at+1]=v>>16;b->bytes[at+2]=v>>8;b->bytes[at+3]=v;
}
static void data(Bin *b,const char *s){size_t n=strlen(s)+1;memcpy(b->bytes+b->pos,s,n);b->pos+=n;}
static void align4(Bin *b){while(b->pos&3)b->bytes[b->pos++]=0;}
static size_t make_hunk(uint8_t *dst,size_t cap,bool file_mode,bool unsupported,bool too_new){
    Bin c={.pos=0};
    u16(&c,0x2c78);u16(&c,4); /* MOVEA.L 4.W,A6 (ExecBase) */
    u16(&c,0x43f9);size_t libptr=c.pos;u32(&c,0); /* LEA dos.library,A1 */
    u16(&c,(uint16_t)(too_new?0x7032:0x7000)); /* MOVEQ #minver,D0 */
    u16(&c,0x4eae);u16(&c,(uint16_t)-552); /* JSR OpenLibrary */
    if(too_new){u16(&c,0x4e75);}
    else{
        u16(&c,0x2c40); /* MOVEA.L D0,A6 */
        if(file_mode){
            u16(&c,0x223c);size_t pathptr=c.pos;u32(&c,0); /* MOVE.L #path,D1 */
            u16(&c,0x243c);u32(&c,1005); /* MOVE.L #MODE_OLDFILE,D2 */
            u16(&c,0x4eae);u16(&c,(uint16_t)-30); /* Open */
            u16(&c,0x2200); /* MOVE.L D0,D1 */
            u16(&c,0x243c);size_t bufptr=c.pos;u32(&c,0);
            u16(&c,0x7607); /* MOVEQ #7,D3 */
            u16(&c,0x4eae);u16(&c,(uint16_t)-42); /* Read */
            u16(&c,0x4eae);u16(&c,(uint16_t)-60); /* Output() */
            u16(&c,0x2200); /* MOVE.L D0,D1 */
            u16(&c,0x243c);size_t bufout=c.pos;u32(&c,0);
            u16(&c,0x7607);
            u16(&c,0x4eae);u16(&c,(uint16_t)-48); /* Write */
            u16(&c,0x4e75); /* RTS */
            size_t liboff=c.pos;data(&c,"dos.library");
            size_t pathoff=c.pos;data(&c,"HOST:note.txt");
            align4(&c);size_t bufoff=c.pos;
            for(int i=0;i<8;i++)c.bytes[c.pos++]=0;
            patch32(&c,libptr,QRX_HUNK_STAGE_ADDR+(uint32_t)liboff);
            patch32(&c,pathptr,QRX_HUNK_STAGE_ADDR+(uint32_t)pathoff);
            patch32(&c,bufptr,QRX_HUNK_STAGE_ADDR+(uint32_t)bufoff);
            patch32(&c,bufout,QRX_HUNK_STAGE_ADDR+(uint32_t)bufoff);
        }else if(unsupported){
            u16(&c,0x4eae);u16(&c,(uint16_t)-606); /* unsupported DOS API */
            u16(&c,0x4e75);
            size_t liboff=c.pos;data(&c,"dos.library");
            patch32(&c,libptr,QRX_HUNK_STAGE_ADDR+(uint32_t)liboff);
        }else{
            u16(&c,0x4eae);u16(&c,(uint16_t)-60); /* Output() */
            u16(&c,0x2200); /* MOVE.L D0,D1 */
            u16(&c,0x243c);size_t msgptr=c.pos;u32(&c,0); /* MOVE.L #message,D2 */
            u16(&c,0x760c); /* MOVEQ #12,D3 */
            u16(&c,0x4eae);u16(&c,(uint16_t)-48); /* Write */
            u16(&c,0x4e75);
            size_t liboff=c.pos;data(&c,"dos.library");
            size_t msgoff=c.pos;data(&c,"Hello QRX68\n");
            patch32(&c,libptr,QRX_HUNK_STAGE_ADDR+(uint32_t)liboff);
            patch32(&c,msgptr,QRX_HUNK_STAGE_ADDR+(uint32_t)msgoff);
        }
    }
    if(too_new){size_t liboff=c.pos;data(&c,"dos.library");patch32(&c,libptr,QRX_HUNK_STAGE_ADDR+(uint32_t)liboff);}
    align4(&c);
    Bin f={.pos=0};
    u32(&f,0x3f3);u32(&f,0);u32(&f,1);u32(&f,0);u32(&f,0);
    u32(&f,(uint32_t)c.pos/4);
    u32(&f,0x3e9);u32(&f,(uint32_t)c.pos/4);
    memcpy(f.bytes+f.pos,c.bytes,c.pos);f.pos+=c.pos;
    u32(&f,0x3f2);
    assert(f.pos<=cap);memcpy(dst,f.bytes,f.pos);return f.pos;
}
typedef struct {char b[128];size_t n;} Capture;
static void cb(void *context,uint8_t c){Capture *o=context;if(o->n<sizeof(o->b)-1)o->b[o->n++]=(char)c;}
int main(void){
    QRX68 q;Capture output={0};uint8_t hunk[2048];size_t n;
    assert(qrx68_init(&q,QRX68_DEFAULT_RAM,cb,&output));
    assert(q.memory[4]==0 && q.memory[6]==0x40);
    assert(q.memory[QRX68_EXEC_BASE-552]==0x4e && q.memory[QRX68_EXEC_BASE-551]==0xf9);
    n=make_hunk(hunk,sizeof(hunk),false,false,false);
    assert(qrx68_load_hunk(&q,hunk,n));
    assert(qrx68_run(&q,1000)==0);
    assert(!strcmp(output.b,"Hello QRX68\n"));
    assert(q.dos_open_count==1 && q.hle_calls==3 && !q.unsupported);
    qrx68_destroy(&q);

    /* The host file must go through the DOS ABI and explicit read-only share. */
    char dir[]="/tmp/qrx68-shared-XXXXXX";assert(mkdtemp(dir));
    char path[320];snprintf(path,sizeof(path),"%s/note.txt",dir);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite("AMIGA!\n",1,7,f)==7);fclose(f);
    assert(qrx68_init(&q,QRX68_DEFAULT_RAM,cb,&output));
    memset(&output,0,sizeof(output));
    assert(qrx68_share(&q,dir,false));
    n=make_hunk(hunk,sizeof(hunk),true,false,false);
    assert(qrx68_load_hunk(&q,hunk,n));
    assert(qrx68_run(&q,2000)==0);
    assert(!strcmp(output.b,"AMIGA!\n"));
    qrx68_destroy(&q);

    /* DOS library opened but unimplemented LVO => hard error, no fake output. */
    assert(qrx68_init(&q,QRX68_DEFAULT_RAM,cb,&output));
    n=make_hunk(hunk,sizeof(hunk),false,true,false);
    assert(qrx68_load_hunk(&q,hunk,n));
    assert(qrx68_run(&q,1000)==-1);
    assert(q.unsupported==1 && q.failed_lib==2 && q.failed_lvo==606);
    qrx68_destroy(&q);

    assert(qrx68_init(&q,QRX68_DEFAULT_RAM,cb,&output));
    n=make_hunk(hunk,sizeof(hunk),false,false,true);
    assert(qrx68_load_hunk(&q,hunk,n));
    assert(qrx68_run(&q,1000)==0);
    assert(q.vm.cpu.d[0]==0); /* version 50 requested against v40 */
    qrx68_destroy(&q);

    /* Real 68k API test: AllocVec(32), FreeVec(ptr), return ptr in D0. */
    {
        uint8_t a[256]={0};unsigned ap=0;
        /* Header / segment size 20 bytes (10 words). */
        append(a,&ap,0x3f3);append(a,&ap,0);
        append(a,&ap,1);append(a,&ap,0);append(a,&ap,0);
        append(a,&ap,5);append(a,&ap,0x3e9);append(a,&ap,5);
        /* MOVEA.L 4.W,A6; MOVEQ #32,D0; MOVEQ #0,D1 */
        a[ap++]=0x2c;a[ap++]=0x78;a[ap++]=0;a[ap++]=4;
        a[ap++]=0x70;a[ap++]=0x20;a[ap++]=0x72;a[ap++]=0;
        /* JSR -684(A6); MOVEA.L D0,A1; JSR -690(A6); RTS */
        a[ap++]=0x4e;a[ap++]=0xae;a[ap++]=0xfd;a[ap++]=0x54;
        a[ap++]=0x22;a[ap++]=0x40;
        a[ap++]=0x4e;a[ap++]=0xae;a[ap++]=0xfd;a[ap++]=0x4e;
        a[ap++]=0x4e;a[ap++]=0x75;
        /* Padding word required: actual code above is 20 bytes? 21? */
        assert(ap == 32u+20u);
        append(a,&ap,0x3f2);
        assert(qrx68_init(&q,QRX68_DEFAULT_RAM,cb,&output));
        assert(qrx68_load_hunk(&q,a,ap));
        assert(qrx68_run(&q,1000)==0);
        assert(q.hle_calls==2 && q.allocations[0].in_use==0);
        qrx68_destroy(&q);
    }
    /* Non-HUNK inputs never accidentally execute host code. */
    assert(qrx68_init(&q,QRX68_DEFAULT_RAM,cb,&output));
    assert(!qrx68_load_hunk(&q,(const uint8_t*)"MZ",2));
    qrx68_destroy(&q);
    unlink(path);rmdir(dir);
    puts("PASS: REAL 68k HUNK guest -> Exec OpenLibrary vector -> DOS Write/Read via QRX68 HLE; bounded, unsupported fail");
    return 0;
}
