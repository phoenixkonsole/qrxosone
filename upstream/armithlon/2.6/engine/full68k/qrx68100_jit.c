/* QRX68100 68k -> AArch64 block JIT, see qrx68100_jit.h.
 *
 * Host register plan inside translated code:
 *   x0-x6   scratch; x4-x6 survive the memory slow path
 *   x7      retired instructions       x8  remaining cycle budget
 *   x9      68k X flag (bit 0)
 *   x10-x17 D0-D7                      x19-x26 A0-A7
 *   x27     JitCtx                     x28/x29 read/write page map
 * Guest flags live in NZCV with the carry inverted against the 68k (host
 * C = !68k C), so SUBS/CMP need no fixup and every 68k condition maps onto one
 * AArch64 condition. ADDS and logic results flip it with CFINV when C is live.
 */
#include "qrx68100_jit.h"
#include "m68kcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#include <sys/mman.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#define CODE_BYTES (128u*1024u*1024u)
#define BLOCK_HASH_BITS 18u
#define BLOCK_HASH_SIZE (1u<<BLOCK_HASH_BITS)
#define ICACHE_BITS 15u
#define ICACHE_SIZE (1u<<ICACHE_BITS)
#define MAX_BLOCK_INSNS 64u
#define MAX_BLOCK_CYCLES 3000u
#define PAGE_SHIFT 12u
#define PAGE_ENTRIES (1u<<20)
#define GRANULE_SHIFT 6u

enum { F_N=1,F_Z=2,F_V=4,F_C=8,F_X=16,F_NZVC=15,F_ALL=31 };
enum { W0=0,W1=1,W2=2,W3=3,W4=4,W5=5,W6=6,RINSN=7,RCYC=8,RX=9,RD0=10,RA0=19,
       RCTX=27,RRPAGE=28,RWPAGE=29,RLR=30,RZR=31 };
enum { C_EQ=0,C_NE=1,C_CS=2,C_CC=3,C_MI=4,C_PL=5,C_VS=6,C_VC=7,C_HI=8,C_LS=9,
       C_GE=10,C_LT=11,C_GT=12,C_LE=13,C_AL=14 };

typedef struct Link { uint32_t *site, *stub; struct Link *next; } Link;
typedef struct Exit { uint32_t *site, *stub; uint32_t target; struct Link *link; struct Block *to; } Exit;
typedef struct Block {
    uint32_t pc, end;
    uint32_t *code;      /* NULL: first instruction has no native translation */
    struct Block *hash_next, *page_next;
    Link *incoming;
    Exit *exits;
    uint8_t exit_count, dead;
} Block;
typedef struct CodePage {
    uint64_t granules;   /* one bit per 64 guest bytes holding translated code */
    uint64_t fine[8];    /* one bit per 8 guest bytes: exec variables sit right next to
                          * its jump table, and every write there would walk the blocks */
    Block *blocks;
} CodePage;
typedef struct { uint64_t pc; const void *code; } ICacheEntry;

/* Offsets below are used directly by generated code. */
typedef struct JitCtx {
    uint32_t *dar;       /* 0 */
    uintptr_t *rpage;    /* 8 */
    uintptr_t *wpage;    /* 16 */
    uint64_t insns;      /* 24 */
    int64_t cycles;      /* 32 */
    uint64_t x_flag;     /* 40 */
    uint64_t nzcv;       /* 48 */
    uint64_t pc;         /* 56 */
    uint32_t *link_site; /* 64 */
    ICacheEntry *icache; /* 72 */
    void *read_helper;   /* 80 */
    void *write_helper;  /* 88 */
    uint64_t force_exit; /* 96 */
    /* Not touched by generated code from here on. */
    QRXFullCPU *cpu;
    uint32_t *code_base, *code_blocks, *code_next, *code_end;
    void (*enter)(struct JitCtx *, const void *);
    uint32_t *exit_stub, *read_stub[3], *write_stub[3], *indirect_stub;
    Block **hash;
    CodePage *pages;
    uint8_t *arena, *arena_next, *arena_end;
    uint64_t start_instructions;
    uint64_t blocks_made, native_insns, interp_insns, flushes, invalidations, slow_reads, slow_writes;
    uint64_t *opcode_misses;
    bool flush_pending, stats;
} JitCtx;
#define OFF_DAR 0
#define OFF_RPAGE 8
#define OFF_WPAGE 16
#define OFF_INSNS 24
#define OFF_CYCLES 32
#define OFF_X 40
#define OFF_NZCV 48
#define OFF_PC 56
#define OFF_LINK 64
#define OFF_ICACHE 72
#define OFF_READ 80
#define OFF_WRITE 88
#define OFF_FORCE 96
_Static_assert(__builtin_offsetof(JitCtx,force_exit)==OFF_FORCE,"generated code offset");

/* ---------- AArch64 encoders ---------- */
typedef struct Cold {
    uint32_t *branch;  /* cbz in the main path */
    uint32_t *resume;
    uint32_t pc;
    uint8_t width, write;
} Cold;
typedef struct Trans {
    JitCtx *j;
    uint32_t *p;
    bool dry, stop;
    uint32_t start, pc, ip;
    uint16_t op;
    uint8_t uses, defs, live;
    uint8_t live_after[MAX_BLOCK_INSNS];
    Cold cold[MAX_BLOCK_INSNS*20];
    unsigned cold_count;
    Exit exits[8];
    unsigned exit_count;
    struct { uint32_t *branch; uint32_t pc; int reg; bool nonzero; } bail[MAX_BLOCK_INSNS];
    unsigned bail_count;
    bool ended;
} Trans;
static inline void E(Trans *t,uint32_t ins){if(!t->dry)*t->p=ins;t->p++;}
#define R5(r) ((uint32_t)(r)&31u)
static void add3(Trans *t,int d,int n,int m){E(t,0x0B000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void adds3(Trans *t,int d,int n,int m){E(t,0x2B000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void sub3(Trans *t,int d,int n,int m){E(t,0x4B000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void subs3(Trans *t,int d,int n,int m){E(t,0x6B000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void adds_lsl(Trans *t,int d,int n,int m,unsigned s){E(t,0x2B000000u|R5(m)<<16|s<<10|R5(n)<<5|R5(d));}
static void subs_lsl(Trans *t,int d,int n,int m,unsigned s){E(t,0x6B000000u|R5(m)<<16|s<<10|R5(n)<<5|R5(d));}
static void and3(Trans *t,int d,int n,int m){E(t,0x0A000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void orr3(Trans *t,int d,int n,int m){E(t,0x2A000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void eor3(Trans *t,int d,int n,int m){E(t,0x4A000000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void mov(Trans *t,int d,int m){if(d!=m)orr3(t,d,RZR,m);}
static void movx(Trans *t,int d,int m){E(t,0xAA0003E0u|R5(m)<<16|R5(d));}
static void mvn(Trans *t,int d,int m){E(t,0x2A2003E0u|R5(m)<<16|R5(d));}
static void addi(Trans *t,int d,int n,unsigned imm){E(t,0x11000000u|(imm&0xFFFu)<<10|R5(n)<<5|R5(d));}
static void subi(Trans *t,int d,int n,unsigned imm){E(t,0x51000000u|(imm&0xFFFu)<<10|R5(n)<<5|R5(d));}
static void addsi(Trans *t,int d,int n,unsigned imm){E(t,0x31000000u|(imm&0xFFFu)<<10|R5(n)<<5|R5(d));}
static void subsi(Trans *t,int d,int n,unsigned imm){E(t,0x71000000u|(imm&0xFFFu)<<10|R5(n)<<5|R5(d));}
static void movz(Trans *t,int d,unsigned imm,unsigned hw){E(t,0x52800000u|hw<<21|(imm&0xFFFFu)<<5|R5(d));}
static void movk(Trans *t,int d,unsigned imm,unsigned hw){E(t,0x72800000u|hw<<21|(imm&0xFFFFu)<<5|R5(d));}
static void movn(Trans *t,int d,unsigned imm,unsigned hw){E(t,0x12800000u|hw<<21|(imm&0xFFFFu)<<5|R5(d));}
static void imm32(Trans *t,int d,uint32_t v){
    if((v>>16)==0)movz(t,d,v,0);
    else if((v&0xFFFFu)==0)movz(t,d,v>>16,1);
    else if((v>>16)==0xFFFFu)movn(t,d,(~v)&0xFFFFu,0);
    else{movz(t,d,v&0xFFFFu,0);movk(t,d,v>>16,1);}
}
static void ubfm(Trans *t,int d,int n,unsigned r,unsigned s){E(t,0x53000000u|r<<16|s<<10|R5(n)<<5|R5(d));}
static void sbfm(Trans *t,int d,int n,unsigned r,unsigned s){E(t,0x13000000u|r<<16|s<<10|R5(n)<<5|R5(d));}
static void bfm(Trans *t,int d,int n,unsigned r,unsigned s){E(t,0x33000000u|r<<16|s<<10|R5(n)<<5|R5(d));}
static void lsli(Trans *t,int d,int n,unsigned s){ubfm(t,d,n,(32u-s)&31u,31u-s);}
static void lsri(Trans *t,int d,int n,unsigned s){ubfm(t,d,n,s,31u);}
static void asri(Trans *t,int d,int n,unsigned s){sbfm(t,d,n,s,31u);}
static void ubfx(Trans *t,int d,int n,unsigned l,unsigned w){ubfm(t,d,n,l,l+w-1u);}
static void uxtb(Trans *t,int d,int n){ubfm(t,d,n,0,7);}
static void uxth(Trans *t,int d,int n){ubfm(t,d,n,0,15);}
static void sxtb(Trans *t,int d,int n){sbfm(t,d,n,0,7);}
static void sxth(Trans *t,int d,int n){sbfm(t,d,n,0,15);}
static void bfi(Trans *t,int d,int n,unsigned l,unsigned w){bfm(t,d,n,(32u-l)&31u,w-1u);}
static void rori(Trans *t,int d,int n,unsigned s){E(t,0x13800000u|R5(n)<<16|(s&31u)<<10|R5(n)<<5|R5(d));}
static void rev32(Trans *t,int d,int n){E(t,0x5AC00800u|R5(n)<<5|R5(d));}
static void rev16w(Trans *t,int d,int n){E(t,0x5AC00400u|R5(n)<<5|R5(d));}
static void lslv(Trans *t,int d,int n,int m){E(t,0x1AC02000u|R5(m)<<16|R5(n)<<5|R5(d));}
static void lsrv(Trans *t,int d,int n,int m){E(t,0x1AC02400u|R5(m)<<16|R5(n)<<5|R5(d));}
static void mul32(Trans *t,int d,int n,int m){E(t,0x1B007C00u|R5(m)<<16|R5(n)<<5|R5(d));}
static void smull(Trans *t,int d,int n,int m){E(t,0x9B207C00u|R5(m)<<16|R5(n)<<5|R5(d));}
static void umull(Trans *t,int d,int n,int m){E(t,0x9BA07C00u|R5(m)<<16|R5(n)<<5|R5(d));}
static void cfinv(Trans *t){E(t,0xD500401Fu);}
static void mrs_nzcv(Trans *t,int d){E(t,0xD53B4200u|R5(d));}
static void msr_nzcv(Trans *t,int s){E(t,0xD51B4200u|R5(s));}
static void cset(Trans *t,int d,int cond){E(t,0x1A800400u|31u<<16|(unsigned)(cond^1)<<12|31u<<5|R5(d));}
static void andi1(Trans *t,int d,int n){E(t,0x12000000u|R5(n)<<5|R5(d));}          /* and wd,wn,#1 */
static void eori1(Trans *t,int d,int n){E(t,0x52000000u|R5(n)<<5|R5(d));}          /* eor wd,wn,#1 */
static void andi7(Trans *t,int d,int n){E(t,0x12000000u|2u<<10|R5(n)<<5|R5(d));}   /* and wd,wn,#7 */
static void andi31(Trans *t,int d,int n){E(t,0x12000000u|4u<<10|R5(n)<<5|R5(d));}  /* and wd,wn,#31 */
static void andi63(Trans *t,int d,int n){E(t,0x12000000u|5u<<10|R5(n)<<5|R5(d));}  /* and wd,wn,#63 */
static void bfi64(Trans *t,int d,int n,unsigned lsb){E(t,0xB3400000u|((64u-lsb)&63u)<<16|R5(n)<<5|R5(d));} /* one bit */
static void ldr_page(Trans *t,int d,int table,int idx){E(t,0xF8607800u|R5(idx)<<16|R5(table)<<5|R5(d));}
static void ldrx(Trans *t,int d,int n,unsigned off){E(t,0xF9400000u|(off/8u)<<10|R5(n)<<5|R5(d));}
static void strx(Trans *t,int d,int n,unsigned off){E(t,0xF9000000u|(off/8u)<<10|R5(n)<<5|R5(d));}
static void stpw(Trans *t,int a,int b,int n,unsigned off){E(t,0x29000000u|(off/4u)<<15|R5(b)<<10|R5(n)<<5|R5(a));}
static void ldpw(Trans *t,int a,int b,int n,unsigned off){E(t,0x29400000u|(off/4u)<<15|R5(b)<<10|R5(n)<<5|R5(a));}
static void stpx(Trans *t,int a,int b,int n,int off){E(t,0xA9000000u|(((uint32_t)(off/8))&0x7Fu)<<15|R5(b)<<10|R5(n)<<5|R5(a));}
static void ldpx(Trans *t,int a,int b,int n,int off){E(t,0xA9400000u|(((uint32_t)(off/8))&0x7Fu)<<15|R5(b)<<10|R5(n)<<5|R5(a));}
static void subsp(Trans *t,unsigned n){E(t,0xD10003FFu|n<<10);}
static void addsp(Trans *t,unsigned n){E(t,0x910003FFu|n<<10);}
static uint32_t enc_b(const uint32_t *from,const uint32_t *to){return 0x14000000u|((uint32_t)(to-from)&0x03FFFFFFu);}
static void b_to(Trans *t,const uint32_t *to){E(t,enc_b(t->p,to));}
static void bl_to(Trans *t,const uint32_t *to){E(t,0x94000000u|((uint32_t)(to-t->p)&0x03FFFFFFu));}
static void br_reg(Trans *t,int n){E(t,0xD61F0000u|R5(n)<<5);}
static void blr_reg(Trans *t,int n){E(t,0xD63F0000u|R5(n)<<5);}
static void ret(Trans *t){E(t,0xD65F03C0u);}
/* Forward branches patch themselves once the target is emitted. */
static void patch_bcond(Trans *t,uint32_t *site,const uint32_t *to,int cond){
    if(!t->dry)*site=0x54000000u|((uint32_t)(to-site)&0x7FFFFu)<<5|(uint32_t)cond;
}
static void patch_cbz(Trans *t,uint32_t *site,const uint32_t *to,int r,bool nz,bool x){
    if(!t->dry)*site=(x?0xB4000000u:0x34000000u)|(nz?0x01000000u:0u)|((uint32_t)(to-site)&0x7FFFFu)<<5|R5(r);
}
static void patch_tbz(Trans *t,uint32_t *site,const uint32_t *to,int r,unsigned bit,bool nz){
    if(!t->dry)*site=(nz?0x37000000u:0x36000000u)|(bit&32u)<<26|(bit&31u)<<19|((uint32_t)(to-site)&0x3FFFu)<<5|R5(r);
}
static uint32_t *hole(Trans *t){uint32_t *s=t->p;E(t,0xD503201Fu);return s;}

/* ---------- guest helpers called from translated code ---------- */
static JitCtx *the_ctx;
static void jit_notify_fwd(QRXFullCPU *c,uint32_t addr,uint32_t len);
static inline void request_exit_if_needed(JitCtx *j){
    QRXFullCPU *c=j->cpu;
    if(c->mem_fault||c->vm.cpu.halted||m68ki_cpu.nmi_pending||CPU_INT_LEVEL>FLAG_INT_MASK)j->force_exit=1;
}
static uint64_t read_helper(uint32_t addr,uint32_t width,uint32_t pc){
    JitCtx *j=the_ctx;QRXFullCPU *c=j->cpu;uint32_t out=0;
    j->slow_reads++;
    REG_PC=pc;REG_PPC=pc;
    if(!qrx_full_bus_read(c,addr,width,&out)){c->mem_fault=true;c->fault_address=addr;out=0xFFFFFFFFu;}
    if(c->smp_strong)__atomic_thread_fence(__ATOMIC_ACQUIRE);
    request_exit_if_needed(j);
    return out;
}
static void write_helper(uint32_t addr,uint32_t width,uint32_t pc,uint32_t value){
    JitCtx *j=the_ctx;QRXFullCPU *c=j->cpu;
    j->slow_writes++;
    /* Plain data next to translated code is the common case by far: only the
     * write map's page gating sent it here. */
    uintptr_t base=c->rpage[addr>>PAGE_SHIFT];
    if(base&&(uint64_t)addr+width<=c->vm.ram.size&&(addr&0xFFFu)<=0x1000u-width&&
       (addr>=c->rom_overlay*8u)&&
       !(width==4&&((value&0xFFFFF000u)==0x00BFE000u||value==709379u||value==715909u))){
        uint8_t *p=(uint8_t*)(base+addr);
        if(c->smp_strong)__atomic_thread_fence(__ATOMIC_RELEASE);
        if(width==4){uint32_t v=__builtin_bswap32(value);memcpy(p,&v,4);}
        else if(width==2){uint16_t v=__builtin_bswap16((uint16_t)value);memcpy(p,&v,2);}
        else *p=(uint8_t)value;
        jit_notify_fwd(c,addr,width);
        return;
    }
    REG_PC=pc;REG_PPC=pc;
    if(!qrx_full_bus_write(c,addr,width,value)){c->mem_fault=true;c->fault_address=addr;}
    request_exit_if_needed(j);
    /* The host door can take a millisecond; timers must get their turn after it. */
    if(addr>=0xFF000880u&&addr<0xFF000900u)j->force_exit=1;
    /* Starting the second core switches translations to ordered accesses. */
    if(addr>=0xFF000600u&&addr<0xFF000640u)j->force_exit=1;
}

/* ---------- runtime stubs ---------- */
static void emit_save_guest(Trans *t){
    ldrx(t,W0,RCTX,OFF_DAR);
    for(int i=0;i<8;i+=2)stpw(t,RD0+i,RD0+i+1,W0,(unsigned)(i*4));
    for(int i=0;i<8;i+=2)stpw(t,RA0+i,RA0+i+1,W0,(unsigned)(32+i*4));
}
static void build_stubs(JitCtx *j){
    Trans *t=calloc(1,sizeof(*t));
    t->j=j;t->p=j->code_base;
    /* enter(ctx,code) */
    j->enter=(void(*)(JitCtx*,const void*))(void*)t->p;
    subsp(t,96);
    stpx(t,19,20,31,0);stpx(t,21,22,31,16);stpx(t,23,24,31,32);
    stpx(t,25,26,31,48);stpx(t,27,28,31,64);stpx(t,29,30,31,80);
    movx(t,RCTX,0);
    ldrx(t,RRPAGE,RCTX,OFF_RPAGE);ldrx(t,RWPAGE,RCTX,OFF_WPAGE);
    ldrx(t,RINSN,RCTX,OFF_INSNS);ldrx(t,RCYC,RCTX,OFF_CYCLES);ldrx(t,RX,RCTX,OFF_X);
    ldrx(t,W0,RCTX,OFF_NZCV);msr_nzcv(t,W0);
    ldrx(t,W0,RCTX,OFF_DAR);
    for(int i=0;i<8;i+=2)ldpw(t,RD0+i,RD0+i+1,W0,(unsigned)(i*4));
    for(int i=0;i<8;i+=2)ldpw(t,RA0+i,RA0+i+1,W0,(unsigned)(32+i*4));
    br_reg(t,W1);
    /* exit: w0=next pc, x1=patchable site or 0 */
    j->exit_stub=t->p;
    strx(t,W0,RCTX,OFF_PC);strx(t,W1,RCTX,OFF_LINK);
    strx(t,RINSN,RCTX,OFF_INSNS);strx(t,RCYC,RCTX,OFF_CYCLES);strx(t,RX,RCTX,OFF_X);
    mrs_nzcv(t,W0);strx(t,W0,RCTX,OFF_NZCV);
    emit_save_guest(t);
    ldpx(t,19,20,31,0);ldpx(t,21,22,31,16);ldpx(t,23,24,31,32);
    ldpx(t,25,26,31,48);ldpx(t,27,28,31,64);ldpx(t,29,30,31,80);
    addsp(t,96);ret(t);
    /* indirect: w0=target pc. Touches no flags. */
    j->indirect_stub=t->p;
    ldrx(t,W1,RCTX,OFF_ICACHE);
    ubfx(t,W2,W0,1,ICACHE_BITS);
    E(t,0x8B021021u);                                             /* add x1,x1,x2,lsl #4 */
    ldpx(t,W3,W2,W1,0);
    E(t,0xCA000063u);                                             /* eor x3,x3,x0 */
    {uint32_t *miss=hole(t);br_reg(t,W2);patch_cbz(t,miss,t->p,W3,true,true);}
    E(t,0xD2800001u);                                             /* mov x1,#0 */
    b_to(t,j->exit_stub);
    /* read stubs: w1=addr, w3=pc; result in w0. x4-x6 survive. */
    static const uint8_t widths[3]={1,2,4};
    for(int k=0;k<3;k++){
        j->read_stub[k]=t->p;
        subsp(t,80);
        stpx(t,4,5,31,0);stpx(t,6,7,31,16);stpx(t,8,9,31,32);stpx(t,29,30,31,48);
        mrs_nzcv(t,W2);strx(t,W2,31,64);
        emit_save_guest(t);
        mov(t,W0,W1);movz(t,W1,widths[k],0);mov(t,W2,W3);
        ldrx(t,W3,RCTX,OFF_READ);blr_reg(t,W3);
        ldrx(t,W2,31,64);msr_nzcv(t,W2);
        ldpx(t,4,5,31,0);ldpx(t,6,7,31,16);ldpx(t,8,9,31,32);ldpx(t,29,30,31,48);
        addsp(t,80);
        ldrx(t,W1,RCTX,OFF_DAR);
        for(int i=0;i<8;i+=2)ldpw(t,RD0+i,RD0+i+1,W1,(unsigned)(i*4));
        ldrx(t,W1,RCTX,OFF_FORCE);
        {uint32_t *skip=hole(t);E(t,0x92800008u);patch_cbz(t,skip,t->p,W1,false,true);} /* movn x8,#0 */
        ret(t);
    }
    /* write stubs: w0=value, w1=addr, w3=pc */
    for(int k=0;k<3;k++){
        j->write_stub[k]=t->p;
        subsp(t,80);
        stpx(t,4,5,31,0);stpx(t,6,7,31,16);stpx(t,8,9,31,32);stpx(t,29,30,31,48);
        mrs_nzcv(t,W2);strx(t,W2,31,64);
        mov(t,W4,W0);emit_save_guest(t);
        mov(t,W0,W1);movz(t,W1,widths[k],0);mov(t,W2,W3);mov(t,W3,W4);
        ldrx(t,W4,RCTX,OFF_WRITE);blr_reg(t,W4);
        ldrx(t,W2,31,64);msr_nzcv(t,W2);
        ldpx(t,4,5,31,0);ldpx(t,6,7,31,16);ldpx(t,8,9,31,32);ldpx(t,29,30,31,48);
        addsp(t,80);
        ldrx(t,W1,RCTX,OFF_DAR);
        for(int i=0;i<8;i+=2)ldpw(t,RD0+i,RD0+i+1,W1,(unsigned)(i*4));
        ldrx(t,W1,RCTX,OFF_FORCE);
        {uint32_t *skip=hole(t);E(t,0x92800008u);patch_cbz(t,skip,t->p,W1,false,true);}
        ret(t);
    }
    j->code_next=t->p;
    free(t);
}

/* ---------- guest memory emission ---------- */
static inline unsigned width_index(unsigned w){return w==1?0u:w==2?1u:2u;}
/* Ordered accesses (ldapr/stlr) fault on unaligned addresses, which a 68040 allows. Those go
 * to the helper by clearing the page base. Host flags hold guest flags, so no compare here. */
static void strong_unaligned_to_cold(Trans *t,unsigned width){
    if(width>=2){E(t,0x36000041u);E(t,0xAA1F03E2u);}      /* tbz w1,#0,+8 ; mov x2,xzr */
    if(width==4){E(t,0x36080041u);E(t,0xAA1F03E2u);}      /* tbz w1,#1,+8 ; mov x2,xzr */
}
/* w1=addr -> w0=value (zero-extended to width). Clobbers x2,x3. */
static void mem_read(Trans *t,unsigned width){
    lsri(t,W2,W1,PAGE_SHIFT);
    ldr_page(t,W2,RRPAGE,W2);
    if(t->j->cpu->smp_strong)strong_unaligned_to_cold(t,width);
    uint32_t *site=hole(t);
    if(t->j->cpu->smp_strong){
        /* With a second core running, keep the 68040's bus order (ldapr after add x2,x2,x1). */
        E(t,0x8B010042u);
        if(width==4){E(t,0xB8BFC040u);rev32(t,W0,W0);}
        else if(width==2){E(t,0x78BFC040u);rev16w(t,W0,W0);}
        else E(t,0x38BFC040u);
    }
    else if(width==4){E(t,0xB8616840u);rev32(t,W0,W0);}  /* ldr w0,[x2,x1] */
    else if(width==2){E(t,0x78616840u);rev16w(t,W0,W0);}/* ldrh w0,[x2,x1] */
    else E(t,0x38616840u);                              /* ldrb w0,[x2,x1] */
    if(!t->dry&&t->cold_count<sizeof(t->cold)/sizeof(t->cold[0]))
        t->cold[t->cold_count]=(Cold){site,t->p,t->pc,(uint8_t)width,0};
    t->cold_count++;
}
/* w0=value, w1=addr. Clobbers x2,x3; w0 survives only on the fast path. */
static void mem_write(Trans *t,unsigned width){
    lsri(t,W2,W1,QRX_WPAGE_SHIFT);
    ldr_page(t,W2,RWPAGE,W2);
    if(t->j->cpu->smp_strong)strong_unaligned_to_cold(t,width);
    uint32_t *site=hole(t);
    if(t->j->cpu->smp_strong){
        E(t,0x8B010042u);                                 /* add x2,x2,x1, then stlr */
        if(width==4){rev32(t,W3,W0);E(t,0x889FFC43u);}
        else if(width==2){rev16w(t,W3,W0);E(t,0x489FFC43u);}
        else E(t,0x089FFC40u);
    }
    else if(width==4){rev32(t,W3,W0);E(t,0xB8216843u);}   /* str w3,[x2,x1] */
    else if(width==2){rev16w(t,W3,W0);E(t,0x78216843u);} /* strh w3,[x2,x1] */
    else E(t,0x38216840u);                               /* strb w0,[x2,x1] */
    if(!t->dry&&t->cold_count<sizeof(t->cold)/sizeof(t->cold[0]))
        t->cold[t->cold_count]=(Cold){site,t->p,t->pc,(uint8_t)width,1};
    t->cold_count++;
}

/* ---------- guest code fetch ---------- */
static inline bool fetch_ok(const Trans *t,uint32_t a,unsigned n){
    return t->j->cpu->rpage[a>>PAGE_SHIFT] && ((a&0xFFFu)+n<=0x1000u);
}
static uint32_t fetch16(Trans *t){
    uint32_t a=t->ip;t->ip+=2;
    if(!fetch_ok(t,a,2)){t->stop=true;return 0;}
    const uint8_t *p=(const uint8_t*)(t->j->cpu->rpage[a>>PAGE_SHIFT]+a);
    return (uint32_t)p[0]<<8|p[1];
}
static uint32_t fetch32(Trans *t){uint32_t h=fetch16(t);return h<<16|fetch16(t);}

static inline int dn(unsigned n){return RD0+(int)n;}
static inline int an(unsigned n){return RA0+(int)n;}
static inline int dareg(unsigned n){return n<8?dn(n):an(n-8);}
#define LIVE(f) (t->live&(f))
#define SIZE_SHIFT(w) (32u-(w)*8u)

/* Address of a memory EA into w1. mode/reg as encoded; false when unsupported. */
static bool add_const(Trans *t,int d,int n,int32_t v){
    if(v==0){mov(t,d,n);return true;}
    if(v>0&&v<4096){addi(t,d,n,(unsigned)v);return true;}
    if(v<0&&v>-4096){subi(t,d,n,(unsigned)-v);return true;}
    imm32(t,W2,(uint32_t)v);add3(t,d,n,W2);
    return true;
}
static void add_index(Trans *t,int d,int base,unsigned ext){
    unsigned xn=(ext>>12)&15u,scale=(ext>>9)&3u;
    unsigned opt=(ext&0x800u)?2u:5u; /* uxtw (long is 32-bit, so plain) / sxth */
    E(t,0x0B200000u|R5(dareg(xn))<<16|opt<<13|scale<<10|R5(base)<<5|R5(d));
}
static bool ea_index(Trans *t,int base_reg,bool have_base,uint32_t pc_base){
    unsigned ext=fetch16(t);
    if(!(ext&0x100u)){ /* brief */
        int32_t d8=(int8_t)ext;
        if(have_base)add_index(t,W1,base_reg,ext);
        else{imm32(t,W2,pc_base);add_index(t,W1,W2,ext);}
        return add_const(t,W1,W1,d8);
    }
    /* Full format without memory indirection. */
    bool bs=(ext&0x80u)!=0,is=(ext&0x40u)!=0;
    unsigned bdsz=(ext>>4)&3u,iis=ext&7u;
    if(iis||bdsz==0)return false;
    int32_t bd=0;
    if(bdsz==2)bd=(int16_t)fetch16(t);else if(bdsz==3)bd=(int32_t)fetch32(t);
    int b=RZR;
    if(!bs){
        if(have_base)b=base_reg;
        else{imm32(t,W2,pc_base);b=W2;}
    }
    if(!is){
        /* Rn=31 is SP in this encoding, so a suppressed base needs a real zero. */
        if(b==RZR){movz(t,W2,0,0);b=W2;}
        add_index(t,W1,b,ext);
    }else mov(t,W1,b);
    imm32(t,W2,(uint32_t)bd);add3(t,W1,W1,W2);
    return true;
}
static bool ea_addr(Trans *t,unsigned mode,unsigned reg,unsigned width){
    switch(mode){
    case 2:mov(t,W1,an(reg));return true;
    case 3:{
        unsigned inc=(width==1&&reg==7)?2u:width;
        mov(t,W1,an(reg));addi(t,an(reg),an(reg),inc);return true;
    }
    case 4:{
        unsigned dec=(width==1&&reg==7)?2u:width;
        subi(t,an(reg),an(reg),dec);mov(t,W1,an(reg));return true;
    }
    case 5:{int32_t d=(int16_t)fetch16(t);return add_const(t,W1,an(reg),d);}
    case 6:return ea_index(t,an(reg),true,0);
    case 7:
        switch(reg){
        case 0:imm32(t,W1,(uint32_t)(int32_t)(int16_t)fetch16(t));return true;
        case 1:imm32(t,W1,fetch32(t));return true;
        case 2:{uint32_t base=t->ip;int32_t d=(int16_t)fetch16(t);imm32(t,W1,base+(uint32_t)d);return true;}
        case 3:{uint32_t base=t->ip;return ea_index(t,0,false,base);}
        default:return false;
        }
    default:return false;
    }
}
static bool ea_is_mem(unsigned mode,unsigned reg){
    if(mode<2)return false;
    return !(mode==7&&reg>=4);
}
static bool ea_is_alterable_mem(unsigned mode,unsigned reg){
    return mode>=2&&mode<=7&&!(mode==7&&reg>1);
}
/* Source operand. Returns a register holding the zero-extended value: the
 * pinned register for a whole long, otherwise `tmp`. */
static int src_operand(Trans *t,unsigned mode,unsigned reg,unsigned width,int tmp,bool *ok){
    if(mode==0){
        if(width==4)return dn(reg);
        if(width==2)uxth(t,tmp,dn(reg));else uxtb(t,tmp,dn(reg));
        return tmp;
    }
    if(mode==1){
        if(width==4)return an(reg);
        uxth(t,tmp,an(reg));
        return tmp;
    }
    if(mode==7&&reg==4){
        uint32_t v;
        if(width==4)v=fetch32(t);
        else v=fetch16(t)&(width==1?0xFFu:0xFFFFu);
        imm32(t,tmp,v);
        return tmp;
    }
    if(!ea_addr(t,mode,reg,width)){*ok=false;return tmp;}
    mem_read(t,width);
    mov(t,tmp,W0);
    return tmp;
}
static void store_dn(Trans *t,unsigned reg,int src,unsigned width){
    if(width==4)mov(t,dn(reg),src);
    else if(width==2)bfi(t,dn(reg),src,0,16);
    else bfi(t,dn(reg),src,0,8);
}

/* ---------- flag emission ---------- */
/* N,Z from the low `width` bytes of r; V=0, 68k C=0. */
static void flags_logic(Trans *t,int r,unsigned width){
    if(!LIVE(F_NZVC))return;
    adds_lsl(t,RZR,RZR,r,SIZE_SHIFT(width));
    if(LIVE(F_C))cfinv(t);
}
/* Replace host C with a guest carry bit held in `bit` (0/1), N,Z,V kept. */
static void set_guest_carry(Trans *t,int bit){
    eori1(t,W3,bit);
    mrs_nzcv(t,W6);
    bfi64(t,W6,W3,29);
    msr_nzcv(t,W6);
}
/* Top-aligned add/sub: r=a+b. Sets result in w0 zero-extended to width. */
static void alu_add(Trans *t,int a,int b,unsigned width){
    bool flags=LIVE(F_ALL)!=0;
    if(width==4){
        if(flags)adds3(t,W0,a,b);else add3(t,W0,a,b);
    }else{
        unsigned s=SIZE_SHIFT(width);
        if(flags){lsli(t,W2,a,s);adds_lsl(t,W2,W2,b,s);lsri(t,W0,W2,s);}
        else add3(t,W0,a,b);
    }
    if(LIVE(F_X))cset(t,RX,C_CS);
    if(LIVE(F_C))cfinv(t);
}
static void alu_sub(Trans *t,int a,int b,unsigned width){
    bool flags=LIVE(F_ALL)!=0;
    if(width==4){
        if(flags)subs3(t,W0,a,b);else sub3(t,W0,a,b);
    }else{
        unsigned s=SIZE_SHIFT(width);
        if(flags){lsli(t,W2,a,s);subs_lsl(t,W2,W2,b,s);lsri(t,W0,W2,s);}
        else sub3(t,W0,a,b);
    }
    if(LIVE(F_X))cset(t,RX,C_CC);
}
static void alu_cmp(Trans *t,int a,int b,unsigned width){
    if(!LIVE(F_NZVC))return;
    if(width==4)subs3(t,RZR,a,b);
    else{unsigned s=SIZE_SHIFT(width);lsli(t,W2,a,s);subs_lsl(t,RZR,W2,b,s);}
}

/* ---------- block control ---------- */
static const uint8_t cond_map[16]={C_AL,C_AL,C_HI,C_LS,C_CS,C_CC,C_NE,C_EQ,C_VC,C_VS,C_PL,C_MI,C_GE,C_LT,C_GT,C_LE};
static const uint8_t cond_uses[16]={0,0,F_C|F_Z,F_C|F_Z,F_C,F_C,F_Z,F_Z,F_V,F_V,F_N,F_N,F_N|F_V,F_N|F_V,F_N|F_V|F_Z,F_N|F_V|F_Z};
static uint32_t *exit_to(Trans *t,uint32_t target){
    uint32_t *site=hole(t);
    if(t->exit_count<8){
        t->exits[t->exit_count].site=site;
        t->exits[t->exit_count].target=target;
        t->exit_count++;
    }
    return site;
}
static void end_indirect(Trans *t){
    b_to(t,t->j->indirect_stub);
}
/* Leave the block at this instruction, nothing changed: Musashi takes it. */
static void bail_if_reg(Trans *t,int reg,bool nonzero){
    if(t->bail_count<MAX_BLOCK_INSNS){
        t->bail[t->bail_count].branch=hole(t);
        t->bail[t->bail_count].pc=t->pc;
        t->bail[t->bail_count].reg=reg;
        t->bail[t->bail_count].nonzero=nonzero;
        t->bail_count++;
    }
}
#define USES(f) (t->uses|=(uint8_t)(f))
#define DEFS(f) (t->defs|=(uint8_t)(f))

/* ---------- MOVEM ---------- */
static bool tr_movem(Trans *t,unsigned size_l,bool to_regs){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,width=size_l?4u:2u;
    unsigned mask=fetch16(t);
    if(mode==4&&!to_regs){ /* -(An): mask bit 0 = A7 */
        mov(t,W5,an(reg));
        for(int i=0;i<16;i++)if(mask&(1u<<i)){
            subi(t,W5,W5,width);
            mov(t,W1,W5);
            mov(t,W0,dareg(15u-(unsigned)i));
            mem_write(t,width);
        }
        mov(t,an(reg),W5);
        return true;
    }
    if(mode==3&&to_regs){
        mov(t,W5,an(reg));
        for(int i=0;i<16;i++)if(mask&(1u<<i)){
            mov(t,W1,W5);
            mem_read(t,width);
            int r=dareg((unsigned)i);
            if(width==2)sxth(t,r,W0);else mov(t,r,W0);
            addi(t,W5,W5,width);
        }
        mov(t,an(reg),W5);
        return true;
    }
    if(mode==4||mode==3)return false;
    if(!ea_is_mem(mode,reg)||(mode==7&&reg==4))return false;
    if(!to_regs&&!ea_is_alterable_mem(mode,reg))return false;
    if(!ea_addr(t,mode,reg,width))return false;
    mov(t,W5,W1);
    for(int i=0;i<16;i++)if(mask&(1u<<i)){
        mov(t,W1,W5);
        if(to_regs){
            mem_read(t,width);
            int r=dareg((unsigned)i);
            if(width==2)sxth(t,r,W0);else mov(t,r,W0);
        }else{
            mov(t,W0,dareg((unsigned)i));
            mem_write(t,width);
        }
        addi(t,W5,W5,width);
    }
    return true;
}

/* ---------- shifts ---------- */
static bool tr_shift_imm(Trans *t,unsigned width){
    unsigned count=(t->op>>9)&7u;if(!count)count=8;
    unsigned type=(t->op>>3)&3u,left=(t->op>>8)&1u,reg=t->op&7u;
    int d=dn(reg);
    unsigned bits=width*8u;
    if(type==2){ /* ROXL/ROXR */
        return false;
    }
    DEFS(F_N|F_Z|F_V|F_C);
    if(type!=3)DEFS(F_X);
    /* value in w0, zero- or sign-extended to 32 bits */
    if(type==0&&!left){
        if(width==4)mov(t,W0,d);else if(width==2)sxth(t,W0,d);else sxtb(t,W0,d);
    }else{
        if(width==4)mov(t,W0,d);else if(width==2)uxth(t,W0,d);else uxtb(t,W0,d);
    }
    bool carry=LIVE(F_C|F_X)!=0;
    if(type==3){ /* ROL/ROR */
        if(width==4)rori(t,W1,W0,left?32u-count:count);
        else{
            unsigned c=count%bits;
            if(!c)mov(t,W1,W0);
            else{
                /* double the value so a 32-bit ROR acts on it */
                if(width==2)bfi(t,W0,W0,16,16);
                else{bfi(t,W0,W0,8,8);bfi(t,W0,W0,16,16);}
                rori(t,W1,W0,left?32u-c:c);
            }
        }
        if(carry){
            if(left)andi1(t,W4,W1);
            else ubfx(t,W4,W1,bits-1u,1);
        }
        if(width==4)mov(t,W0,W1);else if(width==2)uxth(t,W0,W1);else uxtb(t,W0,W1);
    }else if(left){
        if(carry){
            /* last bit shifted out: original bit (bits-count); for 8-bit and
             * larger counts the zero-extended value gives 0 as required */
            if(count<=bits)ubfx(t,W4,W0,bits-count,1);else movz(t,W4,0,0);
        }
        if(type==0&&LIVE(F_V)){
            /* ASL: V set when the top count+1 bits were not all equal */
            lsli(t,W2,W0,32u-bits);
            if(count+1u>=32u)mov(t,W3,W2);
            else asri(t,W3,W2,31u-count);
            addi(t,W3,W3,1);
            E(t,0x7100047Fu);       /* cmp w3,#1 -> HI when not 0 or -1 */
            cset(t,W6,C_HI);
            lsli(t,W0,W0,count);
            if(width==4){/* result ready */}else if(width==2)uxth(t,W0,W0);else uxtb(t,W0,W0);
            adds_lsl(t,RZR,RZR,W0,SIZE_SHIFT(width));
            mrs_nzcv(t,W3);
            bfi64(t,W3,W6,28);
            msr_nzcv(t,W3);
            if(LIVE(F_C))set_guest_carry(t,W4);
            if(LIVE(F_X))mov(t,RX,W4);
            store_dn(t,reg,W0,width);
            return true;
        }
        lsli(t,W0,W0,count);
        if(width==2)uxth(t,W0,W0);else if(width==1)uxtb(t,W0,W0);
    }else{
        if(carry)ubfx(t,W4,W0,count-1u,1);
        if(type==0)asri(t,W0,W0,count);else lsri(t,W0,W0,count);
        if(width==2)uxth(t,W0,W0);else if(width==1)uxtb(t,W0,W0);
    }
    if(LIVE(F_N|F_Z|F_V|F_C)){
        adds_lsl(t,RZR,RZR,W0,SIZE_SHIFT(width));
        if(LIVE(F_C))set_guest_carry(t,W4);
    }
    if(type!=3&&LIVE(F_X))mov(t,RX,W4);
    store_dn(t,reg,W0,width);
    return true;
}
static bool tr_shift_reg(Trans *t,unsigned width){
    unsigned type=(t->op>>3)&3u,left=(t->op>>8)&1u,reg=t->op&7u,cr=(t->op>>9)&7u;
    int d=dn(reg);
    unsigned bits=width*8u;
    if(type==2||type==3)return false;
    if(type==0&&left)return false; /* ASL Dx,Dy: the V rule needs a real loop */
    /* X keeps its old value when the count is zero. */
    USES(F_X);
    DEFS(F_N|F_Z|F_V|F_C|F_X);
    andi63(t,W5,dn(cr));
    if(left){
        if(width==4)mov(t,W0,d);else if(width==2)uxth(t,W0,d);else uxtb(t,W0,d);
        E(t,0x9AC52001u);              /* lslv x1,x0,x5 on the zero-extended value */
        if(width==4)mov(t,W0,W1);else if(width==2)uxth(t,W0,W1);else uxtb(t,W0,W1);
        /* carry = bit `bits` of the 64-bit result */
        E(t,0xD3400000u|(bits<<16)|(bits<<10)|R5(W1)<<5|R5(W4)); /* ubfx x4,x1,#bits,#1 */
    }else{
        /* Value goes in one bit up so bit 0 of the shift is the last bit out. */
        if(type==0){
            if(width==4)E(t,0x93407C00u|R5(d)<<5|R5(W1));        /* sxtw x1,d */
            else if(width==2)E(t,0x93403C00u|R5(d)<<5|R5(W1));   /* sxth x1,d */
            else E(t,0x93401C00u|R5(d)<<5|R5(W1));               /* sxtb x1,d */
        }else{
            if(width==4)E(t,0x2A0003E0u|R5(d)<<16|R5(W1));       /* mov w1,d */
            else if(width==2)uxth(t,W1,d);else uxtb(t,W1,d);
        }
        E(t,0xD37FF821u);                                        /* lsl x1,x1,#1 */
        if(type==0)E(t,0x9AC52821u);else E(t,0x9AC52421u);       /* asr/lsr x1,x1,x5 */
        andi1(t,W4,W1);
        E(t,0xD341FC21u);                                        /* lsr x1,x1,#1 */
        if(width==4)mov(t,W0,W1);else if(width==2)uxth(t,W0,W1);else uxtb(t,W0,W1);
    }
    /* A zero count yields carry 0 above and leaves X alone. */
    store_dn(t,reg,W0,width);
    if(LIVE(F_X)){
        uint32_t *zero=hole(t);
        mov(t,RX,W4);
        patch_cbz(t,zero,t->p,W5,false,false);
    }
    if(LIVE(F_N|F_Z|F_V|F_C)){
        adds_lsl(t,RZR,RZR,W0,SIZE_SHIFT(width));
        if(LIVE(F_C))set_guest_carry(t,W4);
    }
    return true;
}

/* ---------- instruction translation ---------- */
static bool tr_bcc(Trans *t,unsigned cc){
    int32_t disp=(int8_t)(t->op&0xFFu);
    uint32_t base=t->pc+2u;
    if(disp==0)disp=(int16_t)fetch16(t);
    else if(disp==-1)disp=(int32_t)fetch32(t);
    uint32_t target=base+(uint32_t)disp;
    if(target&1u)return false;
    if(cc==1){ /* BSR */
        subi(t,an(7),an(7),4);
        mov(t,W1,an(7));
        imm32(t,W0,t->ip);
        mem_write(t,4);
        exit_to(t,target);
        t->ended=true;
        return true;
    }
    if(cc==0){exit_to(t,target);t->ended=true;return true;}
    USES(cond_uses[cc]);
    uint32_t *branch=hole(t);
    exit_to(t,t->ip);
    uint32_t *taken=t->p;
    exit_to(t,target);
    patch_bcond(t,branch,taken,cond_map[cc]);
    t->ended=true;
    return true;
}
static bool tr_dbcc(Trans *t,unsigned cc){
    unsigned reg=t->op&7u;
    int d=dn(reg);
    uint32_t base=t->pc+2u;
    int32_t disp=(int16_t)fetch16(t);
    uint32_t target=base+(uint32_t)disp;
    if(target&1u)return false;
    uint32_t *skip=NULL;
    if(cc==0){exit_to(t,t->ip);t->ended=true;return true;} /* DBT never branches */
    if(cc!=1){USES(cond_uses[cc]);skip=hole(t);}
    uxth(t,W3,d);
    subi(t,W2,d,1);
    bfi(t,d,W2,0,16);
    uint32_t *expired=hole(t);
    exit_to(t,target);
    uint32_t *out=t->p;
    exit_to(t,t->ip);
    patch_cbz(t,expired,out,W3,false,false);
    if(skip)patch_bcond(t,skip,out,cond_map[cc]);
    t->ended=true;
    return true;
}
static bool tr_scc(Trans *t,unsigned cc){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u;
    if(mode==1)return false; /* DBcc handled by the caller */
    if(!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
    if(cc>1)USES(cond_uses[cc]);
    if(mode==0){
        if(cc==0)imm32(t,W0,0xFF);
        else if(cc==1)imm32(t,W0,0);
        else{cset(t,W0,cond_map[cc]);E(t,0x4B0003E0u|R5(W0)<<16|R5(W0));}
        bfi(t,dn(reg),W0,0,8);
        return true;
    }
    if(!ea_addr(t,mode,reg,1))return false;
    if(cc==0)imm32(t,W0,0xFF);
    else if(cc==1)imm32(t,W0,0);
    else{cset(t,W0,cond_map[cc]);E(t,0x4B0003E0u|R5(W0)<<16|R5(W0));}
    mem_write(t,1);
    return true;
}
/* read-modify-write helper: leaves value in w0, address kept in w5 */
static bool rmw_load(Trans *t,unsigned mode,unsigned reg,unsigned width){
    if(!ea_addr(t,mode,reg,width))return false;
    mov(t,W5,W1);
    mem_read(t,width);
    return true;
}
static void rmw_store(Trans *t,unsigned width){
    mov(t,W1,W5);
    mem_write(t,width);
}
static const uint8_t size_table[4]={1,2,4,0};
/* Common form for ADD/SUB/AND/OR/EOR to an EA: 0=add 1=sub 2=and 3=or 4=eor */
static void apply_alu(Trans *t,unsigned kind,int a,int b,unsigned width){
    switch(kind){
    case 0:alu_add(t,a,b,width);break;
    case 1:alu_sub(t,a,b,width);break;
    case 2:and3(t,W0,a,b);flags_logic(t,W0,width);break;
    case 3:orr3(t,W0,a,b);flags_logic(t,W0,width);break;
    default:eor3(t,W0,a,b);flags_logic(t,W0,width);break;
    }
}
static void alu_defs(Trans *t,unsigned kind){
    DEFS(F_NZVC);
    if(kind<2)DEFS(F_X);
}
static bool tr_alu_to_dn(Trans *t,unsigned kind,unsigned width){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,dst=(t->op>>9)&7u;
    if(mode==1&&(width==1||kind>=2))return false;
    alu_defs(t,kind);
    bool ok=true;
    int s=src_operand(t,mode,reg,width,W4,&ok);
    if(!ok)return false;
    int a=dn(dst);
    if(width!=4){if(width==2)uxth(t,W6,a);else uxtb(t,W6,a);a=W6;}
    apply_alu(t,kind,a,s,width);
    store_dn(t,dst,W0,width);
    return true;
}
static bool tr_alu_to_ea(Trans *t,unsigned kind,unsigned width){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,src=(t->op>>9)&7u;
    if(kind==4&&mode==0){ /* EOR Dx,Dy */
        DEFS(F_NZVC);
        eor3(t,W0,dn(reg),dn(src));
        store_dn(t,reg,W0,width);
        flags_logic(t,W0,width);
        return true;
    }
    if(!ea_is_alterable_mem(mode,reg))return false;
    alu_defs(t,kind);
    if(!rmw_load(t,mode,reg,width))return false;
    int s=dn(src);
    if(width!=4){if(width==2)uxth(t,W4,s);else uxtb(t,W4,s);s=W4;}
    apply_alu(t,kind,W0,s,width);
    rmw_store(t,width);
    return true;
}
static bool tr_alu_imm(Trans *t,unsigned kind,unsigned width){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u;
    if(mode==1)return false;
    if(!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
    uint32_t v=width==4?fetch32(t):(fetch16(t)&(width==1?0xFFu:0xFFFFu));
    alu_defs(t,kind);
    imm32(t,W4,v);
    if(mode==0){
        int a=dn(reg);
        if(width!=4){if(width==2)uxth(t,W6,a);else uxtb(t,W6,a);a=W6;}
        apply_alu(t,kind,a,W4,width);
        store_dn(t,reg,W0,width);
        return true;
    }
    if(!rmw_load(t,mode,reg,width))return false;
    apply_alu(t,kind,W0,W4,width);
    rmw_store(t,width);
    return true;
}
static bool tr_addq_subq(Trans *t,unsigned width){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,v=(t->op>>9)&7u;
    bool sub=(t->op&0x100u)!=0;
    if(!v)v=8;
    if(mode==1){
        if(width==1)return false;
        if(sub)subi(t,an(reg),an(reg),v);else addi(t,an(reg),an(reg),v);
        return true;
    }
    if(!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
    DEFS(F_NZVC|F_X);
    if(mode==0&&width==4){
        int d=dn(reg);
        if(LIVE(F_ALL)){
            if(sub){subsi(t,d,d,v);if(LIVE(F_X))cset(t,RX,C_CC);}
            else{addsi(t,d,d,v);if(LIVE(F_X))cset(t,RX,C_CS);if(LIVE(F_C))cfinv(t);}
        }else if(sub)subi(t,d,d,v);else addi(t,d,d,v);
        return true;
    }
    imm32(t,W4,v);
    if(mode==0){
        int a=dn(reg);
        if(width==2)uxth(t,W6,a);else uxtb(t,W6,a);
        if(sub)alu_sub(t,W6,W4,width);else alu_add(t,W6,W4,width);
        store_dn(t,reg,W0,width);
        return true;
    }
    if(!rmw_load(t,mode,reg,width))return false;
    if(sub)alu_sub(t,W0,W4,width);else alu_add(t,W0,W4,width);
    rmw_store(t,width);
    return true;
}
static bool tr_cmp_common(Trans *t,int a,unsigned mode,unsigned reg,unsigned width){
    DEFS(F_NZVC);
    bool ok=true;
    int s=src_operand(t,mode,reg,width,W4,&ok);
    if(!ok)return false;
    if(width!=4&&a!=W0){if(width==2)uxth(t,W6,a);else uxtb(t,W6,a);a=W6;}
    alu_cmp(t,a,s,width);
    return true;
}
static bool tr_addx_subx(Trans *t,bool sub){
    unsigned sz=(t->op>>6)&3u;
    if(sz!=2||(t->op&8u))return false; /* .L Dy,Dx only */
    int x=dn((t->op>>9)&7u),y=dn(t->op&7u);
    USES(F_X|F_Z);
    DEFS(F_NZVC|F_X);
    bool sticky=LIVE(F_Z)!=0;
    if(sticky)mrs_nzcv(t,W4);
    if(sub){
        subs3(t,RZR,RZR,RX);                                  /* C = no borrow-in */
        E(t,0x7A000000u|R5(y)<<16|R5(x)<<5|R5(W0));            /* sbcs w0,x,y */
        if(LIVE(F_X))cset(t,RX,C_CC);
    }else{
        E(t,0x7100001Fu|R5(RX)<<5|(1u<<10));                   /* cmp w9,#1 */
        E(t,0x3A000000u|R5(y)<<16|R5(x)<<5|R5(W0));            /* adcs w0,x,y */
        if(LIVE(F_X))cset(t,RX,C_CS);
        if(LIVE(F_C))cfinv(t);
    }
    mov(t,x,W0);
    if(sticky){
        /* Z only stays set if it was set before */
        mrs_nzcv(t,W3);
        movn(t,W5,0x4000u,1);
        orr3(t,W4,W4,W5);
        and3(t,W3,W3,W4);
        msr_nzcv(t,W3);
    }
    return true;
}
static bool tr_mul_w(Trans *t,bool is_signed){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,dst=(t->op>>9)&7u;
    if(mode==1)return false;
    DEFS(F_NZVC);
    bool ok=true;
    int s=src_operand(t,mode,reg,2,W4,&ok);
    if(!ok)return false;
    if(is_signed){sxth(t,W4,s);sxth(t,W0,dn(dst));}
    else{uxth(t,W4,s);uxth(t,W0,dn(dst));}
    mul32(t,dn(dst),W0,W4);
    flags_logic(t,dn(dst),4);
    return true;
}
static bool ea_no_side_effects(unsigned mode){return mode!=3&&mode!=4;}
static bool tr_div_w(Trans *t,bool is_signed){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,dst=(t->op>>9)&7u;
    if(mode==1||!ea_no_side_effects(mode))return false;
    USES(F_ALL);   /* can leave the block with the instruction untouched */
    DEFS(F_NZVC);
    bool ok=true;
    int s=src_operand(t,mode,reg,2,W4,&ok);
    if(!ok)return false;
    if(is_signed)sxth(t,W4,s);else uxth(t,W4,s);
    bail_if_reg(t,W4,false);        /* divide by zero raises in Musashi */
    if(is_signed)E(t,0x1AC40C00u|R5(dn(dst))<<5|R5(W0));   /* sdiv w0,dst,w4 */
    else E(t,0x1AC40800u|R5(dn(dst))<<5|R5(W0));           /* udiv w0,dst,w4 */
    /* overflow: the quotient must fit 16 bits */
    if(is_signed){sxth(t,W2,W0);eor3(t,W2,W2,W0);}
    else lsri(t,W2,W0,16);
    bail_if_reg(t,W2,true);
    E(t,0x1B008000u|R5(W4)<<16|R5(dn(dst))<<10|R5(W0)<<5|R5(W1)); /* msub w1,w0,w4,dst */
    bfi(t,W0,W1,16,16);
    mov(t,dn(dst),W0);
    if(LIVE(F_NZVC)){adds_lsl(t,RZR,RZR,W0,16);if(LIVE(F_C))cfinv(t);}
    return true;
}
static bool tr_mull(Trans *t){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u;
    if(mode==1)return false;
    unsigned ext=fetch16(t);
    unsigned dl=(ext>>12)&7u,dh=ext&7u;
    bool is_signed=(ext&0x800u)!=0,wide=(ext&0x400u)!=0;
    if(ext&0x83F8u)return false;
    if(wide&&dl==dh)return false;
    DEFS(F_NZVC);
    bool ok=true;
    int s=src_operand(t,mode,reg,4,W4,&ok);
    if(!ok)return false;
    if(is_signed)smull(t,W0,dn(dl),s);else umull(t,W0,dn(dl),s);
    if(wide){
        mov(t,dn(dl),W0);
        E(t,0xD360FC00u|R5(W0)<<5|R5(dn(dh)));          /* lsr x_h,x0,#32 */
        if(LIVE(F_NZVC)){
            /* N from bit 63, Z from all 64, V=0, C=0 */
            E(t,0xEA00001Fu|R5(W0)<<16|R5(W0)<<5);       /* tst x0,x0 */
            if(LIVE(F_C))cfinv(t);
        }
        return true;
    }
    mov(t,dn(dl),W0);
    if(LIVE(F_NZVC)){
        if(LIVE(F_V)){
            /* V when the 64-bit product does not fit 32 bits */
            if(is_signed)E(t,0xEB20C01Fu|R5(W0)<<16|R5(W0)<<5); /* cmp x0,w0,sxtw */
            else E(t,0xEB20401Fu|R5(W0)<<16|R5(W0)<<5);         /* cmp x0,w0,uxtw */
            cset(t,W6,C_NE);
        }
        adds_lsl(t,RZR,RZR,dn(dl),0);
        if(LIVE(F_V)){
            mrs_nzcv(t,W3);
            bfi64(t,W3,W6,28);
            msr_nzcv(t,W3);
        }
        if(LIVE(F_C))cfinv(t);
    }
    return true;
}
/* Bitfields on a data register. The offset rotates the field to the top of the
 * word; a width of 32 needs no case of its own, because AArch64 register shifts
 * take the count mod 32 and 0 means "whole word" in both encodings. */
static void rorv(Trans *t,int d,int n,int m){E(t,0x1AC02C00u|R5(m)<<16|R5(n)<<5|R5(d));}
static void asrv(Trans *t,int d,int n,int m){E(t,0x1AC02800u|R5(m)<<16|R5(n)<<5|R5(d));}
static void neg32(Trans *t,int d,int m){sub3(t,d,RZR,m);}
static bool tr_bitfield(Trans *t){
    unsigned kind=(t->op>>8)&7u,mode=(t->op>>3)&7u,reg=t->op&7u;
    if(mode!=0)return false;           /* memory forms stay with Musashi */
    if(kind==5)return false;           /* BFFFO */
    unsigned ext=fetch16(t);
    int dr=dn((ext>>12)&7u),d=dn(reg);
    DEFS(F_NZVC);
    /* w4 = offset mod 32, w5 = shift between a top-aligned field and bit 0 */
    if(ext&0x800u)andi31(t,W4,dn((ext>>6)&7u));else movz(t,W4,(ext>>6)&31u,0);
    if(ext&0x20u)neg32(t,W5,dn(ext&7u));
    else movz(t,W5,(32u-(ext&31u))&31u,0);
    andi31(t,W5,W5);
    neg32(t,W6,W4);
    if(kind==7){ /* BFINS Dn,Dy{o:w}: the source's low bits go into the field */
        lslv(t,W0,dr,W5);
        if(LIVE(F_NZVC)){adds3(t,RZR,RZR,W0);if(LIVE(F_C))cfinv(t);}
        movn(t,W2,0,0);
        lslv(t,W2,W2,W5);
        rorv(t,W2,W2,W4);
        rorv(t,W0,W0,W4);
        E(t,0x0A220000u|R5(d)<<5|R5(W3));  /* bic w3,d,w2 */
        orr3(t,d,W3,W0);
        return true;
    }
    rorv(t,W1,d,W6);                       /* field at the top */
    if(kind==0||kind==1||kind==2||kind==3||kind==4||kind==6){
        if(LIVE(F_NZVC)){
            lsrv(t,W2,W1,W5);
            lslv(t,W2,W2,W5);
            adds3(t,RZR,RZR,W2);
            if(LIVE(F_C))cfinv(t);
        }
    }
    switch(kind){
    case 0:return true;                    /* BFTST */
    case 1:lsrv(t,dr,W1,W5);return true;   /* BFEXTU */
    case 3:asrv(t,dr,W1,W5);return true;   /* BFEXTS */
    case 2:case 4:case 6:{                 /* BFCHG, BFCLR, BFSET */
        movn(t,W2,0,0);
        lslv(t,W2,W2,W5);
        rorv(t,W2,W2,W4);
        if(kind==2)eor3(t,d,d,W2);
        else if(kind==4)E(t,0x0A220000u|R5(d)<<5|R5(d));
        else orr3(t,d,d,W2);
        return true;
    }
    default:return false;
    }
}
static bool tr_shift_family(Trans *t){
    unsigned sz=(t->op>>6)&3u;
    if(sz==3){
        if(t->op&0x800u)return tr_bitfield(t);
        return false; /* memory shifts */
    }
    unsigned width=size_table[sz];
    if(t->op&0x20u)return tr_shift_reg(t,width);
    return tr_shift_imm(t,width);
}
static bool tr_bit(Trans *t){
    unsigned mode=(t->op>>3)&7u,reg=t->op&7u,kind=(t->op>>6)&3u;
    bool dynamic=(t->op&0x100u)!=0;
    if(mode==1)return false;
    if(kind!=0&&!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
    DEFS(F_Z);
    uint32_t imm=0;
    if(!dynamic){
        imm=fetch16(t);
        if(imm&0xFF00u)return false;
    }
    /* value in w0, address in w5 when in memory */
    if(mode==0)mov(t,W0,dn(reg));
    else{
        if(mode==7&&reg==4)return false;
        if(!ea_addr(t,mode,reg,1))return false;
        mov(t,W5,W1);
        mem_read(t,1);
    }
    /* bit index in w4 */
    if(dynamic){
        if(mode==0)andi31(t,W4,dn((t->op>>9)&7u));
        else andi7(t,W4,dn((t->op>>9)&7u));
    }else movz(t,W4,imm&(mode==0?31u:7u),0);
    lsrv(t,W6,W0,W4);
    andi1(t,W6,W6);
    if(LIVE(F_ALL&~F_Z)){
        eori1(t,W3,W6);
        mrs_nzcv(t,W2);
        bfi64(t,W2,W3,30);
        msr_nzcv(t,W2);
    }else if(LIVE(F_Z)){
        E(t,0x7200001Fu|R5(W6)<<5);  /* tst w6,#1 */
    }
    if(kind==0)return true;
    movz(t,W2,1,0);
    lslv(t,W2,W2,W4);
    if(kind==1)eor3(t,W0,W0,W2);
    else if(kind==2)E(t,0x0A220000u|R5(W0)<<5|R5(W0));  /* bic w0,w0,w2 */
    else orr3(t,W0,W0,W2);
    if(mode==0)mov(t,dn(reg),W0);
    else rmw_store(t,1);
    return true;
}
static bool tr_group0(Trans *t){
    unsigned op=t->op;
    if(op&0x100u){
        if(((op>>3)&7u)==1)return false; /* MOVEP */
        return tr_bit(t);
    }
    unsigned sz=(op>>6)&3u;
    switch((op>>9)&7u){
    case 0:if(sz==3||(op&0x3Fu)==0x3C)return false;return tr_alu_imm(t,3,size_table[sz]);
    case 1:if(sz==3||(op&0x3Fu)==0x3C)return false;return tr_alu_imm(t,2,size_table[sz]);
    case 2:if(sz==3)return false;return tr_alu_imm(t,1,size_table[sz]);
    case 3:if(sz==3)return false;return tr_alu_imm(t,0,size_table[sz]);
    case 4:return tr_bit(t);
    case 5:if(sz==3||(op&0x3Fu)==0x3C)return false;return tr_alu_imm(t,4,size_table[sz]);
    case 6:{
        if(sz==3)return false;
        unsigned mode=(op>>3)&7u,reg=op&7u,width=size_table[sz];
        if(mode==1||!(mode==0||ea_is_alterable_mem(mode,reg)||(mode==7&&reg<=3)))return false;
        uint32_t v=width==4?fetch32(t):(fetch16(t)&(width==1?0xFFu:0xFFFFu));
        imm32(t,W4,v);
        DEFS(F_NZVC);
        if(mode==0){
            int a=dn(reg);
            if(width!=4){if(width==2)uxth(t,W6,a);else uxtb(t,W6,a);a=W6;}
            alu_cmp(t,a,W4,width);
            return true;
        }
        if(!ea_addr(t,mode,reg,width))return false;
        mem_read(t,width);
        alu_cmp(t,W0,W4,width);
        return true;
    }
    default:return false;
    }
}
static bool tr_move(Trans *t,unsigned width){
    unsigned smode=(t->op>>3)&7u,sreg=t->op&7u,dmode=(t->op>>6)&7u,dreg=(t->op>>9)&7u;
    if(width==1&&smode==1)return false;
    bool ok=true;
    if(dmode==1){
        if(width==1)return false;
        int s=src_operand(t,smode,sreg,width,W4,&ok);
        if(!ok)return false;
        if(width==4)mov(t,an(dreg),s);else sxth(t,an(dreg),s);
        return true;
    }
    if(!(dmode==0||ea_is_alterable_mem(dmode,dreg)))return false;
    DEFS(F_NZVC);
    int s=src_operand(t,smode,sreg,width,W4,&ok);
    if(!ok)return false;
    if(dmode==0){
        store_dn(t,dreg,s,width);
        flags_logic(t,s,width);
        return true;
    }
    /* Keep the source safe across the destination's address arithmetic. */
    if(s!=W4){mov(t,W4,s);}
    if(!ea_addr(t,dmode,dreg,width))return false;
    mov(t,W0,W4);
    mem_write(t,width);
    flags_logic(t,W4,width);
    return true;
}
static bool tr_group4(Trans *t){
    unsigned op=t->op,mode=(op>>3)&7u,reg=op&7u;
    if(op==0x4E71)return true;                       /* NOP */
    if(op==0x4E75){                                  /* RTS */
        mov(t,W1,an(7));
        addi(t,an(7),an(7),4);
        mem_read(t,4);
        end_indirect(t);
        t->ended=true;
        return true;
    }
    if((op&0xFFC0u)==0x4E80||(op&0xFFC0u)==0x4EC0){  /* JSR, JMP */
        bool jsr=(op&0x40u)==0;
        if(mode<2||mode==3||mode==4||(mode==7&&reg==4))return false;
        uint32_t t_after;
        if(mode==7&&reg==1){
            uint32_t a=fetch32(t);
            if(a&1u)return false;
            t_after=t->ip;
            if(jsr){subi(t,an(7),an(7),4);mov(t,W1,an(7));imm32(t,W0,t_after);mem_write(t,4);}
            exit_to(t,a);
            t->ended=true;
            return true;
        }
        if(!ea_addr(t,mode,reg,4))return false;
        t_after=t->ip;
        if(jsr){
            mov(t,W5,W1);
            subi(t,an(7),an(7),4);mov(t,W1,an(7));imm32(t,W0,t_after);mem_write(t,4);
            mov(t,W0,W5);
        }else mov(t,W0,W1);
        end_indirect(t);
        t->ended=true;
        return true;
    }
    if((op&0xFFF8u)==0x49C0){                        /* EXTB.L */
        DEFS(F_NZVC);
        sxtb(t,dn(reg),dn(reg));
        flags_logic(t,dn(reg),4);
        return true;
    }
    if((op&0xF1C0u)==0x41C0){                        /* LEA */
        if(mode<2||mode==3||mode==4||(mode==7&&reg==4))return false;
        if(!ea_addr(t,mode,reg,4))return false;
        mov(t,an((op>>9)&7u),W1);
        return true;
    }
    if((op&0xFFC0u)==0x4840){                        /* PEA, SWAP */
        if(mode==0){
            DEFS(F_NZVC);
            rori(t,dn(reg),dn(reg),16);
            flags_logic(t,dn(reg),4);
            return true;
        }
        if(mode==1||mode==3||mode==4||(mode==7&&reg==4))return false;
        if(!ea_addr(t,mode,reg,4))return false;
        mov(t,W0,W1);
        subi(t,an(7),an(7),4);
        mov(t,W1,an(7));
        mem_write(t,4);
        return true;
    }
    if((op&0xFFF8u)==0x4E50){                        /* LINK.W */
        int32_t d=(int16_t)fetch16(t);
        subi(t,W5,an(7),4);
        mov(t,W1,W5);
        mov(t,W0,an(reg));
        mem_write(t,4);
        mov(t,an(reg),W5);
        add_const(t,an(7),W5,d);
        return true;
    }
    if((op&0xFFF8u)==0x4E58){                        /* UNLK */
        mov(t,W1,an(reg));
        addi(t,W5,an(reg),4);
        mem_read(t,4);
        mov(t,an(reg),W0);
        if(reg!=7)mov(t,an(7),W5);
        return true;
    }
    if((op&0xFFB8u)==0x4880&&mode==0){               /* EXT.W/.L */
        DEFS(F_NZVC);
        if(op&0x40u){sxth(t,dn(reg),dn(reg));flags_logic(t,dn(reg),4);}
        else{sxtb(t,W0,dn(reg));bfi(t,dn(reg),W0,0,16);flags_logic(t,dn(reg),2);}
        return true;
    }
    if((op&0xFB80u)==0x4880){                        /* MOVEM */
        return tr_movem(t,(op>>6)&1u,(op&0x400u)!=0);
    }
    unsigned sz=(op>>6)&3u;
    if((op&0xFF00u)==0x4A00&&sz!=3){                 /* TST */
        unsigned width=size_table[sz];
        if(mode==1&&width==1)return false;
        DEFS(F_NZVC);
        bool ok=true;
        int s=src_operand(t,mode,reg,width,W4,&ok);
        if(!ok)return false;
        flags_logic(t,s,width);
        return true;
    }
    if((op&0xFF00u)==0x4200&&sz!=3){                 /* CLR */
        unsigned width=size_table[sz];
        if(mode==1)return false;
        if(!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
        DEFS(F_NZVC);
        if(mode==0)store_dn(t,reg,RZR,width);
        else{
            if(!ea_addr(t,mode,reg,width))return false;
            mov(t,W0,RZR);
            mem_write(t,width);
        }
        if(LIVE(F_NZVC))subs3(t,RZR,RZR,RZR);
        return true;
    }
    if((op&0xFF00u)==0x4600&&sz!=3){                 /* NOT */
        unsigned width=size_table[sz];
        if(mode==1)return false;
        if(!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
        DEFS(F_NZVC);
        if(mode==0){
            mvn(t,W0,dn(reg));
            store_dn(t,reg,W0,width);
        }else{
            if(!rmw_load(t,mode,reg,width))return false;
            mvn(t,W0,W0);
            mov(t,W4,W0);
            rmw_store(t,width);
            mov(t,W0,W4);
        }
        flags_logic(t,W0,width);
        return true;
    }
    if((op&0xFF00u)==0x4400&&sz!=3){                 /* NEG */
        unsigned width=size_table[sz];
        if(mode==1)return false;
        if(!(mode==0||ea_is_alterable_mem(mode,reg)))return false;
        DEFS(F_NZVC|F_X);
        if(mode==0){
            int a=dn(reg);
            if(width!=4){if(width==2)uxth(t,W6,a);else uxtb(t,W6,a);a=W6;}
            alu_sub(t,RZR,a,width);
            store_dn(t,reg,W0,width);
        }else{
            if(!rmw_load(t,mode,reg,width))return false;
            alu_sub(t,RZR,W0,width);
            rmw_store(t,width);
        }
        return true;
    }
    if((op&0xFFC0u)==0x4C00)return tr_mull(t);
    return false;
}
static bool tr_insn(Trans *t){
    unsigned op=t->op;
    switch(op>>12){
    case 0x0:return tr_group0(t);
    case 0x1:return tr_move(t,1);
    case 0x2:return tr_move(t,4);
    case 0x3:return tr_move(t,2);
    case 0x4:return tr_group4(t);
    case 0x5:{
        unsigned sz=(op>>6)&3u;
        if(sz==3){
            if(((op>>3)&7u)==1)return tr_dbcc(t,(op>>8)&15u);
            return tr_scc(t,(op>>8)&15u);
        }
        return tr_addq_subq(t,size_table[sz]);
    }
    case 0x6:return tr_bcc(t,(op>>8)&15u);
    case 0x7:{
        if(op&0x100u)return false;
        DEFS(F_NZVC);
        unsigned d=(op>>9)&7u;
        imm32(t,dn(d),(uint32_t)(int32_t)(int8_t)op);
        flags_logic(t,dn(d),4);
        return true;
    }
    case 0x8:{
        unsigned opm=(op>>6)&7u;
        if(opm==3)return tr_div_w(t,false);
        if(opm==7)return tr_div_w(t,true);
        if((op&0x1F0u)==0x100)return false; /* SBCD */
        if(opm<3)return tr_alu_to_dn(t,3,size_table[opm]);
        if((op&0x1F0u)==0x140||(op&0x1F0u)==0x180)return false;
        return tr_alu_to_ea(t,3,size_table[opm-4u]);
    }
    case 0x9:case 0xD:{
        bool sub=(op>>12)==0x9;
        unsigned opm=(op>>6)&7u,mode=(op>>3)&7u,reg=op&7u,dst=(op>>9)&7u;
        if(opm==3||opm==7){                 /* ADDA/SUBA */
            unsigned width=opm==3?2u:4u;
            bool ok=true;
            int s=src_operand(t,mode,reg,width,W4,&ok);
            if(!ok)return false;
            if(width==2){sxth(t,W4,s);s=W4;}
            if(sub)sub3(t,an(dst),an(dst),s);else add3(t,an(dst),an(dst),s);
            return true;
        }
        if(opm>=4&&mode<2)return tr_addx_subx(t,sub);
        if(opm<3)return tr_alu_to_dn(t,sub?1u:0u,size_table[opm]);
        return tr_alu_to_ea(t,sub?1u:0u,size_table[opm-4u]);
    }
    case 0xB:{
        unsigned opm=(op>>6)&7u,mode=(op>>3)&7u,reg=op&7u,dst=(op>>9)&7u;
        if(opm==3||opm==7){                 /* CMPA */
            unsigned width=opm==3?2u:4u;
            DEFS(F_NZVC);
            bool ok=true;
            int s=src_operand(t,mode,reg,width,W4,&ok);
            if(!ok)return false;
            if(width==2){sxth(t,W4,s);s=W4;}
            alu_cmp(t,an(dst),s,4);
            return true;
        }
        if(opm<3){                          /* CMP */
            if(opm==0&&mode==1)return false;
            return tr_cmp_common(t,dn(dst),mode,reg,size_table[opm]);
        }
        unsigned width=size_table[opm-4u];
        if(mode==1){                        /* CMPM (Ay)+,(Ax)+ */
            DEFS(F_NZVC);
            if(!ea_addr(t,3,reg,width))return false;
            mem_read(t,width);
            mov(t,W4,W0);
            if(!ea_addr(t,3,dst,width))return false;
            mem_read(t,width);
            alu_cmp(t,W0,W4,width);
            return true;
        }
        return tr_alu_to_ea(t,4,width);     /* EOR */
    }
    case 0xC:{
        unsigned opm=(op>>6)&7u,mode=(op>>3)&7u,reg=op&7u,dst=(op>>9)&7u;
        if(opm==3)return tr_mul_w(t,false);
        if(opm==7)return tr_mul_w(t,true);
        if((op&0x1F8u)==0x140||(op&0x1F8u)==0x148||(op&0x1F8u)==0x188){ /* EXG */
            int a,b;
            if((op&0x1F8u)==0x140){a=dn(dst);b=dn(reg);}
            else if((op&0x1F8u)==0x148){a=an(dst);b=an(reg);}
            else{a=dn(dst);b=an(reg);}
            mov(t,W0,a);mov(t,a,b);mov(t,b,W0);
            return true;
        }
        if((op&0x1F0u)==0x100)return false; /* ABCD */
        if(opm<3)return tr_alu_to_dn(t,2,size_table[opm]);
        if(mode<2)return false;
        return tr_alu_to_ea(t,2,size_table[opm-4u]);
    }
    case 0xE:return tr_shift_family(t);
    default:return false;
    }
}

/* ---------- block construction ---------- */
static void *arena_alloc(JitCtx *j,size_t n){
    n=(n+15u)&~(size_t)15u;
    if((size_t)(j->arena_end-j->arena_next)<n)return NULL;
    void *p=j->arena_next;j->arena_next+=n;
    return p;
}
static inline void write_enable(bool on){
#if defined(__APPLE__)
    pthread_jit_write_protect_np(!on);
#else
    (void)on;
#endif
}
static inline void icache_sync(void *p,size_t n){
#if defined(__APPLE__)
    sys_icache_invalidate(p,n);
#else
    __builtin___clear_cache((char*)p,(char*)p+n);
#endif
}
static unsigned hash_pc(uint32_t pc){return (pc>>1)&(BLOCK_HASH_SIZE-1u);}
static Block *find_block(JitCtx *j,uint32_t pc){
    for(Block *b=j->hash[hash_pc(pc)];b;b=b->hash_next)if(b->pc==pc&&!b->dead)return b;
    return NULL;
}
static void protect_code(JitCtx *j,uint32_t from,uint32_t to){
    QRXFullCPU *c=j->cpu;
    uint32_t g0=from>>QRX_WPAGE_SHIFT,g1=(to-1u)>>QRX_WPAGE_SHIFT;
    /* A write straddling in from the granule below must reach the full bus too. */
    if(g0)c->wpage[g0-1u]=0;
    for(uint32_t g=g0;g<=g1;g++)c->wpage[g]=0;
}
static void register_block_pages(JitCtx *j,Block *b){
    uint32_t page=b->pc>>PAGE_SHIFT;
    CodePage *cp=&j->pages[page];
    b->page_next=cp->blocks;cp->blocks=b;
    for(uint32_t a=b->pc&~((1u<<GRANULE_SHIFT)-1u);a<b->end;a+=1u<<GRANULE_SHIFT)
        cp->granules|=UINT64_C(1)<<((a>>GRANULE_SHIFT)&63u);
    for(uint32_t a=b->pc&~7u;a<b->end;a+=8u){
        uint32_t bit=(a&0xFFFu)>>3;
        /* A block may run over into the next page: that bit belongs there. */
        if(a>>PAGE_SHIFT==page)cp->fine[bit>>6]|=UINT64_C(1)<<(bit&63u);
    }
    if((uint64_t)b->pc<j->cpu->vm.ram.size)protect_code(j,b->pc,b->end);
}
static Block *new_block(JitCtx *j,uint32_t pc){
    Block *b=arena_alloc(j,sizeof(*b));
    if(!b)return NULL;
    memset(b,0,sizeof(*b));
    b->pc=pc;b->end=pc+2u;
    unsigned h=hash_pc(pc);
    b->hash_next=j->hash[h];j->hash[h]=b;
    return b;
}
static void set_icache(JitCtx *j,uint32_t pc,const void *code){
    ICacheEntry *e=&j->icache[(pc>>1)&(ICACHE_SIZE-1u)];
    e->pc=pc;e->code=code;
}
/* Translate at most MAX_BLOCK_INSNS, stopping before a page boundary. */
static Block *translate(JitCtx *j,uint32_t pc){
    if((size_t)(j->code_end-j->code_next)<4096u*4u||
       (size_t)(j->arena_end-j->arena_next)<16384u){j->flush_pending=true;return NULL;}
    Trans *t=calloc(1,sizeof(*t));
    if(!t)return NULL;
    Block *b=new_block(j,pc);
    if(!b){free(t);j->flush_pending=true;return NULL;}
    t->j=j;t->start=pc;
    /* Dry pass: which instructions are native, and their flag use/def. */
    uint8_t uses[MAX_BLOCK_INSNS],defs[MAX_BLOCK_INSNS];
    uint32_t pcs[MAX_BLOCK_INSNS];
    uint16_t ops[MAX_BLOCK_INSNS];
    unsigned count=0,cycles=0;
    uint32_t cur=pc,end=pc;
    t->dry=true;
    uint32_t scratch_probe=0;t->p=&scratch_probe;
    bool ended=false;
    while(count<MAX_BLOCK_INSNS&&!ended){
        if((cur&1u)||!fetch_ok(t,cur,2))break;
        t->pc=cur;t->ip=cur;t->stop=false;t->uses=t->defs=0;t->ended=false;
        t->live=F_ALL;t->cold_count=0;t->exit_count=0;t->bail_count=0;t->p=&scratch_probe;
        t->op=(uint16_t)fetch16(t);
        bool ok=tr_insn(t);
        if(!ok||t->stop)break;
        unsigned cyc=m68ki_cpu.cyc_instruction[t->op];
        if(cycles+cyc>MAX_BLOCK_CYCLES)break;
        cycles+=cyc;
        pcs[count]=cur;ops[count]=t->op;uses[count]=t->uses;defs[count]=t->defs;
        count++;
        ended=t->ended;
        cur=t->ip;end=cur;
    }
    if(count==0){
        b->code=NULL;
        free(t);
        register_block_pages(j,b); /* the opcode may be rewritten too */
        return b;
    }
    /* Backward liveness; all flags are live when a block ends. */
    uint8_t live=F_ALL;
    for(int i=(int)count-1;i>=0;i--){
        t->live_after[i]=live;
        live=(uint8_t)((live&~defs[i])|uses[i]);
    }
    /* Real pass. */
    t->dry=false;t->cold_count=0;t->exit_count=0;t->bail_count=0;t->stop=false;
    t->p=j->code_next;
    write_enable(true);
    /* An expired budget exits before any guest effect. The stub sits in front
     * of the entry so the short-range TBNZ always reaches it. */
    uint32_t *budget_stub=t->p;
    imm32(t,W0,pc);
    E(t,0xD2800001u);                 /* mov x1,#0 */
    b_to(t,j->exit_stub);
    uint32_t *code=t->p;
    /* Tested before charging: a block costing more than the slice still runs. */
    {uint32_t *site=hole(t);patch_tbz(t,site,budget_stub,RCYC,63,true);}
    E(t,0xD1000108u|((cycles>4095u?4095u:cycles)<<10));   /* sub x8,x8,#cycles */
    E(t,0x910000E7u|(count<<10));                         /* add x7,x7,#count */
    for(unsigned i=0;i<count;i++){
        t->pc=pcs[i];t->ip=pcs[i];t->op=ops[i];
        t->live=t->live_after[i];t->uses=t->defs=0;t->ended=false;
        (void)fetch16(t);
        (void)tr_insn(t);
    }
    if(!t->ended)exit_to(t,cur);
    /* Direct exits */
    for(unsigned i=0;i<t->exit_count;i++){
        uint32_t *site=t->exits[i].site;
        uint32_t *stub=t->p;
        *site=enc_b(site,stub);
        imm32(t,W0,t->exits[i].target);
        /* adr x1,site */
        int64_t off=(int64_t)((uint8_t*)site-(uint8_t*)t->p);
        E(t,0x10000001u|(((uint32_t)off&3u)<<29)|(((uint32_t)(off>>2)&0x7FFFFu)<<5));
        b_to(t,j->exit_stub);
        t->exits[i].stub=stub;
    }
    /* Bail exits: state untouched, Musashi takes the instruction. */
    for(unsigned i=0;i<t->bail_count;i++){
        patch_cbz(t,t->bail[i].branch,t->p,t->bail[i].reg,t->bail[i].nonzero,false);
        imm32(t,W0,t->bail[i].pc);
        E(t,0xD2800021u);             /* mov x1,#1: LINK_BAIL */
        b_to(t,j->exit_stub);
    }
    /* Memory slow paths */
    for(unsigned i=0;i<t->cold_count;i++){
        Cold *k=&t->cold[i];
        patch_cbz(t,k->branch,t->p,W2,false,true);
        imm32(t,W3,k->pc);
        bl_to(t,k->write?j->write_stub[width_index(k->width)]:j->read_stub[width_index(k->width)]);
        b_to(t,k->resume);
    }
    size_t words=(size_t)(t->p-budget_stub);
    icache_sync(budget_stub,words*4u);
    write_enable(false);
    j->code_next=t->p;
    b->code=code;b->end=end;
    b->exit_count=(uint8_t)t->exit_count;
    if(t->exit_count){
        b->exits=arena_alloc(j,sizeof(Exit)*t->exit_count);
        if(b->exits)memcpy(b->exits,t->exits,sizeof(Exit)*t->exit_count);
    }
    register_block_pages(j,b);
    j->blocks_made++;
    free(t);
    return b;
}
static void link_exit(JitCtx *j,uint32_t *site,Block *to){
    /* Find this exit so an invalidated target can restore it. */
    Link *l=arena_alloc(j,sizeof(*l));
    if(!l)return;
    /* The stub is the branch target the site holds right now. */
    int32_t disp=(int32_t)((*site)<<6)>>6;
    l->site=site;l->stub=site+disp;l->next=to->incoming;to->incoming=l;
    write_enable(true);
    *site=enc_b(site,to->code);
    icache_sync(site,4);
    write_enable(false);
}
static void kill_block(JitCtx *j,Block *b){
    if(b->dead)return;
    b->dead=1;
    j->invalidations++;
    ICacheEntry *e=&j->icache[(b->pc>>1)&(ICACHE_SIZE-1u)];
    if(e->pc==b->pc){e->pc=UINT64_MAX;e->code=NULL;}
    if(!b->incoming)return;
    write_enable(true);
    for(Link *l=b->incoming;l;l=l->next){
        *l->site=enc_b(l->site,l->stub);
        icache_sync(l->site,4);
    }
    write_enable(false);
    b->incoming=NULL;
}
static void jit_notify(QRXFullCPU *c,uint32_t addr,uint32_t len){
    JitCtx *j=c->jit;
    if(qrx_x86code_notify)qrx_x86code_notify(c,addr,len);
    if(!j||!len)return;
    uint64_t end=(uint64_t)addr+len;
    for(uint64_t a=addr;a<end;a=(a|0xFFFu)+1u){
        uint32_t page=(uint32_t)(a>>PAGE_SHIFT);
        if(page>=PAGE_ENTRIES)break;
        CodePage *cp=&j->pages[page];
        if(!cp->granules)continue;
        uint64_t pe=((uint64_t)page+1u)<<PAGE_SHIFT;
        uint64_t e=end<pe?end:pe;
        uint64_t mask=0;
        for(uint64_t g=a>>GRANULE_SHIFT;g<=(e-1u)>>GRANULE_SHIFT;g++)mask|=UINT64_C(1)<<(g&63u);
        if(!(cp->granules&mask))continue;
        bool hit=false;
        for(uint64_t f=a>>3;f<=(e-1u)>>3&&!hit;f++){
            uint32_t bit=(uint32_t)f&511u;
            hit=(cp->fine[bit>>6]>>(bit&63u))&1u;
        }
        if(!hit)continue;
        for(Block *b=cp->blocks;b;b=b->page_next)
            if(!b->dead&&(uint64_t)b->pc<e&&a<(uint64_t)b->end)kill_block(j,b);
    }
}
static void jit_notify_fwd(QRXFullCPU *c,uint32_t addr,uint32_t len){jit_notify(c,addr,len);}
static void dma_notify(void *opaque,uint32_t addr,unsigned len){jit_notify(opaque,addr,len);}
static void flush_all(JitCtx *j){
    QRXFullCPU *c=j->cpu;
    j->flushes++;
    j->code_next=j->code_blocks;
    memset(j->hash,0,BLOCK_HASH_SIZE*sizeof(Block*));
    memset(j->icache,0xFF,ICACHE_SIZE*sizeof(ICacheEntry));
    for(uint32_t p=0;p<PAGE_ENTRIES;p++)if(j->pages[p].granules||j->pages[p].blocks){
        /* Reopen this page and the granule protected just below it. */
        uint32_t g=p<<(PAGE_SHIFT-QRX_WPAGE_SHIFT);
        if(((uint64_t)p<<PAGE_SHIFT)<c->vm.ram.size&&c->rpage[p]){
            for(uint32_t i=0;i<(1u<<(PAGE_SHIFT-QRX_WPAGE_SHIFT));i++)c->wpage[g+i]=c->rpage[p];
            if(p&&c->rpage[p-1u])c->wpage[g-1u]=c->rpage[p-1u];
        }
        j->pages[p].granules=0;j->pages[p].blocks=NULL;
        memset(j->pages[p].fine,0,sizeof j->pages[p].fine);
    }
    j->arena_next=j->arena;
    j->flush_pending=false;
    if(qrx_x86code_regate)qrx_x86code_regate(c);
}

/* ---------- execution ---------- */
/* link_site value from a bail exit: the very next instruction goes to Musashi. */
#define LINK_BAIL ((uint32_t*)(uintptr_t)1)
static inline void flags_to_host(JitCtx *j){
    uint64_t n=(FLAG_N&0x80u)?UINT64_C(1)<<31:0;
    uint64_t z=(FLAG_Z==0)?UINT64_C(1)<<30:0;
    uint64_t c=(FLAG_C&0x100u)?0:UINT64_C(1)<<29;
    uint64_t v=(FLAG_V&0x80u)?UINT64_C(1)<<28:0;
    j->nzcv=n|z|c|v;
    j->x_flag=(FLAG_X&0x100u)?1u:0u;
}
static inline void flags_from_host(const JitCtx *j){
    FLAG_N=(j->nzcv>>31&1u)?0x80u:0u;
    FLAG_Z=(j->nzcv>>30&1u)?0u:1u;
    FLAG_C=(j->nzcv>>29&1u)?0u:0x100u;
    FLAG_V=(j->nzcv>>28&1u)?0x80u:0u;
    FLAG_X=(j->x_flag&1u)?0x100u:0u;
}
static int jit_execute(QRXFullCPU *c,int slice){
    JitCtx *j=c->jit;
    if(c->instructions<j->start_instructions||c->subroutine_armed)return m68k_execute(slice);
    int64_t remaining=slice;
    while(remaining>0){
        if(c->mem_fault||c->vm.cpu.halted)break;
        if(m68ki_cpu.nmi_pending||CPU_INT_LEVEL>FLAG_INT_MASK||CPU_STOPPED||FLAG_T1||
           FLAG_T0||m68ki_cpu.pmmu_enabled||(REG_PC&1u)||RESET_CYCLES){
            /* Interrupts, STOP, trace and MMU mode all belong to Musashi.
             * While stopped it consumes the whole budget in one call. */
            m68ki_cpu.pref_addr=1;
            int used=m68k_execute((int)(CPU_STOPPED||remaining<64?remaining:64));
            remaining-=used>0?used:1;
            continue;
        }
        if(j->flush_pending)flush_all(j);
        uint32_t pc=REG_PC;
        Block *b=find_block(j,pc);
        if(!b){
            b=translate(j,pc);
            if(!b){
                if(j->flush_pending){flush_all(j);b=translate(j,pc);}
                if(!b){m68k_execute(16);remaining-=16;continue;}
            }
        }
        if(!b->code){
            if(j->opcode_misses){
                uintptr_t base=c->rpage[pc>>PAGE_SHIFT];
                if(base)j->opcode_misses[__builtin_bswap16(*(const uint16_t*)(base+pc))]++;
            }
            m68ki_cpu.pref_addr=1;
            int used=m68k_execute(1);
            j->interp_insns++;
            remaining-=used>0?used:1;
            continue;
        }
        set_icache(j,pc,b->code);
        j->cycles=remaining;
        j->insns=0;
        j->force_exit=0;
        j->link_site=NULL;
        flags_to_host(j);
        j->enter(j,b->code);
        flags_from_host(j);
        REG_PC=(uint32_t)j->pc;
        REG_PPC=REG_PC;
        c->instructions+=j->insns;
        j->native_insns+=j->insns;
        remaining=j->cycles;
        if(j->link_site==LINK_BAIL){
            m68ki_cpu.pref_addr=1;
            int used=m68k_execute(1);
            j->interp_insns++;
            remaining-=used>0?used:1;
            continue;
        }
        if(j->link_site&&j->arena_next+64u<j->arena_end){
            uint32_t next=(uint32_t)j->pc;
            Block *to=find_block(j,next);
            if(!to)to=translate(j,next);
            if(to&&to->code)link_exit(j,j->link_site,to);
        }
    }
    return (int)(slice-(remaining>0?remaining:0));
}

static void jit_request_flush(QRXFullCPU *c){
    JitCtx *j=c->jit;
    if(j)__atomic_store_n(&j->flush_pending,true,__ATOMIC_RELEASE);
}
static void *map_code(size_t n){
#if defined(__APPLE__)
    void *p=mmap(NULL,n,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);
#else
    void *p=mmap(NULL,n,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0);
#endif
    return p==MAP_FAILED?NULL:p;
}
bool qrx_jit_init(QRXFullCPU *c,uint64_t start_instructions){
    if(!c||c->jit||!c->rpage||!c->fast_bus||c->aux_opaque)return false;
    if(c->cpu_type!=68040&&c->cpu_type!=68030&&c->cpu_type!=68020)return false;
    JitCtx *j=calloc(1,sizeof(*j));
    if(!j)return false;
    j->cpu=c;
    j->code_base=map_code(CODE_BYTES);
    j->hash=calloc(BLOCK_HASH_SIZE,sizeof(Block*));
    j->icache=malloc(ICACHE_SIZE*sizeof(ICacheEntry));
    j->pages=mmap(NULL,PAGE_ENTRIES*sizeof(CodePage),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    size_t arena=64u*1024u*1024u;
    j->arena=mmap(NULL,arena,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(!j->code_base||!j->hash||!j->icache||j->pages==MAP_FAILED||j->arena==MAP_FAILED){
        if(j->code_base)munmap(j->code_base,CODE_BYTES);
        free(j->hash);free(j->icache);
        if(j->pages!=MAP_FAILED)munmap(j->pages,PAGE_ENTRIES*sizeof(CodePage));
        if(j->arena!=MAP_FAILED)munmap(j->arena,arena);
        free(j);
        return false;
    }
    j->arena_next=j->arena;j->arena_end=j->arena+arena;
    j->code_end=j->code_base+CODE_BYTES/4u;
    memset(j->icache,0xFF,ICACHE_SIZE*sizeof(ICacheEntry));
    j->dar=m68ki_cpu.dar;j->rpage=c->rpage;j->wpage=c->wpage;
    j->read_helper=(void*)read_helper;j->write_helper=(void*)write_helper;
    j->start_instructions=start_instructions;
    j->stats=getenv("QRX_JIT_STATS")!=NULL;
    if(j->stats)j->opcode_misses=calloc(65536,sizeof(uint64_t));
    write_enable(true);
    build_stubs(j);
    icache_sync(j->code_base,(size_t)((uint8_t*)j->code_next-(uint8_t*)j->code_base));
    write_enable(false);
    j->code_blocks=j->code_next;
    the_ctx=j;
    c->jit=j;
    c->jit_exec=jit_execute;
    c->jit_notify=jit_notify;
    c->jit_request_flush=jit_request_flush;
    c->vm.dma_ram_write=dma_notify;c->vm.dma_opaque=c;
    return true;
}
void qrx_jit_destroy(QRXFullCPU *c){
    if(!c||!c->jit)return;
    JitCtx *j=c->jit;
    qrx_jit_report(c);
    munmap(j->code_base,CODE_BYTES);
    munmap(j->pages,PAGE_ENTRIES*sizeof(CodePage));
    munmap(j->arena,(size_t)(j->arena_end-j->arena));
    free(j->hash);free(j->icache);free(j->opcode_misses);
    if(the_ctx==j)the_ctx=NULL;
    free(j);
    c->jit=NULL;c->jit_exec=NULL;c->jit_notify=NULL;c->jit_request_flush=NULL;
    c->vm.dma_ram_write=NULL;c->vm.dma_opaque=NULL;
}
bool qrx_jit_cpu_stopped(void){return CPU_STOPPED!=0;}
void qrx_jit_counters(const QRXFullCPU *c,uint64_t *blocks,uint64_t *native,uint64_t *interpreted){
    const JitCtx *j=c?c->jit:NULL;
    if(blocks)*blocks=j?j->blocks_made:0;
    if(native)*native=j?j->native_insns:0;
    if(interpreted)*interpreted=j?j->interp_insns:0;
}
void qrx_jit_report(const QRXFullCPU *c){
    const JitCtx *j=c?c->jit:NULL;
    if(!j||!j->stats)return;
    fprintf(stderr,"[JIT] blocks=%llu native=%llu interp=%llu flushes=%llu invalidated=%llu slow_r=%llu slow_w=%llu code=%zuKB\n",
        (unsigned long long)j->blocks_made,(unsigned long long)j->native_insns,
        (unsigned long long)j->interp_insns,(unsigned long long)j->flushes,
        (unsigned long long)j->invalidations,(unsigned long long)j->slow_reads,
        (unsigned long long)j->slow_writes,(size_t)((uint8_t*)j->code_next-(uint8_t*)j->code_base)/1024u);
    if(!j->opcode_misses)return;
    fprintf(stderr,"[JIT] interpreted opcodes by count:");
    for(int n=0;n<40;n++){
        unsigned best=0;uint64_t v=0;
        for(unsigned i=0;i<65536;i++)if(j->opcode_misses[i]>v){v=j->opcode_misses[i];best=i;}
        if(!v)break;
        fprintf(stderr," %04x:%llu",best,(unsigned long long)v);
        j->opcode_misses[best]=0;
    }
    fprintf(stderr,"\n");
}
#else /* !__aarch64__ */
bool qrx_jit_cpu_stopped(void){return CPU_STOPPED!=0;}
bool qrx_jit_init(QRXFullCPU *c,uint64_t start_instructions){(void)c;(void)start_instructions;return false;}
void qrx_jit_destroy(QRXFullCPU *c){(void)c;}
void qrx_jit_report(const QRXFullCPU *c){(void)c;}
bool qrx_jit_cpu_stopped(void){return CPU_STOPPED!=0;}
void qrx_jit_counters(const QRXFullCPU *c,uint64_t *b,uint64_t *n,uint64_t *i){
    (void)c;if(b)*b=0;if(n)*n=0;if(i)*i=0;
}
#endif
