/* 1.7 QRX-specific 68080-inspired extension ABI. Musashi stub: NOT ISA proof. */
#include "qrx68100_vampire.h"
#include "m68k.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static void put32(uint8_t*p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void writev(QRXFullCPU *b,unsigned reg,uint64_t value){
    assert(qrx_full_bus_write(b,QRX_VAMP_VSEL,4,reg));
    assert(qrx_full_bus_write(b,QRX_VAMP_VLO,4,(uint32_t)value));
    assert(qrx_full_bus_write(b,QRX_VAMP_VHI,4,(uint32_t)(value>>32)));
}
static uint64_t readv(QRXFullCPU *b,unsigned reg){
    uint32_t lo,hi;
    assert(qrx_full_bus_write(b,QRX_VAMP_VSEL,4,reg));
    assert(qrx_full_bus_read(b,QRX_VAMP_VLO,4,&lo));
    assert(qrx_full_bus_read(b,QRX_VAMP_VHI,4,&hi));
    return ((uint64_t)hi<<32)|lo;
}
static unsigned do_vector(QRXFullCPU*b,unsigned a,unsigned c,unsigned dst,unsigned cmd){
    uint32_t v;
    assert(qrx_full_bus_write(b,QRX_VAMP_VSEL,4,a));
    assert(qrx_full_bus_write(b,QRX_VAMP_VSRC,4,c));
    assert(qrx_full_bus_write(b,QRX_VAMP_VDST,4,dst));
    assert(qrx_full_bus_write(b,QRX_VAMP_VCMD,4,cmd));
    assert(qrx_full_bus_read(b,QRX_VAMP_VSTATUS,4,&v));
    return v;
}
int main(void){
    QRXFullCPU b;QRXDual d;uint32_t v=0;
    assert(qrx_full_init(&b,1u<<16,68040));
    put32(b.ram_owned,0x8000);put32(b.ram_owned+4,0x100);
    assert(qrx_full_reset(&b));assert(qrx_dual_init(&d,&b));
    assert(d.mode==QRX_BOOT_LEGACY_AMIGA && d.core[1].online==0);
    assert(!qrx_full_bus_read(&b,QRX_VAMP_MAGIC,4,&v));
    assert(!qrx_full_bus_write(&b,QRX_VAMP_VCMD,4,QRX_VAMP_CMD_PADDB));
    assert(!qrx_vamp_vector_write(&d,0,0,1));
    assert(qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL));
    assert(qrx_full_bus_read(&b,QRX_VAMP_MAGIC,4,&v)&&v==0x51563137);
    assert(qrx_full_bus_read(&b,QRX_VAMP_CPU_ID,4,&v)&&v==0x68100);
    assert(qrx_full_bus_read(&b,QRX_VAMP_FEATURES,4,&v)&&v==QRX_VAMP_CAPABILITIES);
    assert(qrx_full_bus_read(&b,QRX_VAMP_IC_BYTES,4,&v)&&v==16384);
    assert(qrx_full_bus_read(&b,QRX_VAMP_DC_BYTES,4,&v)&&v==32768);
    assert(qrx_full_bus_read(&b,QRX_VAMP_L2_BYTES,4,&v)&&v==0);
    assert(!qrx_full_bus_write(&b,QRX_VAMP_VSEL,4,24));
    assert(!qrx_full_bus_write(&b,QRX_VAMP_VSRC,4,24));
    assert(!qrx_full_bus_write(&b,QRX_VAMP_VDST,4,24));
    assert(!qrx_full_bus_write(&b,QRX_VAMP_CTRL,4,0xdead));
    assert(!qrx_full_bus_read(&b,QRX_VAMP_VLO,2,&v)); /* 32-bit only */
    /* Properly independent 64-bit vector registers, 3-register ALU. */
    writev(&b,0,UINT64_C(0x01020304feff0010));
    writev(&b,1,UINT64_C(0x080706050102fffe));
    assert(do_vector(&b,0,1,2,QRX_VAMP_CMD_PADDB)==0);
    assert(readv(&b,2)==UINT64_C(0x09090909ff01ff0e));
    assert(do_vector(&b,0,1,3,QRX_VAMP_CMD_PADDW)==0);
    assert(readv(&b,3)==UINT64_C(0x090909090001000e));
    assert(do_vector(&b,0,1,4,QRX_VAMP_CMD_PSUBB)==0);
    assert(readv(&b,4)==UINT64_C(0xf9fbfdfffdfd0112));
    assert(do_vector(&b,0,1,5,QRX_VAMP_CMD_PXOR)==0);
    assert(readv(&b,5)==(UINT64_C(0x01020304feff0010)^UINT64_C(0x080706050102fffe)));
    writev(&b,6,UINT64_MAX);
    writev(&b,7,UINT64_C(0x0101010101010101));
    assert(do_vector(&b,6,7,8,QRX_VAMP_CMD_PADDSB)==0 && readv(&b,8)==UINT64_MAX);
    assert(do_vector(&b,6,7,9,QRX_VAMP_CMD_PAVGB)==0 && readv(&b,9)==UINT64_C(0x8080808080808080));
    writev(&b,10,UINT64_MAX);writev(&b,11,2);
    assert(do_vector(&b,10,11,12,QRX_VAMP_CMD_MUL64)==0);
    assert(readv(&b,12)==UINT64_MAX-1u);
    assert(do_vector(&b,10,11,13,QRX_VAMP_CMD_DIV64)==0);
    assert(readv(&b,13)==UINT64_MAX/2u);
    writev(&b,14,1234);writev(&b,15,0);
    assert(do_vector(&b,14,15,12,QRX_VAMP_CMD_DIV64)==QRX_VAMP_STATUS_DIV_ZERO);
    assert(readv(&b,12)==UINT64_MAX-1u);
    assert(do_vector(&b,0,1,12,0xffff)==QRX_VAMP_STATUS_BAD_CMD);
    assert(readv(&b,12)==UINT64_MAX-1u);
    /* User-mode SIMD operations allowed; privileged cache control denied. */
    m68k_set_reg(M68K_REG_SR,0);
    assert(!qrx_full_bus_write(&b,QRX_VAMP_CTRL,4,0));
    assert(!qrx_full_bus_write(&b,QRX_VAMP_CACHE_INV,4,1));
    assert(qrx_full_bus_write(&b,QRX_VAMP_VCMD,4,QRX_VAMP_CMD_PXOR));
    m68k_set_reg(M68K_REG_SR,0x2700);
    /* I-cache is a *functional* JIT decoder cache. Different from 68040 fetch. */
    assert(qrx_full_bus_write(&b,0x1000,4,0x528060fc));
    assert(qrx_vamp_cached_read(&d,0,0x1000,2,true,&v)&&v==0x5280);
    assert(qrx_vamp_cached_read(&d,0,0x1000,2,true,&v)&&v==0x5280);
    assert(qrx_full_bus_read(&b,QRX_VAMP_IC_HITS,4,&v)&&v>=1);
    assert(qrx_full_bus_read(&b,QRX_VAMP_IC_MISSES,4,&v)&&v>=1);
    assert(qrx_vamp_cached_read(&d,0,0x1002,2,false,&v)&&v==0x60fc);
    assert(qrx_vamp_cached_read(&d,0,0x1002,2,false,&v)&&v==0x60fc);
    assert(qrx_full_bus_read(&b,QRX_VAMP_DC_HITS,4,&v)&&v>=1);
    /* Normal bus write invalidates I+D cache; DMA API invalidates both cores. */
    assert(qrx_full_bus_write(&b,0x1000,2,0x4e71));
    assert(qrx_vamp_cached_read(&d,0,0x1000,2,true,&v)&&v==0x4e71);
    b.ram_owned[0x1001]=0x75;
    qrx_dual_notify_ram_write(&d,0x1001,1);
    assert(qrx_vamp_cached_read(&d,0,0x1000,2,true,&v)&&v==0x4e75);
    /* A 32-bit unaligned store touches two different 64-byte cache lines. */
    assert(qrx_full_bus_write(&b,0x103e,4,0x11223344));
    assert(qrx_vamp_cached_read(&d,0,0x103e,2,true,&v)&&v==0x1122);
    assert(qrx_vamp_cached_read(&d,0,0x1040,2,true,&v)&&v==0x3344);
    assert(qrx_full_bus_write(&b,0x103f,4,0xaabbccdd));
    assert(qrx_vamp_cached_read(&d,0,0x103e,2,true,&v)&&v==0x11aa);
    assert(qrx_vamp_cached_read(&d,0,0x1040,2,true,&v)&&v==0xbbcc);
    /* 4-way set associative cache: five tags in one set cause eviction. */
    uint32_t old_misses,new_misses;
    assert(qrx_full_bus_read(&b,QRX_VAMP_IC_MISSES,4,&old_misses));
    for(unsigned tag=0x2000;tag<=0x6000;tag+=0x1000){
        assert(qrx_vamp_cached_read(&d,0,tag,2,true,&v));
    }
    assert(qrx_full_bus_read(&b,QRX_VAMP_IC_MISSES,4,&new_misses));
    assert(new_misses>=old_misses+5);
    /* A simple 2-bit dynamic branch predictor, no fake IPC speed claim. */
    assert(!qrx_vamp_predict(&d,0,0x200));
    qrx_vamp_branch(&d,0,0x200,true);
    assert(qrx_vamp_predict(&d,0,0x200));
    qrx_vamp_branch(&d,0,0x200,true);
    qrx_vamp_branch(&d,0,0x200,false);
    assert(qrx_full_bus_read(&b,QRX_VAMP_BP_BAD,4,&v)&&v==2);
    assert(qrx_full_bus_read(&b,QRX_VAMP_BP_GOOD,4,&v)&&v==1);
    assert(qrx_vamp_prefetch(&d,0,0x1800));
    assert(qrx_full_bus_write(&b,QRX_VAMP_CTRL,4,0));
    assert(!qrx_vamp_prefetch(&d,0,0x1800));
    assert(qrx_full_bus_write(&b,QRX_VAMP_CTRL,4,QRX_VAMP_CTRL_MASK));
    /* Two actual (mocked-context) cores see distinct vectors and shared RAM. */
    assert(qrx_full_bus_write(&b,QRX_DUAL_CORE1_PC,4,0x200));
    assert(qrx_full_bus_write(&b,QRX_DUAL_CORE1_SP,4,0x8000));
    assert(qrx_full_bus_write(&b,QRX_DUAL_CORE1_CTL,4,1));
    assert(qrx_dual_commit(&d)&&d.core[1].online);
    assert(qrx_vamp_vector_write(&d,1,0,UINT64_C(0xabcdabcdabcdabcd)));
    assert(qrx_vamp_vector_execute(&d,1,0,0,2,QRX_VAMP_CMD_PXOR)==0);
    assert(qrx_vamp_vector(&d,1,2)==0 && qrx_vamp_vector(&d,0,0)==UINT64_C(0x01020304feff0010));
    assert(qrx_vamp_cached_read(&d,1,0x1000,2,true,&v)&&v==0x4e75);
    assert(qrx_full_bus_write(&b,0x1000,2,0x4e71));
    assert(qrx_vamp_cached_read(&d,1,0x1000,2,true,&v)&&v==0x4e71);
    /* Switching into legacy mode zeroes extended registers, hides core 1. */
    assert(qrx_dual_set_mode(&d,QRX_BOOT_LEGACY_AMIGA));
    assert(!qrx_full_bus_read(&b,QRX_VAMP_MAGIC,4,&v));
    assert(!qrx_vamp_cached_read(&d,0,0x1000,2,true,&v));
    assert(qrx_dual_set_mode(&d,QRX_BOOT_QRX_DUAL));
    assert(qrx_vamp_vector(&d,0,0)==0 && !d.core[1].online);
    qrx_dual_destroy(&d);qrx_full_destroy(&b);
    puts("PASS QRX68100-1.7 Vampire-inspired functional L1/JIT, vector64, 64-bit arithmetic, branch predictor, SMP isolation, Amiga legacy (CPU STUB, no Apollo AMMX opcode emulation)");
    return 0;
}
