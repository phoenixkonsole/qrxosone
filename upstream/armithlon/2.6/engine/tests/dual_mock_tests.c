/* Hermetic ABI + coherence tests using a non-executing Musashi test double. */
#include "qrx68100_dual.h"
#include "m68k.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put32(unsigned char *p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(void){
  QRXFullCPU c;QRXDual d;uint32_t v=0;
  assert(qrx_full_init(&c,65536,68040));
  put32(c.ram_owned,0x10000);put32(c.ram_owned+4,0x100);
  assert(qrx_full_reset(&c));
  assert(qrx_dual_init(&d,&c));
  assert(d.mode==QRX_BOOT_LEGACY_AMIGA && !d.core[1].online);
  /* AmigaOS 3.x: no second CPU, new registers absent. */
  assert(!qrx_full_bus_read(&c,QRX_DUAL_MAGIC,4,&v));
  assert(!qrx_full_bus_write(&c,QRX_DUAL_CORE1_CTL,4,1));
  assert(qrx_dual_run(&d,512)==1 && d.core[0].cycles>=512 && d.core[1].cycles==0);
  assert(qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL));
  m68k_set_reg(M68K_REG_SR,0); /* user mode: deny SMP control writes */
  assert(!qrx_full_bus_write(&c,QRX_DUAL_CORE1_CTL,4,1));
  m68k_set_reg(M68K_REG_SR,0x2700);
  assert(qrx_full_bus_read(&c,QRX_DUAL_MAGIC,4,&v)&&v==0x51525844);
  assert(qrx_full_bus_read(&c,QRX_DUAL_FEATURES,4,&v)&&(v&QRX_DUAL_FEATURE_IPI));
  assert(qrx_full_bus_write(&c,QRX_DUAL_CORE1_PC,4,0x200));
  assert(qrx_full_bus_write(&c,QRX_DUAL_CORE1_SP,4,0x8000));
  assert(qrx_full_bus_write(&c,QRX_DUAL_CORE1_CTL,4,1));
  assert(!d.core[1].online && d.boot_requested); /* deferred, safe to commit */
  assert(qrx_dual_commit(&d));
  assert(d.core[1].online && qrx_dual_pc(&d,1)==0x200);
  assert(qrx_dual_pc(&d,0)==0x100);
  assert(qrx_dual_run(&d,1024)==1);
  assert(d.core[0].cycles>=1536 && d.core[1].cycles>=1024);
  /* Multi-ISA cache stores translations' metadata only. */
  assert(qrx_dual_jit_insert(&d,0,0,QRX_DUAL_ISA_X86,0x1000,32));
  assert(qrx_dual_jit_insert(&d,1,0,QRX_DUAL_ISA_PPC,0x1000,32));
  assert(qrx_dual_jit_lookup(&d,0,0,QRX_DUAL_ISA_X86,0x1000));
  assert(qrx_dual_jit_lookup(&d,1,0,QRX_DUAL_ISA_PPC,0x1000));
  assert(!qrx_dual_jit_insert(&d,0,0,QRX_DUAL_ISA_ARM,0x0ffc,16)); /* cross-page */
  assert(qrx_full_bus_write(&c,0x1002,2,0x4e71));
  assert(!qrx_dual_jit_lookup(&d,0,0,QRX_DUAL_ISA_X86,0x1000));
  assert(!qrx_dual_jit_lookup(&d,1,0,QRX_DUAL_ISA_PPC,0x1000));
  assert(qrx_dual_jit_insert(&d,0,1,QRX_DUAL_ISA_X64,0x1000,32));
  assert(!qrx_dual_jit_lookup(&d,0,2,QRX_DUAL_ISA_X64,0x1000)); /* ASID */
  assert(qrx_dual_jit_lookup(&d,0,1,QRX_DUAL_ISA_X64,0x1000));
  qrx_dual_notify_ram_write(&d,0x1000,4); /* bypassed bus DMA */
  assert(!qrx_dual_jit_lookup(&d,0,1,QRX_DUAL_ISA_X64,0x1000));
  assert(qrx_dual_jit_insert(&d,1,7,QRX_DUAL_ISA_ARM,0x1010,32));
  assert(qrx_full_bus_write(&c,QRX_DUAL_CACHE_FLUSH,4,1));
  assert(!qrx_dual_jit_lookup(&d,1,7,QRX_DUAL_ISA_ARM,0x1010));
  assert(qrx_full_bus_write(&c,QRX_DUAL_IPI_SET,4,2));
  assert(d.core[1].ipi_pending);
  assert(qrx_full_bus_write(&c,QRX_DUAL_CORE1_CTL,4,2));
  assert(qrx_dual_commit(&d) && !d.core[1].online);
  assert(qrx_dual_set_mode(&d,QRX_BOOT_LEGACY_AMIGA));
  assert(!qrx_full_bus_read(&c,QRX_DUAL_MAGIC,4,&v));
  assert(qrx_dual_run(&d,256)==1);
  assert(d.core[1].cycles<d.core[0].cycles);
  qrx_dual_destroy(&d);qrx_full_destroy(&c);
  puts("PASS dual-core MMIO, singlecore Amiga compatibility, context isolation (stub), multi-ISA JIT block metadata, page-coherent invalidation (NOT 68k ISA execution)");
  return 0;
}
