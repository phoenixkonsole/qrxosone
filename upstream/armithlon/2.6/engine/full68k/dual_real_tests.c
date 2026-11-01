/* Requires actual Musashi fetch. Two genuine independently executed 68040 contexts. */
#include "qrx68100_dual.h"
#include "qrx68100_parallel.h"
#include "m68k.h"
#include <assert.h>
#include <stdio.h>
static void w16(unsigned char*p,unsigned o,unsigned v){p[o]=v>>8;p[o+1]=v;}
static void w32(unsigned char*p,unsigned o,unsigned v){w16(p,o,v>>16);w16(p,o+2,v);}
int main(void){
    QRXFullCPU cpu;QRXDual dual;unsigned val;
    assert(qrx_full_init(&cpu,65536,68040));
    w32(cpu.ram_owned,0,0x8000);w32(cpu.ram_owned,4,0x100);
    w16(cpu.ram_owned,0x100,0x5280); /* ADDQ.L #1,D0 */
    w16(cpu.ram_owned,0x102,0x60fc); /* BRA -4 */
    w16(cpu.ram_owned,0x200,0x5281); /* ADDQ.L #1,D1 */
    w16(cpu.ram_owned,0x202,0x60fc); /* BRA -4 */
    assert(qrx_full_reset(&cpu));
    assert(qrx_dual_init(&dual,&cpu));
    assert(qrx_dual_run(&dual,4000)==1);
    assert(qrx_dual_reg(&dual,0,M68K_REG_D0)>0);
    assert(dual.core[1].cycles==0); /* original AmigaOS profile */
    assert(qrx_dual_set_mode(&dual,QRX_BOOT_QRX_DUAL));
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_SP,4,0x9000));
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_PC,4,0x200));
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_CTL,4,1));
    assert(qrx_dual_run(&dual,5000)==1);
    assert(qrx_dual_reg(&dual,0,M68K_REG_D0)>0);
    assert(qrx_dual_reg(&dual,0,M68K_REG_D1)==0);
    assert(qrx_dual_reg(&dual,1,M68K_REG_D0)==0);
    assert(qrx_dual_reg(&dual,1,M68K_REG_D1)>0);
    assert(dual.core[1].cycles>0 && dual.core[1].instructions>0);
    assert(qrx_full_bus_read(&cpu,QRX_DUAL_STATUS,4,&val) && val==3);
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CACHE_FLUSH,4,1));
    qrx_dual_destroy(&dual);qrx_full_destroy(&cpu);
    /* A SECOND real Musashi board for the new parallel JIT fast path.
     * The context stack is freshly initialized and neither core has executed
     * unknown instruction counts before we test the loop translator. */
    assert(qrx_full_init(&cpu,65536,68040));
    w32(cpu.ram_owned,0,0x8000);w32(cpu.ram_owned,4,0x100);
    w16(cpu.ram_owned,0x100,0x5280);w16(cpu.ram_owned,0x102,0x60fc);
    w16(cpu.ram_owned,0x200,0x5281);w16(cpu.ram_owned,0x202,0x60fc);
    assert(qrx_full_reset(&cpu));assert(qrx_dual_init(&dual,&cpu));
    assert(qrx_dual_set_mode(&dual,QRX_BOOT_QRX_DUAL));
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_PC,4,0x200));
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_SP,4,0x9000));
    assert(qrx_full_bus_write(&cpu,QRX_DUAL_CORE1_CTL,4,1));
    assert(qrx_dual_commit(&dual));
    assert(qrx_dual_parallel_loops(&dual,50000)==(qrx_dual_native_supported()?1:2));
    assert(qrx_dual_reg(&dual,0,M68K_REG_D0)==50000);
    assert(qrx_dual_reg(&dual,1,M68K_REG_D1)==50000);
    assert(qrx_dual_parallel_loops(&dual,50000)==(qrx_dual_native_supported()?1:2));
    assert(qrx_dual_reg(&dual,0,M68K_REG_D0)==100000);
    assert(qrx_dual_reg(&dual,1,M68K_REG_D1)==100000);
    qrx_dual_destroy(&dual);qrx_full_destroy(&cpu);
    puts("PASS REAL Musashi 68040: legacy single core + two independent instruction streams + shared bus + JIT cache API");
    return 0;
}
