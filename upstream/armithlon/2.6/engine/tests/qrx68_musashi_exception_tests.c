/* REAL-MUSASHI-only test, registered when pinned Musashi is enabled.
 * Verifies actual illegal-instruction exception -> 040 supervisor stack ->
 * RTE back to user mode -> RTS to QRX68 return sentinel. NOT a mock.
 */
#include "qrx68_hle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void out(void *ctx,uint8_t ch){(void)ctx;(void)ch;}
static void put32(unsigned char *p,unsigned n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
int main(void){
    QRX68 r;
    unsigned char hunk[48]={0};
    /* One HUNK_CODE, 2 longwords: ILLEGAL;RTS;MOVEQ #7,D0;RTE */
    const unsigned hdr[]={0x3f3,0,1,0,0,2,0x3e9,2};
    for(unsigned i=0;i<8;i++)put32(hunk+4*i,hdr[i]);
    const unsigned char code[]={0x4a,0xfc,0x4e,0x75,0x70,0x07,0x4e,0x73};
    memcpy(hunk+32,code,8);put32(hunk+40,0x3f2);
    assert(qrx68_init(&r,2u*1024u*1024u,out,NULL));
    assert(qrx68_load_hunk(&r,hunk,44));
    put32(r.memory+16,0x10004); /* vector #4 = ILLEGAL handler at entry+4 */
    int rc=qrx68_run_musashi(&r,3000);
    assert(rc==0);
    assert(r.complete && !r.failed && r.unsupported==0);
    assert(r.vm.cpu.d[0]==7u);
    assert((r.vm.cpu.sr&0x2000u)==0); /* user mode after RTE */
    qrx68_destroy(&r);
    puts("PASS: real Musashi 68040 illegal exception, supervisor stack and RTE");
    return 0;
}
