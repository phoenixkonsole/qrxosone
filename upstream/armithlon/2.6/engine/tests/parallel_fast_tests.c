/* Hermetic dual-core FAST LOOP test; Musashi stub is NOT real 68040 execution.
 * It DOES execute 2 host pthread workers, and native A64 code on AArch64.
 */
#include "qrx68100_parallel.h"
#include "m68k.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
static void put16(unsigned char*p,unsigned off,unsigned v){p[off]=v>>8;p[off+1]=v;}
static void put32(unsigned char*p,unsigned off,unsigned v){put16(p,off,v>>16);put16(p,off+2,v);}
int main(void) {
  QRXFullCPU b;QRXDual d;int expect=qrx_dual_native_supported()?1:2;
  assert(qrx_full_init(&b,65536,68040));
  put32(b.ram_owned,0,0x8000);put32(b.ram_owned,4,0x100);
  put16(b.ram_owned,0x100,0x5280);put16(b.ram_owned,0x102,0x60fc);
  put16(b.ram_owned,0x200,0x5281);put16(b.ram_owned,0x202,0x60fc);
  assert(qrx_full_reset(&b));
  assert(qrx_dual_init(&d,&b));
  assert(qrx_dual_parallel_loops(&d,1000)==0);/* legacy: no second CPU */
  assert(qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL));
  assert(qrx_full_bus_write(&b,QRX_DUAL_CORE1_PC,4,0x200));
  assert(qrx_full_bus_write(&b,QRX_DUAL_CORE1_SP,4,0x9000));
  assert(qrx_full_bus_write(&b,QRX_DUAL_CORE1_CTL,4,1));
  assert(qrx_dual_commit(&d));
  /* Test correct N, Z, V, C/X semantics, not only low-word arithmetic. */
  m68k_set_reg(M68K_REG_D0,0x7fffffffu);
  assert(qrx_dual_parallel_loops(&d,1)==expect);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==0x80000000u);
  assert((qrx_dual_reg(&d,0,M68K_REG_SR)&0x1fu)==0x0au);
  assert(qrx_dual_reg(&d,1,M68K_REG_D1)==1u);
  assert(qrx_dual_pc(&d,0)==0x100u && qrx_dual_pc(&d,1)==0x200u);
  assert(qrx_dual_parallel_loops(&d,0)==-1);
  assert(qrx_dual_parallel_loops(&d,200000)==expect);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==0x80000000u+200000u);
  assert(qrx_dual_reg(&d,1,M68K_REG_D1)==200001u);
  assert(d.core[0].parallel_iterations==200001u && d.core[1].parallel_iterations==200001u);
  /* Same guest page modified: compiled code cannot be reused unsafely. */
  assert(qrx_full_bus_write(&b,0x102,2,0x4e71u));
  uint32_t before=qrx_dual_reg(&d,0,M68K_REG_D0);
  assert(qrx_dual_parallel_loops(&d,1000)==0);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==before);
  assert(qrx_full_bus_write(&b,0x102,2,0x60fcu));
  assert(qrx_dual_parallel_loops(&d,1000)==expect);
  /* Replace source code with ADDQ.L #8,D0: force translation replacement,
   * then verify wrap, X/C and Z flags on both host backends. */
  assert(qrx_full_bus_write(&b,0x100,2,0x5080u));
  m68k_set_reg(M68K_REG_D0,0xfffffff8u);
  assert(qrx_dual_parallel_loops(&d,1)==expect);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==0u);
  assert((qrx_dual_reg(&d,0,M68K_REG_SR)&0x1fu)==0x15u);
  /* Explicit flush replaces cached code even without a guest-memory write. */
  assert(qrx_full_bus_write(&b,QRX_DUAL_CACHE_FLUSH,4,1));
  assert(qrx_dual_parallel_loops(&d,1)==expect);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==8u);
  /* 1.7 Vampire-inspired ADD/SUB fusion: same stable branch, subtract underflow. */
  assert(qrx_full_bus_write(&b,0x100,2,0x5380u)); /* SUBQ.L #1,D0 */
  m68k_set_reg(M68K_REG_D0,0u);
  assert(qrx_dual_parallel_loops(&d,1)==expect);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==UINT32_MAX);
  assert((qrx_dual_reg(&d,0,M68K_REG_SR)&0x1fu)==0x19u);
  m68k_set_reg(M68K_REG_D0,0x80000000u);
  assert(qrx_dual_parallel_loops(&d,1)==expect);
  assert(qrx_dual_reg(&d,0,M68K_REG_D0)==0x7fffffffu);
  assert((qrx_dual_reg(&d,0,M68K_REG_SR)&0x1fu)==0x02u);
  assert(qrx_full_bus_write(&b,0x100,2,0x5280u));
  /* Preserve 68k architectural visibility at legacy transition. */
  assert(qrx_dual_set_mode(&d,QRX_BOOT_LEGACY_AMIGA));
  assert(qrx_dual_parallel_loops(&d,1000)==0 && !d.core[1].online);
  qrx_dual_destroy(&d);qrx_full_destroy(&b);
  printf("PASS parallel 2-worker loop executor / %s / flag coherence, invalidation, legacy gating\n",
          expect==1?"native-AArch64-JIT":"portable-C-fallback");
  return 0;
}
