/* Differential test for the AArch64 JIT against Musashi on AArch64 hosts.
 * Runs random instruction sequences through both from the same starting state
 * and compares registers, CCR and memory. Exit status 0 only with no mismatch
 * and a real share of native execution. */
#include "qrx68100_full.h"
#include "qrx68100_jit.h"
#include "m68k.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (1024u*1024u)
#define CODE 0x10000u
#define HANDLER 0x8000u
#define DATA 0x40000u
#define DATA_BYTES 0x8000u
#define STACK 0x70000u
static uint64_t rng=0x9E3779B97F4A7C15ull;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)(rng>>16);}
static uint32_t pick(const uint32_t *v,unsigned n){return v[rnd()%n];}

typedef struct { uint8_t bytes[4096]; unsigned n; } Prog;
static void w16(Prog *p,unsigned v){p->bytes[p->n++]=(uint8_t)(v>>8);p->bytes[p->n++]=(uint8_t)v;}
static void w32(Prog *p,uint32_t v){w16(p,v>>16);w16(p,v&0xFFFFu);}
/* An EA whose address stays inside the data window. A0-A5 point there and are
 * only ever moved by small steps in this test. */
static unsigned gen_ea(Prog *p,unsigned size,bool dest,bool allow_an,unsigned *ext,unsigned *ext_words){
    *ext_words=0;
    for(;;){
        unsigned m=rnd()%(dest?6u:9u);
        unsigned r=rnd()%6u;
        switch(m){
        case 0:return r|0u;
        case 1:if(allow_an)return 8u|r;continue;
        case 2:return 16u|r;
        case 3:return 24u|r;
        case 4:return 32u|r;
        case 5:ext[0]=(rnd()%512u)&~1u;*ext_words=1;return 40u|r;
        case 6:{ /* d8(An,Xn) with a small index register D6/D7 (kept small) */
            unsigned xn=6u+rnd()%2u,scale=rnd()%3u,word=rnd()%2u;
            ext[0]=(xn<<12)|(word<<11)|(scale<<9)|((rnd()%64u)&~1u);
            *ext_words=1;return 48u|r;
        }
        case 7:ext[0]=(DATA+(rnd()%1024u))&~1u;*ext_words=1;return 56u;     /* abs.w */
        default:
            if(size==4){ext[0]=rnd()>>16;ext[1]=rnd()&0xFFFFu;*ext_words=2;}
            else{ext[0]=size==1?(rnd()&0xFFu):(rnd()&0xFFFFu);*ext_words=1;}
            return 60u; /* immediate */
        }
    }
    (void)p;
}
static void emit_ext(Prog *p,const unsigned *ext,unsigned n){for(unsigned i=0;i<n;i++)w16(p,ext[i]);}
static void gen_one(Prog *p){
    static const unsigned szb[3]={0,1,2},szw[3]={1,2,4};
    unsigned k=rnd()%42u,ext[2],en;
    unsigned s=rnd()%3u,size=szw[s],dn=rnd()%6u;
    switch(k){
    case 0:{ /* MOVE */
        static const unsigned mv[3]={0x1000,0x3000,0x2000};
        unsigned src=gen_ea(p,size,false,size!=1,ext,&en);
        unsigned e2[2],en2;unsigned dst=gen_ea(p,size,true,false,e2,&en2);
        w16(p,mv[s]|((dst&7u)<<9)|((dst>>3)<<6)|src);
        emit_ext(p,ext,en);emit_ext(p,e2,en2);
        return;
    }
    case 1:{ /* ADD/SUB ea,Dn */
        unsigned e=gen_ea(p,size,false,size!=1,ext,&en);
        w16(p,((rnd()%2u)?0xD000u:0x9000u)|(dn<<9)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 2:{ /* ADD/SUB Dn,ea (memory) */
        unsigned e;do{e=gen_ea(p,size,true,false,ext,&en);}while((e>>3)==0);
        w16(p,((rnd()%2u)?0xD100u:0x9100u)|(dn<<9)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 3:{ /* AND/OR ea,Dn */
        unsigned e=gen_ea(p,size,false,false,ext,&en);
        w16(p,((rnd()%2u)?0xC000u:0x8000u)|(dn<<9)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 4:{ /* AND/OR Dn,ea */
        unsigned e;do{e=gen_ea(p,size,true,false,ext,&en);}while((e>>3)==0);
        w16(p,((rnd()%2u)?0xC100u:0x8100u)|(dn<<9)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 5:{ /* EOR Dn,ea */
        unsigned e=gen_ea(p,size,true,false,ext,&en);
        w16(p,0xB100u|(dn<<9)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 6:{ /* CMP ea,Dn */
        unsigned e=gen_ea(p,size,false,size!=1,ext,&en);
        w16(p,0xB000u|(dn<<9)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 7:{ /* ADDQ/SUBQ */
        unsigned e=gen_ea(p,size,true,size!=1,ext,&en);
        w16(p,0x5000u|((rnd()%8u)<<9)|((rnd()%2u)<<8)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 8:{ /* ADDI SUBI ORI ANDI EORI CMPI */
        static const uint32_t base[6]={0x0600,0x0400,0x0000,0x0200,0x0A00,0x0C00};
        unsigned e=gen_ea(p,size,true,false,ext,&en);
        w16(p,pick(base,6)|(szb[s]<<6)|e);
        if(size==4)w32(p,rnd());else w16(p,size==1?(rnd()&0xFFu):(rnd()&0xFFFFu));
        emit_ext(p,ext,en);return;
    }
    case 9:w16(p,0x7000u|(dn<<9)|(rnd()&0xFFu));return; /* MOVEQ */
    case 10:{ /* TST CLR NOT NEG */
        static const uint32_t base[4]={0x4A00,0x4200,0x4600,0x4400};
        unsigned e=gen_ea(p,size,true,false,ext,&en);
        w16(p,pick(base,4)|(szb[s]<<6)|e);emit_ext(p,ext,en);return;
    }
    case 11:{ /* shifts by immediate: AS LS RO, either direction */
        static const unsigned type[3]={0,1,3};
        w16(p,0xE000u|((rnd()%8u)<<9)|((rnd()%2u)<<8)|(szb[s]<<6)|(type[rnd()%3u]<<3)|dn);return;
    }
    case 12:{ /* shifts by register */
        unsigned t=rnd()%3u;
        w16(p,0xE020u|((rnd()%6u)<<9)|((rnd()%2u)<<8)|(szb[s]<<6)|(t<<3)|dn);return;
    }
    case 13:w16(p,0x4840u|dn);return; /* SWAP */
    case 14:w16(p,((rnd()%2u)?0x4880u:0x48C0u)|dn);return; /* EXT */
    case 15:w16(p,0x49C0u|dn);return; /* EXTB.L */
    case 16:{ /* MULU/MULS.W */
        unsigned e=gen_ea(p,2,false,false,ext,&en);
        w16(p,((rnd()%2u)?0xC0C0u:0xC1C0u)|(dn<<9)|e);emit_ext(p,ext,en);return;
    }
    case 17:{ /* DIVU/DIVS.W with imm or register */
        unsigned e=gen_ea(p,2,false,false,ext,&en);
        if((e>>3)==3||(e>>3)==4)e=dn;
        if(e==dn){en=0;}
        w16(p,((rnd()%2u)?0x80C0u:0x81C0u)|((rnd()%6u)<<9)|e);emit_ext(p,ext,en);return;
    }
    case 18:{ /* MULx.L, 32 and 64 bit */
        unsigned e=gen_ea(p,4,false,false,ext,&en);
        unsigned dl=rnd()%6u,dh=(dl+1u+rnd()%4u)%6u;
        w16(p,0x4C00u|e);
        w16(p,(dl<<12)|((rnd()%2u)<<11)|((rnd()%2u)<<10)|dh);
        emit_ext(p,ext,en);return;
    }
    case 19:{ /* BTST BCHG BCLR BSET: dynamic */
        unsigned e=gen_ea(p,1,true,false,ext,&en);
        w16(p,0x0100u|((rnd()%6u)<<9)|((rnd()%4u)<<6)|e);emit_ext(p,ext,en);return;
    }
    case 20:{ /* same, static */
        unsigned e=gen_ea(p,1,true,false,ext,&en);
        w16(p,0x0800u|((rnd()%4u)<<6)|e);w16(p,rnd()&31u);emit_ext(p,ext,en);return;
    }
    case 21:{ /* Bcc over one short instruction */
        unsigned cc=2u+rnd()%14u;
        w16(p,0x6000u|(cc<<8)|2u);
        w16(p,0x7000u|(dn<<9)|(rnd()&0xFFu));return;
    }
    case 22:{ /* Scc */
        unsigned e=gen_ea(p,1,true,false,ext,&en);
        w16(p,0x50C0u|((rnd()%16u)<<8)|e);emit_ext(p,ext,en);return;
    }
    case 23:{ /* DBcc skipping back never: forward-only displacement */
        unsigned cc=rnd()%16u;
        w16(p,0x50C8u|(cc<<8)|dn);w16(p,4u);
        w16(p,0x4E71u);return;
    }
    case 24:{ /* MOVEM to and from memory */
        unsigned mask=(rnd()&rnd()&0x3F3Fu)|1u;
        if(rnd()%2u){ /* regs -> -(An) */
            unsigned r=rnd()%6u;w16(p,((rnd()%2u)?0x48A0u:0x48E0u)|r);
            unsigned m=0;for(unsigned i=0;i<16;i++)if(mask&(1u<<i))m|=1u<<(15u-i);
            w16(p,m);
        }else{ /* (An)+ -> regs, Dn only so pointers survive */
            unsigned r=rnd()%6u;w16(p,((rnd()%2u)?0x4C98u:0x4CD8u)|r);
            w16(p,mask&0x3Fu);
        }
        return;
    }
    case 25:{ /* LEA d16(An) staying in the data window */
        unsigned r=rnd()%6u;
        w16(p,0x41E8u|(r<<9)|r);w16(p,((rnd()%32u)&~1u));return;
    }
    case 26:w16(p,0xC140u|((rnd()%6u)<<9)|(rnd()%6u));return; /* EXG Dx,Dy */
    case 27:{ /* ADDX/SUBX.L Dy,Dx */
        w16(p,((rnd()%2u)?0xD180u:0x9180u)|(dn<<9)|(rnd()%6u));return;
    }
    case 28:{ /* CMPM */
        w16(p,0xB108u|((rnd()%6u)<<9)|(szb[s]<<6)|(rnd()%6u));return;
    }
    case 29:{ /* ADDA/SUBA/CMPA with a small value */
        unsigned r=rnd()%6u;
        w16(p,0x7000u|(6u<<9)|((rnd()%8u)&0x7Fu));
        w16(p,((rnd()%2u)?0xD0C6u:0xB0C6u)|(r<<9));
        w16(p,0xD1C6u|(r<<9));w16(p,0x91C6u|(r<<9));
        return;
    }
    case 30:{ /* LINK/UNLK pair on A5 */
        w16(p,0x4E55u);w16(p,((rnd()%64u)&~1u));
        w16(p,0x4E5Du);return;
    }
    case 31:{ /* PEA d16(An) then pop */
        unsigned r=rnd()%6u;
        w16(p,0x4868u|r);w16(p,((rnd()%32u)&~1u));
        w16(p,0x201Fu|(dn<<9));return;
    }
    case 32:{ /* JSR to next instruction via BSR.S, then RTS style pop */
        w16(p,0x6100u);w16(p,2u);   /* BSR.W +2 */
        w16(p,0x201Fu|(dn<<9));     /* pop return address */
        return;
    }
    case 33:{ /* MOVE.L An,Dn with pointers masked back into the window */
        w16(p,0x2008u|(dn<<9)|(rnd()%6u));return;
    }
    case 35:{ /* bitfields on a data register, static or dynamic offset and width */
        static const unsigned kinds[7]={0xE8C0,0xE9C0,0xEAC0,0xEBC0,0xECC0,0xEEC0,0xEFC0};
        unsigned ext=((rnd()%6u)<<12)|((rnd()%2u)<<11)|((rnd()%2u)<<5);
        if(ext&0x800u)ext|=(rnd()%6u)<<6;else ext|=(rnd()%32u)<<6;
        if(ext&0x20u)ext|=rnd()%6u;else ext|=rnd()%32u;
        w16(p,kinds[rnd()%7u]|dn);w16(p,ext);
        return;
    }
    case 34:{ /* ADDX on memory and BCD: not native, exercises fallback */
        w16(p,0xC100u|(dn<<9)|(rnd()%6u));return;
    }
    default:{ /* plain register ALU, the common case */
        static const uint32_t base[4]={0xD000,0x9000,0xC000,0x8000};
        w16(p,pick(base,4)|(dn<<9)|(szb[s]<<6)|(rnd()%6u));return;
    }
    }
}
typedef struct { uint32_t da[16],sr; } Regs;
static void load_program(QRXFullCPU *c,const Prog *prog,const uint32_t *d,uint32_t ccr){
    memset(c->ram_owned,0,RAM_SIZE);
    /* every vector lands on a BRA-to-self; SP and PC set by the caller */
    for(uint32_t v=8;v<1024;v+=4)qrx_full_bus_write(c,v,4,HANDLER);
    qrx_full_bus_write(c,HANDLER,2,0x60FE);
    memcpy(c->ram_owned+CODE,prog->bytes,prog->n);
    /* sentinel loop after the program */
    c->ram_owned[CODE+prog->n]=0x60;c->ram_owned[CODE+prog->n+1]=0xFE;
    /* The direct memcpy above bypasses the bus, so tell the JIT by hand. */
    if(c->jit_notify)c->jit_notify(c,CODE,4096u+8u);
    uint64_t seed=d[0]^((uint64_t)d[1]<<32);
    for(uint32_t i=0;i<DATA_BYTES;i++){
        seed=seed*6364136223846793005ull+1442695040888963407ull;
        c->ram_owned[DATA+i]=(uint8_t)(seed>>56);
    }
    for(unsigned i=0;i<8;i++)m68k_set_reg((m68k_register_t)(M68K_REG_D0+i),d[i]);
    for(unsigned i=0;i<6;i++)m68k_set_reg((m68k_register_t)(M68K_REG_A0+i),DATA+0x1000u+i*0x800u);
    m68k_set_reg(M68K_REG_A6,DATA+0x7000u);
    m68k_set_reg(M68K_REG_A7,STACK);
    m68k_set_reg(M68K_REG_SR,0x2700u|ccr);
    m68k_set_reg(M68K_REG_PC,CODE);
}
static void snapshot(Regs *r){
    for(unsigned i=0;i<16;i++)r->da[i]=m68k_get_reg(NULL,(m68k_register_t)(M68K_REG_D0+i));
    r->sr=m68k_get_reg(NULL,M68K_REG_SR);
}
static bool run_until_loop(QRXFullCPU *c,uint32_t end,int (*exec)(QRXFullCPU*,int),unsigned max){
    for(unsigned i=0;i<max;i++){
        (void)exec(c,64);
        uint32_t pc=m68k_get_reg(NULL,M68K_REG_PC);
        if(pc==end||pc==HANDLER)return true;
    }
    return false;
}
static int interp_exec(QRXFullCPU *c,int n){(void)c;return m68k_execute(n);}
int main(int argc,char **argv){
    unsigned programs=argc>1?(unsigned)strtoul(argv[1],NULL,10):4000u;
    if(argc>2)rng^=strtoull(argv[2],NULL,10)*0x100000001B3ull;
    QRXFullCPU c;
    if(!qrx_full_init(&c,RAM_SIZE,68040)){fprintf(stderr,"init failed\n");return 2;}
    /* A reset needs vectors; RAM boot with an even PC and SP is enough. */
    qrx_full_bus_write(&c,0,4,STACK);qrx_full_bus_write(&c,4,4,CODE);
    if(!qrx_full_reset(&c)){fprintf(stderr,"reset failed\n");return 2;}
    if(!qrx_jit_init(&c,0)){
#if defined(__aarch64__)
        fprintf(stderr,"JIT unavailable on AArch64\n");return 2;
#else
        puts("SKIP: not an AArch64 host");return 0;
#endif
    }
    int (*jit_exec)(QRXFullCPU*,int)=c.jit_exec;
    uint32_t *ref_mem=malloc(DATA_BYTES+0x10000u);
    unsigned failures=0,handler_runs=0,compared=0,hung=0;
    for(unsigned n=0;n<programs&&failures<8;n++){
        Prog prog;prog.n=0;
        unsigned count=1u+rnd()%6u;
        for(unsigned i=0;i<count;i++)gen_one(&prog);
        uint32_t d[8];
        static const uint32_t edge[8]={0,1,0x7F,0x80,0x7FFF,0x8000,0x7FFFFFFF,0x80000000u};
        for(unsigned i=0;i<6;i++)d[i]=(rnd()%3u)?rnd():edge[rnd()%8u];
        d[6]=rnd()%32u;d[7]=rnd()%32u; /* index registers stay small */
        uint32_t ccr=rnd()&0x1Fu;
        uint32_t end=CODE+prog.n;

        c.jit_exec=NULL;
        load_program(&c,&prog,d,ccr);
        bool ok1=run_until_loop(&c,end,interp_exec,400);
        Regs ref;snapshot(&ref);
        uint32_t ref_pc=m68k_get_reg(NULL,M68K_REG_PC);
        memcpy(ref_mem,c.ram_owned+DATA,DATA_BYTES);
        memcpy((uint8_t*)ref_mem+DATA_BYTES,c.ram_owned+STACK-0x10000u,0x10000u);

        c.jit_exec=jit_exec;
        load_program(&c,&prog,d,ccr);
        bool ok2=run_until_loop(&c,end,jit_exec,400);
        Regs got;snapshot(&got);
        uint32_t got_pc=m68k_get_reg(NULL,M68K_REG_PC);
        if(ok1!=ok2){
            failures++;
            fprintf(stderr,"ONE SIDE HUNG program %u: musashi=%d pc=%08x jit=%d pc=%08x\n",n,ok1,ref_pc,ok2,got_pc);
            fprintf(stderr,"  code:");
            for(unsigned i=0;i<prog.n;i+=2)fprintf(stderr," %02x%02x",prog.bytes[i],prog.bytes[i+1]);
            fprintf(stderr,"\n");
            continue;
        }
        if(!ok1){
            hung++;
            if(getenv("QRX_JIT_DIFF_VERBOSE")){
                fprintf(stderr,"both hung pc=%08x code:",ref_pc);
                for(unsigned i=0;i<prog.n;i+=2)fprintf(stderr," %02x%02x",prog.bytes[i],prog.bytes[i+1]);
                fprintf(stderr,"\n");
            }
            continue;
        }
        if(ref_pc==HANDLER)handler_runs++;
        compared++;
        bool same=ref_pc==got_pc&&ref.sr==got.sr&&!memcmp(ref.da,got.da,sizeof(ref.da));
        bool same_mem=!memcmp(ref_mem,c.ram_owned+DATA,DATA_BYTES)&&
            !memcmp((uint8_t*)ref_mem+DATA_BYTES,c.ram_owned+STACK-0x10000u,0x10000u);
        if(!same||!same_mem){
            failures++;
            fprintf(stderr,"MISMATCH program %u: pc %08x/%08x sr %04x/%04x mem=%s ccr_in=%02x\n  code:",n,ref_pc,got_pc,ref.sr,got.sr,same_mem?"same":"DIFF",ccr);
            for(unsigned i=0;i<prog.n;i+=2)fprintf(stderr," %02x%02x",prog.bytes[i],prog.bytes[i+1]);
            fprintf(stderr,"\n");
            if(!same_mem){
                const uint8_t *rm=(const uint8_t*)ref_mem,*gm=c.ram_owned+DATA;
                for(uint32_t i=0;i<DATA_BYTES;i++)if(rm[i]!=gm[i]){fprintf(stderr,"  data %05x ref=%02x jit=%02x\n",DATA+i,rm[i],gm[i]);break;}
                gm=c.ram_owned+STACK-0x10000u;rm+=DATA_BYTES;
                for(uint32_t i=0;i<0x10000u;i++)if(rm[i]!=gm[i]){fprintf(stderr,"  stack %05x ref=%02x jit=%02x\n",STACK-0x10000u+i,rm[i],gm[i]);break;}
            }
            for(unsigned i=0;i<16;i++)if(ref.da[i]!=got.da[i])
                fprintf(stderr,"  %c%u ref=%08x jit=%08x\n",i<8?'D':'A',i%8,ref.da[i],got.da[i]);
        }
    }
    uint64_t blocks,native,interp;
    qrx_jit_counters(&c,&blocks,&native,&interp);
    printf("compared=%u hung=%u exceptions=%u failures=%u blocks=%" PRIu64 " native=%" PRIu64 " interpreted=%" PRIu64 "\n",
        compared,hung,handler_runs,failures,blocks,native,interp);
    qrx_jit_destroy(&c);
    qrx_full_destroy(&c);
    free(ref_mem);
    if(failures)return 1;
    if(compared<programs/4u||native<interp){fprintf(stderr,"coverage too low\n");return 1;}
    puts("PASS");
    return 0;
}
