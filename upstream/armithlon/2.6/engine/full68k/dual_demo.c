/* ARMITHLON 1.5 real-Musashi QRX68100 2x 68040 diagnostic application. */
#include "qrx68100_dual.h"
#include "m68k.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
static void put16(uint8_t*m,unsigned a,uint16_t v){m[a]=(uint8_t)(v>>8);m[a+1]=(uint8_t)v;}
static void put32(uint8_t*m,unsigned a,uint32_t v){put16(m,a,v>>16);put16(m,a+2,(uint16_t)v);}
int main(int argc,char **argv){
  (void)argv;
  QRXFullCPU b;QRXDual d;
  if(!qrx_full_init(&b,1u<<20,68040)){fputs("full board init failed\n",stderr);return 2;}
  put32(b.ram_owned,0,0x80000);put32(b.ram_owned,4,0x100);
  put16(b.ram_owned,0x100,0x5280);put16(b.ram_owned,0x102,0x60fc);
  put16(b.ram_owned,0x200,0x5281);put16(b.ram_owned,0x202,0x60fc);
  if(!qrx_full_reset(&b)||!qrx_dual_init(&d,&b)){fputs("CPU init failed\n",stderr);qrx_full_destroy(&b);return 2;}
  int rc=qrx_dual_run(&d,50000);
  printf("mode=AmigaOS3.x visible_cores=1 rc=%d core0.D0=%u core1.cycles=%" PRIu64 "\n",
      rc,qrx_dual_reg(&d,0,M68K_REG_D0),d.core[1].cycles);
  if(argc>1 && argv[1][0]=='d'){
      if(!qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL)||
         !qrx_full_bus_write(&b,QRX_DUAL_CORE1_PC,4,0x200)||
         !qrx_full_bus_write(&b,QRX_DUAL_CORE1_SP,4,0x90000)||
         !qrx_full_bus_write(&b,QRX_DUAL_CORE1_CTL,4,1)){
          fputs("second core registration failed\n",stderr);qrx_dual_destroy(&d);qrx_full_destroy(&b);return 2;
      }
      rc=qrx_dual_run(&d,50000);
      printf("mode=QRX visible_cores=2 rc=%d core0.D0=%u core1.D1=%u c0.cycles=%" PRIu64 " c1.cycles=%" PRIu64 "\n",
          rc,qrx_dual_reg(&d,0,M68K_REG_D0),qrx_dual_reg(&d,1,M68K_REG_D1),
          d.core[0].cycles,d.core[1].cycles);
  }
  qrx_dual_destroy(&d);qrx_full_destroy(&b);
  return rc<0?1:0;
}
