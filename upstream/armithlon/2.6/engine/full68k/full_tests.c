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
    CHECK(qrx_full_run_cycles(&c,100000)==0); /* QUIT monitor -> shutdown=1 */
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
    /* Open guest bus floats high (Amiga-faithful): motherboard-RAM probes
     * must fail their compare instead of faulting. ROM writes still fail. */
    CHECK(qrx_full_bus_read(&c,0x80000000u,4,&value)&&value==0xFFFFFFFFu);
    CHECK(qrx_full_bus_write(&c,0x80000000u,4,0x12345678));
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
static uint32_t rd(QRXFullCPU *c,uint32_t a,unsigned n){uint32_t v=0;qrx_full_bus_read(c,a,n,&v);return v;}
static void wr(QRXFullCPU *c,uint32_t a,unsigned n,uint32_t v){qrx_full_bus_write(c,a,n,v);}
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int test_amiga_devices(void){
    QRXFullCPU c;CHECK(qrx_full_init(&c,32*1024*1024,68040));
    uint8_t *rom=calloc(512*1024,1),*adf=malloc(1802240);CHECK(rom&&adf);
    w32(rom,0,0x80000);w32(rom,4,0xf80100);w16(rom,0x100,0x4e72);w16(rom,0x102,0x2700);
    CHECK(qrx_full_load_rom(&c,0xf80000,rom,512*1024));free(rom);
    for(unsigned i=0;i<1802240;i++)adf[i]=(uint8_t)(i*13+(i>>9));
    qrx_full_load_adf(&c,adf,1802240);qrx_full_set_rom_overlay(&c,true);CHECK(qrx_full_reset(&c));
    wr(&c,0xbfe001,1,0);CHECK(!c.rom_overlay);
    wr(&c,0xbfd100,1,0x75);CHECK(rd(&c,0xbfd100,1)==0x75);CHECK(!(rd(&c,0xbfe001,1)&0x10));
    wr(&c,0xbfd100,1,0x74);wr(&c,0xbfd100,1,0x75);CHECK(c.fl_cyl[0]==1);CHECK(rd(&c,0xbfe001,1)&0x10);
    wr(&c,0xbfd100,1,0x76);wr(&c,0xbfd100,1,0x77);CHECK(c.fl_cyl[0]==0);
    wr(&c,0xdff09a,2,0xc00a);CHECK(rd(&c,0xdff01c,2)==0x400a);
    wr(&c,0xbfed01,1,0x81);wr(&c,0xbfee01,1,8);wr(&c,0xbfe401,1,99);wr(&c,0xbfe501,1,0);
    CHECK(rd(&c,0xbfee01,1)&1);CHECK(qrx_full_run_cycles(&c,2000)==1);
    CHECK(!(rd(&c,0xbfee01,1)&1));CHECK((rd(&c,0xbfed01,1)&0x81)==0x81);CHECK(rd(&c,0xbfed01,1)==0);
    CHECK(rd(&c,0xdff01e,2)&8);wr(&c,0xdff09c,2,8);CHECK(!(rd(&c,0xdff01e,2)&8));
    wr(&c,0xdff1f4,2,0x0d04);CHECK(c.fsaga_width==1024 && c.fsaga_height==768);
    wr(&c,0xdff1f4,2,0x0c02);CHECK(c.fsaga_width==800 && c.fsaga_height==600);
    /* This interval was incorrectly swallowed by an absent Gayle device. */
    for(uint32_t addr=0xda8000;addr<0xdb0000;addr+=4){
        wr(&c,addr,4,addr^0x5a123456u);CHECK(rd(&c,addr,4)==(addr^0x5a123456u));
    }
    c.fl_cyl[0]=40;wr(&c,0xbfd100,1,0x77);wr(&c,0xdff096,2,0x8210);wr(&c,0xdff020,4,0x10000);
    wr(&c,0xdff024,2,0x8000|13630);CHECK(!c.dsk_active);wr(&c,0xdff024,2,0x8000|13630);CHECK(c.dsk_active);
    CHECK(qrx_full_run_cycles(&c,1500000)==1);CHECK(!c.dsk_active);CHECK(rd(&c,0xdff01e,2)&2);
    const uint8_t *m=c.ram_owned+0x10000;
    for(unsigned sec=0;sec<22;sec++){
        const uint8_t *p=m+3320+8+sec*1088;
        uint32_t id=((be32(p)&0x55555555)<<1)|(be32(p+4)&0x55555555);
        CHECK(id==(0xff500000u|(sec<<8)|(22-sec)));
        uint32_t sum=0;for(unsigned j=0;j<10;j++)sum^=be32(p+j*4)&0x55555555;
        uint32_t hs=((be32(p+40)&0x55555555)<<1)|(be32(p+44)&0x55555555);CHECK(sum==hs);
        uint32_t ds=((be32(p+48)&0x55555555)<<1)|(be32(p+52)&0x55555555);
        for(unsigned j=0;j<128;j++){
            uint32_t o=be32(p+56+j*4)&0x55555555,e=be32(p+568+j*4)&0x55555555;
            CHECK(((o<<1)|e)==be32(adf+(80*22+sec)*512+j*4));ds^=o^e;
        }
        CHECK(ds==0);
    }
    wr(&c,0xdff09c,2,2);CHECK(!(rd(&c,0xdff01e,2)&2));
    free(adf);qrx_full_destroy(&c);return 0;
}
int main(void){
    int (*tests[])(void)={test_monitor,test_real_opcodes,test_trap,test_rom_and_mmio,test_timer,test_amiga_devices};
    const char *labels[]={"boot monitor", "DBRA/MULU/STOP", "TRAP/RTE", "ROM+MMIO bus", "cycle timer IRQ", "ROM timers and HD disk DMA data/checksums"};
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        if(tests[i]())return 1;
        printf("PASS Musashi full68k: %s\n",labels[i]);
    }
    return 0;
}
