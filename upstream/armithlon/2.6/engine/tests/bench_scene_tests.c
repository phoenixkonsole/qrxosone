/* SPDX-License-Identifier: MIT */
#include "qrx_bench.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
int main(void){
    QRXAccelFrame *f=qrx_bench_scene();assert(f);
    assert(f->count==QRX_BENCH_COMMANDS);
    assert(f->commands[0].kind==QRX_AC_CLEAR);
    for(unsigned i=1;i<f->count;i++)assert(f->commands[i].kind==QRX_AC_TRIANGLE);
    const size_t bytes=(size_t)QRX_BENCH_W*QRX_BENCH_H*4u;
    uint8_t *a=malloc(bytes),*b=malloc(bytes);assert(a&&b);
    assert(!qrx_bench_reference(f,a,bytes));
    assert(!qrx_bench_reference(f,b,bytes));
    assert(qrx_bench_hash(a,bytes)==qrx_bench_hash(b,bytes));
    assert(a[0]==0x11&&a[1]==0x18&&a[2]==0x27&&a[3]==255);
    unsigned painted=0;for(size_t i=0;i<bytes;i+=4)if(a[i]!=0x11||a[i+1]!=0x18||a[i+2]!=0x27)painted++;
    assert(painted>20000&&painted<220000);
    printf("QRXBENCH scene: %u commands / %u painted pixels / FNV64 %016llx\n",
           f->count,painted,(unsigned long long)qrx_bench_hash(a,bytes));
    free(a);free(b);qrx_accel_frame_release(f);return 0;
}
