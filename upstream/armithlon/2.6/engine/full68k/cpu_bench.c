/* Runs a bare-metal HUNK program to its shutdown write and reports throughput.
 * tools/armithlon/bench68k/build.sh makes the workload. No guest OS involved. */
#include "qrx68100_full.h"
#include "qrx68100_jit.h"
#include "qrx_hunk.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+(double)t.tv_nsec*1e-9;}
int main(int argc,char **argv){
    if(argc<2){fprintf(stderr,"usage: qrx68100_cpu_bench program.hunk [--interp]\n");return 2;}
    bool interp=argc>2&&!strcmp(argv[2],"--interp");
    FILE *f=fopen(argv[1],"rb");if(!f){perror(argv[1]);return 2;}
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    uint8_t *file=malloc((size_t)n);
    if(!file||fread(file,1,(size_t)n,f)!=(size_t)n){fclose(f);return 2;}
    fclose(f);
    QRXFullCPU c;
    if(!qrx_full_init(&c,32u*1024u*1024u,68040))return 2;
    uint32_t entry=0;size_t loaded=0;
    if(!qrx_hunk_load(file,(size_t)n,c.ram_owned,c.vm.ram.size,QRX_HUNK_STAGE_ADDR,&entry,&loaded)){fprintf(stderr,"bad HUNK\n");return 2;}
    qrx_full_bus_write(&c,0,4,0x00100000u);
    qrx_full_bus_write(&c,4,4,entry);
    if(!qrx_full_reset(&c))return 2;
    if(!interp&&!qrx_jit_init(&c,0)){fprintf(stderr,"JIT unavailable\n");return 2;}
    double t0=now();
    int rc=1;
    while(rc==1)rc=qrx_full_run_cycles(&c,2000000u);
    double dt=now()-t0;
    uint32_t result=0;qrx_full_bus_read(&c,0x8000u,4,&result);
    printf("%s result=%08x seconds=%.3f instructions=%llu mips=%.1f rc=%d\n",interp?"interp":"jit",result,dt,
        (unsigned long long)c.instructions,(double)c.instructions/dt/1e6,rc);
    qrx_jit_report(&c);
    qrx_jit_destroy(&c);
    qrx_full_destroy(&c);
    free(file);
    return rc==0?0:1;
}
