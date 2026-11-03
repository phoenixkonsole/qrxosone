/* QRX 1.7 microbenchmark: host-side interface only, NOT Apollo/68040 clock
 * performance, not Musashi 68k ISA execution, not a guest CPU MIPS score. */
#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx68100_vampire.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double seconds(void){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0.0;
    return (double)t.tv_sec+(double)t.tv_nsec*1e-9;
}
static void wr32(unsigned char*p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(int argc,char**argv){
    unsigned long rounds=500000;
    if(argc>1){char*end;rounds=strtoul(argv[1],&end,10);if(*end||rounds<1000||rounds>50000000ul)return 2;}
    QRXFullCPU b;QRXDual d;
    if(!qrx_full_init(&b,65536,68040))return 3;
    wr32(b.ram_owned,0x8000);wr32(b.ram_owned+4,0x100);
    if(!qrx_full_reset(&b)||!qrx_dual_init(&d,&b)||!qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL))return 4;
    qrx_vamp_vector_write(&d,0,0,UINT64_C(0x1020304050607080));
    qrx_vamp_vector_write(&d,0,1,UINT64_C(0x0101010101010101));
    double start=seconds();
    for(unsigned long i=0;i<rounds;i++){
        if(qrx_vamp_vector_execute(&d,0,0,1,0,QRX_VAMP_CMD_PADDB)!=QRX_VAMP_STATUS_OK)return 5;
    }
    double dt=seconds()-start;
    uint64_t answer=qrx_vamp_vector(&d,0,0);
    /* Do not claim any speedup or 68080 equivalence from these numbers. */
    printf("{\"system\":\"QRX68100-v1.7\",\"kind\":\"native_host_vector_api_MOCK_CPU_NOT_68k_guest\",\"runs\":%lu,\"seconds\":%.8f,\"ops_per_second\":%.3f,\"last_vector_hex\":\"%016" PRIx64 "\",\"comparison_to_Vampire\":null}\n",
        rounds,dt,dt>0?(double)rounds/dt:0.,answer);
    qrx_dual_destroy(&d);qrx_full_destroy(&b);
    return 0;
}
