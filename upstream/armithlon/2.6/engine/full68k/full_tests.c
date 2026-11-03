#include "qrx68100_full.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) do{if(!(v)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#v);return 1;}}while(0)
static void w16(uint8_t *p,size_t at,uint16_t v){p[at]=(uint8_t)(v>>8);p[at+1]=(uint8_t)v;}
static void w32(uint8_t *p,size_t at,uint32_t v){w16(p,at,(uint16_t)(v>>16));w16(p,at+2,(uint16_t)v);}
typedef struct{char text[128];int n;} TestConsole;
static void console(void *arg,uint8_t ch){TestConsole *t=(TestConsole*)arg;if(t->n<(int)sizeof(t->text)-1)t->text[t->n++]=(char)ch;}
static int test_monitor(void){
    QRXFullCPU c;TestConsole out={0};
    CHECK(qrx_full_init(&c,64u*1024u,68040));
    CHECK(qrx_build_boot_monitor(c.ram_owned,c.vm.ram.size));
    qrx_full_set_host(&c,console,NULL,&out);
    CHECK(qrx_full_reset(&c));
    CHECK(qrx_full_run_cycles(&c,100000)==0);
    CHECK(strcmp(out.text,"QRX68100 qrxvirt board READY\n")==0);
    CHECK(!c.mem_fault && c.vm.cpu.halted);
    CHECK(c.instructions>=30 && c.instructions<200);
    qrx_full_destroy(&c);
    return 0;
}
static int test_real_opcodes(void){
    QRXFullCPU c;
    CHECK(qrx_full_init(&c,32768,68040));
    uint8_t *m=c.ram_owned;
    w32(m,0,0x8000);w32(m,4,0x100);
    w16(m,0x100,0x7004); /* MOVEQ #4,D0 */
    w16(m,0x102,0x7200); /* MOVEQ #0,D1 */
    w16(m,0x104,0x5281); /* ADDQ.L #1,D1 */
    w16(m,0x106,0x51c8);w16(m,0x108,0xfffc); /* DBF D0,$104 */
    w16(m,0x10a,0xc2fc);w16(m,0x10c,0x0003); /* MULU.W #3,D1 */
    w16(m,0x10e,0x4e72);w16(m,0x110,0x2700); /* STOP */
    CHECK(qrx_full_reset(&c));
    CHECK(qrx_full_run_cycles(&c,100000)>=1); /* STOP is not shutdown */
    CHECK(qrx_full_reg(&c,M68K_REG_D1)==15);
    CHECK((qrx_full_reg(&c,M68K_REG_D0)&0xffffu)==0xffffu);
    CHECK(c.instructions>=14 && c.instructions<100);
    CHECK(!c.mem_fault);
    qrx_full_destroy(&c);return 0;
}
static int test_trap(void){
    QRXFullCPU c;CHECK(qrx_full_init(&c,32768,68020));
    uint8_t *m=c.ram_owned;
    w32(m,0,0x8000);w32(m,4,0x100);
    w32(m,34*4,0x180); /* TRAP #2 */
    w16(m,0x100,0x4e42);w16(m,0x102,0x7206);
    w16(m,0x104,0x4e72);w16(m,0x106,0x2700);
    w16(m,0x180,0x700b);w16(m,0x182,0x4e73); /* MOVEQ #11,D0; RTE */
    CHECK(qrx_full_reset(&c));
    CHECK(qrx_full_run_cycles(&c,10000)>=1);
    CHECK(qrx_full_reg(&c,M68K_REG_D0)==11);
    CHECK(qrx_full_reg(&c,M68K_REG_D1)==6);
    CHECK(!c.mem_fault);
    qrx_full_destroy(&c);return 0;
}
static int test_rom_and_mmio(void){
    QRXFullCPU c;CHECK(qrx_full_init(&c,2u*1024u*1024u,68040));
    uint8_t *rom=calloc(512u*1024u,1);CHECK(rom);
    w32(rom,0,0x20000);w32(rom,4,QRX_FULL_ROM_MAIN_BASE+0x100);
    w16(rom,0x100,0x4e72);w16(rom,0x102,0x2700);
    CHECK(qrx_full_load_rom(&c,QRX_FULL_ROM_MAIN_BASE,rom,512u*1024u));
    free(rom);
    qrx_full_set_rom_overlay(&c,true);
    CHECK(qrx_full_reset(&c));
    CHECK(qrx_full_pc(&c)==QRX_FULL_ROM_MAIN_BASE+0x100);
    uint32_t value=0;
    CHECK(qrx_full_bus_read(&c,0,4,&value)&&value==0x20000);
    CHECK(qrx_full_bus_read(&c,QRX_FULL_IO_BASE,4,&value)&&value==QRX_IO_MAGIC);
    CHECK(qrx_full_bus_write(&c,QRX_FULL_ROM_OVERLAY_CTRL,4,0));
    CHECK(qrx_full_bus_read(&c,QRX_FULL_ROM_OVERLAY_CTRL,4,&value)&&value==0);
    CHECK(qrx_full_bus_write(&c,QRX_FULL_ROM_OVERLAY_CTRL,4,1));
    CHECK(!qrx_full_bus_write(&c,QRX_FULL_ROM_MAIN_BASE+0x100,2,0xffff));
    CHECK(qrx_full_bus_read(&c,QRX_FULL_ROM_MAIN_BASE+0x100,2,&value)&&value==0x4e72);
    CHECK(qrx_full_bus_write(&c,0x1000,4,0x12345678));
    CHECK(qrx_full_bus_read(&c,0x1001,2,&value)&&value==0x3456);
    CHECK(!qrx_full_bus_read(&c,0x80000000u,4,&value));
    qrx_full_destroy(&c);return 0;
}
static int test_timer(void){
    QRXFullCPU c;CHECK(qrx_full_init(&c,65536,68000));
    uint8_t *m=c.ram_owned;
    w32(m,0,0x8000);w32(m,4,0x100);
    w32(m,26*4,0x200); /* autovector level 2 */
    w16(m,0x100,0x46fc);w16(m,0x102,0x2000); /* enable IRQ in SR */
    w16(m,0x104,0x60fe); /* BRA -2 forever */
    w16(m,0x200,0x7005); /* MOVEQ #5,D0 */
    w16(m,0x202,0x4e73); /* RTE */
    CHECK(qrx_full_reset(&c));
    CHECK(qrx_full_bus_write(&c,QRX_FULL_TIMER_PERIOD,4,128));
    CHECK(qrx_full_bus_write(&c,QRX_FULL_TIMER_CTRL,4,QRX_FULL_TIMER_ENABLE));
    CHECK(qrx_full_run_cycles(&c,4096)>=1);
    CHECK(qrx_full_reg(&c,M68K_REG_D0)==5);
    uint32_t pend=0;CHECK(qrx_full_bus_read(&c,QRX_FULL_IRQ_PENDING,4,&pend));
    CHECK(pend!=0);
    CHECK(qrx_full_bus_write(&c,QRX_FULL_IRQ_ACK,4,1u<<2));
    CHECK(qrx_full_bus_read(&c,QRX_FULL_IRQ_PENDING,4,&pend)&&pend==0);
    qrx_full_destroy(&c);return 0;
}
int main(void){
    int (*tests[])(void)={test_monitor,test_real_opcodes,test_trap,test_rom_and_mmio,test_timer};
    const char *labels[]={"boot monitor", "DBRA/MULU/STOP", "TRAP/RTE", "ROM+MMIO bus", "cycle timer IRQ"};
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(tests[i]())return 1;
        printf("PASS Musashi full68k: %s\n",labels[i]);
    }
    return 0;
}
