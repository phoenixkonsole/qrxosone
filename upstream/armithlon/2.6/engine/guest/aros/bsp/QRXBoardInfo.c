/* Native AROS m68k QRX board capability probe; no assumptions about AROS SMP.
 * This utility must run only on the QRX virtual 68040 machine. */
#include <proto/dos.h>
#include <stdint.h>
#define QRX_NATIVE_040 1
#include "../../qrxvirt_board.h"
#include "../../qrxvirt_gpu.h"
#include "../../qrxvirt_block.h"
#include "qrx_smp_board.h"
#include "qrxhostfs_guest.h"
int main(void){
  volatile const uint32_t *q=(volatile const uint32_t *)(uintptr_t)QRXVIRT_MMIO_BASE;
  if(q[0]!=QRXVIRT_MAGIC){Printf("QRX68100 board missing\n");return 20;}
  Printf("QRX68100 board v%08lx features %08lx\n",(unsigned long)q[1],(unsigned long)q[2]);
  if(q[2]&QRXVIRT_FEAT_RAMSIZE){
      unsigned long ramlo=q[QRXVIRT_REG_RAM_LO/4],ramhi=q[QRXVIRT_REG_RAM_HI/4];
      Printf("QRX 68040 guest RAM: %08lx:%08lx bytes (hi:lo); not automatically registered as AROS MemHeader\n",ramhi,ramlo);
  }
  Printf("HostFS: %s\n",(q[2]&QRXVIRT_FEAT_HOSTFS)?"present":"not mounted");
  Printf("GPU: %s\n",qrxv_gpu_available()?"present":"not available");
  Printf("Virtual SMP: %ld online\n",(long)qrxb_smp_cpu_count());
  return 0;
}
