#include "qrx68_hle.h"
#include "qrx_hunk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SZ (96u*1024u)
static void w32(unsigned char *p,unsigned x){p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static void out(void *p,uint8_t c){(void)p;(void)c;}
int main(void){
    size_t n=32u+SZ+4u;
    unsigned char *h=calloc(1,n);assert(h);
    unsigned hdr[]={0x3f3,0,1,0,0,SZ/4u,0x3e9,SZ/4u};
    for(unsigned i=0;i<8;i++)w32(h+i*4,hdr[i]);
    h[32]=0x4e;h[33]=0x75;/* RTS */
    w32(h+32+SZ,0x3f2);
    QRX68 r;
    assert(qrx68_init(&r,4u*1024u*1024u,out,NULL));
    assert(qrx68_load_hunk(&r,h,n));
    assert(r.heap_cursor>=QRX_HUNK_STAGE_ADDR+SZ);
    assert(r.heap_cursor < r.heap_limit);
    assert(qrx68_run(&r,100)==0);
    qrx68_destroy(&r);free(h);
    puts("PASS 96KiB HUNK (former 64KiB cap removed), aligned heap beyond program");
    return 0;
}
