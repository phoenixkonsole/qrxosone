/* Actual Musashi context bootstrap, then two parallel host-worker loops.
 * Measures wall execution time of one whitelisted translation workload.
 * Does not measure real 68040 frequency, arbitrary 68k ISA or guest OS FPS.
 */
#include "qrx68100_parallel.h"
#include "m68k.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
static void w16(unsigned char*p,unsigned o,unsigned v){p[o]=v>>8;p[o+1]=v;}
static void w32(unsigned char*p,unsigned o,unsigned v){w16(p,o,v>>16);w16(p,o+2,v);}
static double seconds_now(void){
  struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;
  return (double)t.tv_sec+1e-9*(double)t.tv_nsec;
}
int main(int argc,char**argv){
  uint32_t n=10000000u;
  if(argc>1){char*end=NULL;unsigned long v=strtoul(argv[1],&end,10);
    if(!end||*end||v<1000u||v>200000000u){fprintf(stderr,"iterations 1000..200000000\n");return 2;}n=(uint32_t)v;
  }
  QRXFullCPU cpu;QRXDual d;
  if(!qrx_full_init(&cpu,65536,68040))return 2;
  w32(cpu.ram_owned,0,0x8000);w32(cpu.ram_owned,4,0x100);
  w16(cpu.ram_owned,0x100,0x5280);w16(cpu.ram_owned,0x102,0x60fc);
  w16(cpu.ram_owned,0x200,0x5281);w16(cpu.ram_owned,0x202,0x60fc);
  if(!qrx_full_reset(&cpu)||!qrx_dual_init(&d,&cpu) ||
     !qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL) ||
     !qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_PC,4,0x200) ||
     !qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_SP,4,0x9000) ||
     !qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_CTL,4,1) ||
     !qrx_dual_commit(&d))return 2;
  double start=seconds_now();
  int mode=qrx_dual_parallel_loops(&d,n);
  double stop=seconds_now();
  uint32_t a=qrx_dual_reg(&d,0,M68K_REG_D0),b=qrx_dual_reg(&d,1,M68K_REG_D1);
  int ok=(mode>0 && a==n && b==n && stop>=start);
  printf("{\"test\":\"qrx68100_dual_addq_bra\",\"status\":\"%s\","
         "\"backend\":\"%s\",\"cores\":2,\"iterations_per_core\":%u,"
         "\"decoded_guest_instructions\":%llu,\"wall_seconds\":%.9f,"
         "\"instructions_per_wall_second\":%.3f,\"core0_d0\":%u,\"core1_d1\":%u,"
         "\"qualifier\":\"whitelisted isolated loop only, no emulator clock claim\"}\n",
         ok?"PASS":"FAIL",mode==1?"arm64-jit-two-pthreads":mode==2?"c-two-pthreads":"ineligible",
         n,(unsigned long long)n*4ull,stop-start,stop>start?(double)n*4.0/(stop-start):0.0,a,b);
  qrx_dual_destroy(&d);qrx_full_destroy(&cpu);
  return ok?0:1;
}
