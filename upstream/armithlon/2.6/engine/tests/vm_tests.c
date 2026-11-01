#include "qrxvirt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(cond) do {if (!(cond)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);return false;}}while(0)
static void w16(uint8_t *b,size_t at,uint16_t x) {b[at]=x>>8;b[at+1]=x;}
static void w32(uint8_t *b,size_t at,uint32_t x) {w16(b,at,x>>16);w16(b,at+2,x);}
static void vectors(uint8_t *b,size_t size) {memset(b,0,size);w32(b,0,(uint32_t)size);w32(b,4,0x100);}
typedef struct {char str[128];size_t count;unsigned ticks;} Host;
static void charout(void *ctx,uint8_t c) {
    Host *h=ctx;
    if (h->count<sizeof(h->str)-1) h->str[h->count++]=(char)c;
}
static uint32_t clockms(void *ctx) {return ((Host *)ctx)->ticks;}
static bool test_ram_and_board(void) {
    uint8_t ram[1024]; vectors(ram,sizeof ram);
    QRXVM v; Host h={0}; h.ticks=12345678;
    qrx_vm_init(&v,ram,sizeof ram);qrx_vm_set_host(&v,charout,clockms,&h);
    CHECK(qrx_vm_reset(&v));
    CHECK(v.cpu.a[7]==sizeof ram && v.cpu.pc==0x100 && v.cpu.sr==0x2700);
    CHECK(QRX_VCPU_ONLINE==1 && QRX_VCPU_CAPACITY==2);
    uint32_t x=0;
    CHECK(qrx_vm_write(&v,0x20,4,0x12345678));
    CHECK(qrx_vm_read(&v,0x20,4,&x) && x==0x12345678);
    CHECK(ram[0x20]==0x12 && ram[0x23]==0x78);
    CHECK(qrx_vm_read(&v,0x21,2,&x) && x==0x3456); /* 68020+ unaligned data */
    CHECK(!qrx_vm_write(&v,1023,4,100));
    CHECK(!qrx_vm_read(&v,QRX_IO_BASE+1,4,&x));
    CHECK(!qrx_vm_read(&v,QRX_IO_BASE+0x40,4,&x));
    CHECK(qrx_vm_read(&v,QRX_IO_BASE,4,&x) && x==QRX_IO_MAGIC);
    CHECK(qrx_vm_read(&v,QRX_IO_BASE+4,4,&x) && x==QRX_IO_VERSION);
    CHECK(qrx_vm_read(&v,QRX_IO_BASE+0x10,4,&x) && x==12345678);
    CHECK(qrx_vm_read(&v,QRX_IO_BASE+0x14,4,&x) && x==1);
    CHECK(qrx_vm_read(&v,QRX_IO_BASE+0x18,4,&x) && x==2);
    CHECK(qrx_vm_write(&v,QRX_IO_BASE+0x0c,1,'A'));
    CHECK(!qrx_vm_write(&v,QRX_IO_BASE+0x0c,4,'A'));
    CHECK(strcmp(h.str,"A")==0);
    CHECK(v.ram_write_epoch==1);
    return true;
}
static bool test_monitor(void) {
    uint8_t ram[2048];
    QRXVM v; Host h={0};
    CHECK(qrx_build_boot_monitor(ram,sizeof ram));
    qrx_vm_init(&v,ram,sizeof ram);qrx_vm_set_host(&v,charout,clockms,&h);
    CHECK(qrx_vm_reset(&v));
    CHECK(qrx_vm_run(&v,500)==0);
    CHECK(!v.cpu.fault && v.cpu.halted);
    CHECK(strcmp(h.str,"QRX68100 qrxvirt board READY\n")==0);
    return true;
}
static bool test_flags_branches(void) {
    uint8_t ram[1024];vectors(ram,sizeof ram);
    size_t p=0x100;
    w16(ram,p,0x70ff);p+=2; /* MOVEQ #-1,D0 */
    w16(ram,p,0x5280);p+=2; /* ADDQ.L #1,D0 => zero, X/C */
    w16(ram,p,0x6702);p+=2; /* BEQ +2, skips MOVEQ #7,D1 */
    w16(ram,p,0x7207);p+=2;
    w16(ram,p,0x5380);p+=2; /* SUBQ.L #1,D0 => -1, X/C */
    w16(ram,p,0x6b02);p+=2; /* BMI +2, skips MOVEQ #4,D1 */
    w16(ram,p,0x7204);p+=2;
    w16(ram,p,0x4e72);p+=2;w16(ram,p,0x2700); /* STOP */
    QRXVM v; qrx_vm_init(&v,ram,sizeof ram);
    CHECK(qrx_vm_reset(&v));
    for (int i=0; i<5; ++i) CHECK(qrx_vm_step(&v));
    CHECK(v.cpu.d[0]==0xffffffffu && v.cpu.d[1]==0);
    CHECK((v.cpu.sr&0x1f)==(QRX_CCR_X|QRX_CCR_N|QRX_CCR_C));
    CHECK(qrx_vm_run(&v,100)==0);
    return true;
}
static bool test_jsr_and_memory(void) {
    uint8_t ram[1024];vectors(ram,sizeof ram);
    size_t p=0x100;
    w16(ram,p,0x4eb9);p+=2;w32(ram,p,0x140);p+=4; /* JSR */
    w16(ram,p,0x41f9);p+=2;w32(ram,p,0x180);p+=4; /* LEA 0x180,A0 */
    w16(ram,p,0x20bc);p+=2;w32(ram,p,0xaabbccdd);p+=4; /* MOVE.L #imm,(A0) */
    w16(ram,p,0x2010);p+=2; /* MOVE.L (A0),D0 */
    w16(ram,p,0x4e72);p+=2;w16(ram,p,0x2700);
    w16(ram,0x140,0x7205);w16(ram,0x142,0x4e75); /* MOVEQ #5,D1; RTS */
    QRXVM v;qrx_vm_init(&v,ram,sizeof ram);
    CHECK(qrx_vm_reset(&v));CHECK(qrx_vm_run(&v,50)==0);
    CHECK(v.cpu.d[0]==0xaabbccdd && v.cpu.d[1]==5);
    CHECK(v.cpu.a[7]==sizeof ram);
    CHECK(ram[0x180]==0xaa && ram[0x183]==0xdd);
    return true;
}
static bool test_exception_user_to_supervisor(void) {
    uint8_t ram[1024];vectors(ram,sizeof ram);
    w32(ram,4*4,0x180); /* illegal instruction vector */
    w16(ram,0x100,0x4afc);
    QRXVM v;qrx_vm_init(&v,ram,sizeof ram);
    CHECK(qrx_vm_reset(&v));
    v.cpu.sr=0; v.cpu.usp=0x3f0; v.cpu.ssp=0x3d0;v.cpu.a[7]=v.cpu.usp;
    CHECK(qrx_vm_step(&v));
    CHECK(v.last_exception==4 && v.cpu.pc==0x180);
    CHECK(v.cpu.a[7]==0x3ca && v.cpu.usp==0x3f0);
    CHECK(v.cpu.sr&QRX_SR_S);
    uint32_t sr=99,pc=99;
    CHECK(qrx_vm_read(&v,0x3ca,2,&sr) && sr==0);
    CHECK(qrx_vm_read(&v,0x3cc,4,&pc) && pc==0x100);
    w16(ram,0x180,0x4e73); /* RTE */
    CHECK(qrx_vm_step(&v));
    CHECK(v.cpu.pc==0x100 && v.cpu.a[7]==0x3f0 && v.cpu.sr==0);
    return true;
}
static bool test_illegal_unhandled(void) {
    uint8_t ram[1024];vectors(ram,sizeof ram);
    w16(ram,0x100,0xffff); /* unsupported opcode -> exception 4 */
    QRXVM v;qrx_vm_init(&v,ram,sizeof ram);CHECK(qrx_vm_reset(&v));
    CHECK(qrx_vm_run(&v,10)==-1);
    CHECK(v.cpu.halted && v.cpu.fault==4 && v.last_exception==4);
    return true;
}
static bool test_trap_vector(void) {
    uint8_t ram[1024];vectors(ram,sizeof ram);
    w32(ram,(32+2)*4,0x180); /* TRAP #2 vector */
    w16(ram,0x100,0x4e42);
    QRXVM v;qrx_vm_init(&v,ram,sizeof ram);CHECK(qrx_vm_reset(&v));
    CHECK(qrx_vm_step(&v));CHECK(v.cpu.pc==0x180 && v.last_exception==34);
    uint32_t pc=0;CHECK(qrx_vm_read(&v,v.cpu.a[7]+2,4,&pc) && pc==0x102);
    return true;
}
static bool test_jit_block_and_invalidation(void) {
    uint8_t ram[1024]; vectors(ram,sizeof ram);
    w16(ram,0x100,0x7001); /* MOVEQ #1,D0 */
    w16(ram,0x102,0x4e71); /* NOP */
    w16(ram,0x104,0x7202); /* MOVEQ #2,D1 */
    w16(ram,0x106,0x4e72);w16(ram,0x108,0x2700); /* STOP */
    QRXVM oracle, jit;
    qrx_vm_init(&oracle,ram,sizeof ram);
    qrx_vm_init(&jit,ram,sizeof ram);
    CHECK(qrx_vm_reset(&oracle) && qrx_vm_reset(&jit));
    jit.jit_enabled=true;
    CHECK(qrx_vm_run(&oracle,20)==0);
    CHECK(qrx_vm_run(&jit,20)==0);
    CHECK(memcmp(&oracle.cpu,&jit.cpu,sizeof(QRXCPU))==0);
#if defined(__aarch64__)
    /* This intentionally fails on an M1 with broken MAP_JIT setup. */
    CHECK(jit.jit_executed>0 && jit.jit_compiled>0);
    jit.cpu.pc=0x100;jit.cpu.halted=0;jit.stopped=false;
    CHECK(qrx_jit_vm_block(&jit,20));
    uint64_t compiled=jit.jit_compiled;
    jit.cpu.pc=0x100;
    CHECK(qrx_jit_vm_block(&jit,20));
    CHECK(jit.jit_compiled==compiled); /* cached block re-used */
    CHECK(qrx_vm_write(&jit,0x100,2,0x7005)); /* self-modifying code */
    jit.cpu.pc=0x100;
    CHECK(qrx_jit_vm_block(&jit,20));
    CHECK(jit.cpu.d[0]==5 && jit.jit_compiled>compiled);
#else
    CHECK(jit.jit_executed==0 && jit.jit_compiled==0);
#endif
    qrx_vm_destroy(&jit);
    qrx_vm_destroy(&oracle);
    return true;
}
static bool test_mmio_hole_with_large_ram(void) {
    /* RAM >16MiB must not shadow the QRX MMIO region. */
    size_t count=17u*1024u*1024u;
    uint8_t *mem=calloc(count,1);
    if (!mem) return false;
    QRXVM v;qrx_vm_init(&v,mem,count);
    uint32_t value=0;
    bool ok=qrx_vm_read(&v,QRX_IO_BASE,4,&value) && value==QRX_IO_MAGIC &&
        !qrx_vm_write(&v,QRX_IO_BASE,4,0xf00d) &&
        !qrx_vm_write(&v,QRX_IO_BASE-2,4,0xf00d);
    qrx_vm_destroy(&v);
    free(mem);
    CHECK(ok);
    return true;
}
int main(void) {
    struct {const char *name; bool (*fn)(void);} cases[]={
        {"RAM / MMIO / endianness",test_ram_and_board},
        {"boot monitor",test_monitor},
        {"flags / conditional branches",test_flags_branches},
        {"JSR / RTS / guest memory",test_jsr_and_memory},
        {"user to supervisor exception / RTE",test_exception_user_to_supervisor},
        {"unsupported opcode fault",test_illegal_unhandled},
        {"TRAP vector delivery",test_trap_vector},
        {"JIT differential / code invalidation",test_jit_block_and_invalidation},
        {"MMIO hole in large RAM",test_mmio_hole_with_large_ram},
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        if (!cases[i].fn()) return 1;
        printf("PASS %s\n",cases[i].name);
    }
    return 0;
}
