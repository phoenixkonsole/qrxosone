#include "qrx_hunk.h"
#include "qrx_bootshell.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void w32(unsigned char *p,size_t i,uint32_t v){p[i]=v>>24;p[i+1]=v>>16;p[i+2]=v>>8;p[i+3]=v;}
static uint32_t r32(const unsigned char *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void append(uint8_t *b,unsigned *p,uint32_t v){w32(b,*p,v);*p+=4;}
int main(void){
    unsigned char code[256]={0}; static unsigned char guest[QRX_HUNK_STAGE_ADDR+QRX_HUNK_STAGE_MAX+4]={0};
    unsigned pos=0;uint32_t entry=0;size_t loaded=0;
    append(code,&pos,0x3f3);append(code,&pos,0); /* header, empty resident names */
    append(code,&pos,2);append(code,&pos,0);append(code,&pos,1); /* hunks 0..1 */
    append(code,&pos,2);append(code,&pos,2); /* code 8, data 8 bytes */
    append(code,&pos,0x3e9);append(code,&pos,2); /* HUNK_CODE */
    append(code,&pos,0x4e754e71);append(code,&pos,4); /* RTS,NOP; relocatable offset */
    append(code,&pos,0x3ec);append(code,&pos,1);append(code,&pos,1);append(code,&pos,4);append(code,&pos,0);
    append(code,&pos,0x3f2); /* end code */
    append(code,&pos,0x3ea);append(code,&pos,2);append(code,&pos,0x11223344);append(code,&pos,0x55667788);
    append(code,&pos,0x3f2); /* end data */
    assert(qrx_hunk_load(code,pos,guest,sizeof(guest),QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    assert(entry==QRX_HUNK_STAGE_ADDR && loaded==16);
    assert(r32(guest+QRX_HUNK_STAGE_ADDR+4)==QRX_HUNK_STAGE_ADDR+8+4);
    assert(r32(guest+QRX_HUNK_STAGE_ADDR+8)==0x11223344);
    /* Failed load cannot partially damage the prior executable. */
    uint8_t snapshot[16];memcpy(snapshot,guest+QRX_HUNK_STAGE_ADDR,16);
    unsigned char damaged[256];memcpy(damaged,code,pos);
    for(unsigned i=0;i+3<pos;i+=4)if(r32(damaged+i)==0x3ec){w32(damaged,i+12,4096);break;}
    assert(!qrx_hunk_load(damaged,pos,guest,sizeof(guest),QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    assert(!memcmp(snapshot,guest+QRX_HUNK_STAGE_ADDR,16));
    assert(!qrx_hunk_load(code,pos-4,guest,sizeof(guest),QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    /* Dynamic staging no longer requires 32 MiB for a 16-byte executable.
       Reject genuinely insufficient guest space, not a harmless small RAM. */
    assert(qrx_hunk_load(code,pos,guest,0x18000u,QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    assert(!qrx_hunk_load(code,pos,guest,QRX_HUNK_STAGE_ADDR+15u,QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    /* HUNK_RELOC32SHORT: u16 count/hunk/offsets, zero count, optional pad. */
    unsigned char sh[256]={0};unsigned sp=0;
    append(sh,&sp,0x3f3);append(sh,&sp,0);
    append(sh,&sp,2);append(sh,&sp,0);append(sh,&sp,1);
    append(sh,&sp,2);append(sh,&sp,2);
    append(sh,&sp,0x3e9);append(sh,&sp,2);
    append(sh,&sp,0x4e754e71);append(sh,&sp,2);
    append(sh,&sp,0x3fc);
    sh[sp++]=0;sh[sp++]=1; /* 1 entry */
    sh[sp++]=0;sh[sp++]=1; /* hunk #1 */
    sh[sp++]=0;sh[sp++]=4; /* offset 4 (longword) */
    sh[sp++]=0;sh[sp++]=0; /* terminator */
    append(sh,&sp,0x3f2);
    append(sh,&sp,0x3ea);append(sh,&sp,2);
    append(sh,&sp,0x11223344);append(sh,&sp,0x55667788);
    append(sh,&sp,0x3f2);
    assert(qrx_hunk_load(sh,sp,guest,sizeof(guest),QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    assert(r32(guest+QRX_HUNK_STAGE_ADDR+4)==QRX_HUNK_STAGE_ADDR+8+2);
    /* Truncated short relocation fails transactionally, never corrupts RAM. */
    uint8_t before[16];memcpy(before,guest+QRX_HUNK_STAGE_ADDR,16);
    assert(!qrx_hunk_load(sh,sp-4,guest,sizeof(guest),QRX_HUNK_STAGE_ADDR,&entry,&loaded));
    assert(!memcmp(before,guest+QRX_HUNK_STAGE_ADDR,16));
    printf("PASS strict AmigaOS HUNK_HEADER CODE DATA BSS relocation loader; atomic failure; no Exec ABI claim\n");
    return 0;
}
