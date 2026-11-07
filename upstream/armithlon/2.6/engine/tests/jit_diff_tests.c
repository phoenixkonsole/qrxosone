/* Differential test for the AArch64 JIT against Musashi on AArch64 hosts.
 * Runs random instruction sequences through both from the same starting state
 * and compares registers, CCR, FPU state and memory. Exit status 0 only with no
 * mismatch and a real share of native execution. */
#include "qrx68100_full.h"
#include "qrx68100_jit.h"
#include "m68k.h"
#include "m68kcpu.h"
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
/* ---------- FPU programs ---------- */
static uint32_t rnd32(void){return rnd()^(rnd()<<16);}
static uint64_t rnd64(void){return (uint64_t)rnd32()<<32|rnd32();}
static uint32_t pick_single(void){
    static const uint32_t special[]={0,0x80000000u,0x3F800000u,0xBF800000u,0x3F000000u,0x40000000u,
        0x7F7FFFFFu,0xFF7FFFFFu,0x00800000u,0x80800000u,0x00800001u,0x00000001u,0x007FFFFFu,
        0x7F800000u,0xFF800000u,0x7FC00000u,0x7F800001u,0x5F000000u,0x4F000000u,0x4EFFFFFFu};
    switch(rnd()%8u){
    case 0:return special[rnd()%(sizeof(special)/sizeof(special[0]))];
    case 1:return rnd32();                                              /* anything at all */
    case 2:return (rnd()%2u)<<31|(1u+rnd()%6u)<<23|(rnd32()&0x7FFFFFu); /* products underflow */
    case 3:return (rnd()%2u)<<31|(248u+rnd()%6u)<<23|(rnd32()&0x7FFFFFu); /* products overflow */
    case 4:return (rnd()%2u)<<31|(127u+rnd()%8u)<<23|(rnd32()&0x700000u); /* few mantissa bits */
    default:return (rnd()%2u)<<31|(107u+rnd()%40u)<<23|(rnd32()&0x7FFFFFu);
    }
}
static uint64_t pick_double(void){
    static const uint64_t special[]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3FF0000000000000),
        UINT64_C(0x7FEFFFFFFFFFFFFF),UINT64_C(0x0010000000000000),UINT64_C(0x8010000000000001),
        UINT64_C(0x0000000000000001),UINT64_C(0x000FFFFFFFFFFFFF),UINT64_C(0x7FF0000000000000),
        UINT64_C(0xFFF0000000000000),UINT64_C(0x7FF8000000000000),UINT64_C(0x7FF0000000000001),
        /* just outside the single range, and around its denormals */
        UINT64_C(0x47F0000000000000),UINT64_C(0x47EFFFFFF0000000),UINT64_C(0x3810000000000000),
        UINT64_C(0x36A0000000000000),UINT64_C(0x3690000000000000),UINT64_C(0x41E0000000000000)};
    switch(rnd()%8u){
    case 0:return special[rnd()%(sizeof(special)/sizeof(special[0]))];
    case 1:return rnd64();
    case 2:return (uint64_t)(rnd()%2u)<<63|(uint64_t)(1u+rnd()%40u)<<52|(rnd64()&UINT64_C(0xFFFFFFFFFFFFF));
    case 3:return (uint64_t)(rnd()%2u)<<63|(uint64_t)(2000u+rnd()%46u)<<52|(rnd64()&UINT64_C(0xFFFFFFFFFFFFF));
    case 4:{ /* a single, so FSxxx can run on it */
        uint32_t v=pick_single();
        return float32_to_float64(v);
    }
    /* Mid-range doubles: most of the 53 bits in use, which is where double rounding would show. */
    default:return (uint64_t)(rnd()%2u)<<63|(uint64_t)(1023u-40u+rnd()%80u)<<52|(rnd64()&UINT64_C(0xFFFFFFFFFFFFF));
    }
}
static floatx80 pick_extended(void){
    floatx80 v;
    switch(rnd()%10u){
    case 0:case 1:case 2:case 3:return float32_to_floatx80(pick_single());
    case 4:case 5:case 6:return float64_to_floatx80(pick_double());
    case 7:v.high=(uint16_t)((rnd()%2u)<<15|(0x3FFFu-32u+rnd()%64u));v.low=rnd64()|UINT64_C(0x8000000000000000);return v; /* 64 bits */
    case 8:v.high=(uint16_t)rnd();v.low=rnd64()|UINT64_C(0x8000000000000000);return v;     /* outside double range */
    /* Denormal. No unnormals, here or read from raw memory: softfloat's square root never returns on them. */
    default:v.high=(uint16_t)((rnd()%2u)<<15);v.low=rnd64()&(UINT64_C(0x7FFFFFFFFFFFFFFF)>>(rnd()%64u));return v;
    }
}
/* Effective address for an FPU source or destination, valid for that format in m68kfpu.c.
 * fmt: 0 L 1 S 2 X 4 W 5 D 6 B. The immediate goes to ext[]. */
static unsigned gen_fp_ea(unsigned fmt,bool dest,unsigned *ext,unsigned *en){
    *en=0;
    for(;;){
        unsigned m=rnd()%(dest?6u:8u),r=rnd()%6u;
        switch(m){
        case 0:if(fmt==5u||fmt==2u)continue;if(dest&&(fmt==4u||fmt==6u))continue;return r;
        case 1:return 16u|r;
        case 2:return 24u|r;
        case 3:return 32u|r;
        case 4:ext[0]=(rnd()%512u)&~1u;*en=1;return 40u|r;
        case 5:{
            if(fmt==2u)continue;
            unsigned xn=6u+rnd()%2u,scale=rnd()%3u,word=rnd()%2u;
            ext[0]=(xn<<12)|(word<<11)|(scale<<9)|((rnd()%64u)&~1u);
            *en=1;return 48u|r;
        }
        case 6:
            if(fmt==5u||fmt==2u)continue;
            ext[0]=(DATA+(rnd()%1024u))&~1u;*en=1;return 56u;
        default:
            if(fmt==2u)continue;
            if(fmt==1u){uint32_t v=pick_single();ext[0]=v>>16;ext[1]=v&0xFFFFu;*en=2;}
            else if(fmt==0u){uint32_t v=rnd32();ext[0]=v>>16;ext[1]=v&0xFFFFu;*en=2;}
            else if(fmt==5u){uint64_t v=pick_double();for(unsigned i=0;i<4;i++)ext[i]=(unsigned)(v>>(48u-16u*i))&0xFFFFu;*en=4;}
            else{ext[0]=fmt==4u?(rnd()&0xFFFFu):(rnd()&0xFFu);*en=1;}
            return 60u;
        }
    }
}
static void gen_fp(Prog *p){
    static const uint8_t native[]={0x00,0x40,0x44,0x18,0x58,0x5C,0x1A,0x5A,0x5E,0x41,0x45,
        0x60,0x64,0x62,0x66,0x63,0x67,0x68,0x6C,0x38,0x3A};
    /* Musashi only, so translated blocks have to hand over and take back exact state. */
    static const uint8_t other[]={0x20,0x22,0x23,0x28,0x04,0x01,0x03,0x24,0x27};
    static const uint8_t fmts[]={1,1,1,5,5,0,4,6};
    unsigned ext[4],en=0,dst=rnd()%8u,src=rnd()%8u,k=rnd()%32u;
    /* Extended precision comes from FMOVE.X of a register into memory and FMOVEM, not raw bytes. */
    if(k<18u){ /* general */
        unsigned opmode=(rnd()%6u)?native[rnd()%sizeof(native)]:other[rnd()%sizeof(other)];
        if(rnd()%3u){ /* from an effective address */
            unsigned fmt=fmts[rnd()%sizeof(fmts)];
            unsigned ea=gen_fp_ea(fmt,false,ext,&en);
            w16(p,0xF200u|ea);
            w16(p,0x4000u|fmt<<10|dst<<7|opmode);
            emit_ext(p,ext,en);
        }else{
            w16(p,0xF200u);
            w16(p,src<<10|dst<<7|opmode);
        }
        return;
    }
    if(k<23u){ /* FMOVE FPn,<ea> */
        static const uint8_t out[]={1,1,1,5,5,0,0,4,6,2};
        unsigned fmt=out[rnd()%sizeof(out)];
        unsigned ea=gen_fp_ea(fmt,true,ext,&en);
        w16(p,0xF200u|ea);
        w16(p,0x6000u|fmt<<10|src<<7);
        emit_ext(p,ext,en);
        return;
    }
    if(k<25u){ /* FBcc over one short instruction */
        unsigned cond=rnd()%32u,reg=rnd()%6u;
        if(rnd()%2u){w16(p,0xF280u|cond);w16(p,4u);}
        else{w16(p,0xF2C0u|cond);w32(p,6u);}
        w16(p,0x7000u|(reg<<9)|(rnd()&0xFFu));
        return;
    }
    if(k==27u){ /* new rounding mode through a register, mostly back to nearest */
        static const uint8_t modes[]={0,0,0,0x10,0x20,0x30,0x40};
        unsigned reg=rnd()%6u;
        w16(p,0x7000u|(reg<<9)|modes[rnd()%sizeof(modes)]);
        w16(p,0xF200u|reg);w16(p,0x9000u);
        return;
    }
    if(k==28u){ /* read the control registers, write FPSR */
        static const unsigned forms[]={0xB000,0xA800,0xA400,0x8800};
        w16(p,0xF200u|(rnd()%6u));w16(p,forms[rnd()%4u]);
        return;
    }
    if(k==29u){ /* several control registers in memory, the form AROS switches tasks with */
        unsigned reg=rnd()%6u;
        w16(p,0xF210u|reg);w16(p,0xBC00u);
        w16(p,0xF210u|reg);w16(p,0x9C00u);
        return;
    }
    if(k==26u){ /* FScc, through registers and memory */
        unsigned reg=rnd()%6u;
        if(rnd()%2u){w16(p,0xF240u|reg);w16(p,rnd()%32u);}
        else{w16(p,0xF268u|reg);w16(p,rnd()%32u);w16(p,(rnd()%512u)&~1u);}
        return;
    }
    if(k==25u){ /* extended through memory: every register value, and back to another register */
        unsigned reg=rnd()%6u;
        w16(p,0xF220u|reg);w16(p,0x6800u|src<<7);
        w16(p,0xF218u|reg);w16(p,0x4800u|dst<<7);
        return;
    }
    if(k==30u){ /* FMOVEM data registers */
        unsigned reg=rnd()%6u,mask=rnd()&0xFFu,back=0;
        for(unsigned i=0;i<8;i++)if(mask>>i&1u)back|=1u<<(7u-i);
        w16(p,0xF220u|reg);w16(p,0xE000u|mask);
        if(rnd()%2u){w16(p,0xF200u);w16(p,src<<10|dst<<7|0x62u);}
        if(rnd()%2u){w16(p,0xF218u|reg);w16(p,0xD000u|back);return;}
        /* The same through a displacement, which leaves An where it is. */
        w16(p,0xF228u|reg);w16(p,0xD000u|back);w16(p,0u);
        return;
    }
    /* FRESTORE of a null frame: FSAVE keeps writing null frames until the next FPU instruction. */
    {
        unsigned reg=rnd()%6u,reg2=rnd()%6u;
        w16(p,0x4290u|reg);
        w16(p,0xF350u|reg);
        if(rnd()%2u){w16(p,0xF23Cu);w16(p,0x4000u|1u<<10|0x4u<<7);w32(p,0x3F800000u);}
        w16(p,0xF320u|reg2);
        return;
    }
}
typedef struct { floatx80 fp[8]; uint32_t fpsr,fpcr; int just_reset; } FpState;
static void fp_state_put(const FpState *f){
    for(unsigned i=0;i<8;i++)m68ki_cpu.fpr[i]=f->fp[i];
    m68ki_cpu.fpsr=f->fpsr;
    m68ki_cpu.fpcr=f->fpcr;
    m68ki_cpu.fpu_just_reset=f->just_reset;
    float_rounding_mode=(int8)((f->fpcr>>4)&3u);
    floatx80_rounding_precision=80;
}
static void fp_state_get(FpState *f){
    memset(f,0,sizeof *f);
    for(unsigned i=0;i<8;i++){f->fp[i].high=m68ki_cpu.fpr[i].high;f->fp[i].low=m68ki_cpu.fpr[i].low;}
    f->fpsr=m68ki_cpu.fpsr;
    f->fpcr=m68ki_cpu.fpcr;
    f->just_reset=m68ki_cpu.fpu_just_reset;
}
static void fp_state_pick(FpState *f){
    memset(f,0,sizeof *f);
    /* Single-only register files reach the fastest paths. */
    bool singles=rnd()%3u==0;
    for(unsigned i=0;i<8;i++){
        f->fp[i]=singles?float32_to_floatx80(pick_single()):pick_extended();
    }
    f->fpsr=(rnd()%4u)?0:(rnd32()&0xFFFFFFF8u);
    static const uint8_t modes[]={0,0,0,0,0,0,0x10,0x20,0x30,0x40,0x80};
    f->fpcr=modes[rnd()%sizeof(modes)];
}
/* Data window: raw bytes, or floats or doubles where aligned accesses find them. */
static uint8_t fp_data[DATA_BYTES];
static void data_pick(void){
    unsigned kind=rnd()%3u;
    for(uint32_t i=0;i<DATA_BYTES;i+=8){
        uint64_t v=kind==0?rnd64():kind==1?(uint64_t)pick_single()<<32|pick_single():pick_double();
        for(unsigned b=0;b<8;b++)fp_data[i+b]=(uint8_t)(v>>(56u-8u*b));
    }
}
static const FpState *current_fp;
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
    memcpy(c->ram_owned+DATA,fp_data,DATA_BYTES);
    fp_state_put(current_fp);
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
static bool fp_same(const FpState *a,const FpState *b){
    for(unsigned i=0;i<8;i++)if(a->fp[i].high!=b->fp[i].high||a->fp[i].low!=b->fp[i].low)return false;
    return a->fpsr==b->fpsr&&a->fpcr==b->fpcr&&a->just_reset==b->just_reset;
}
static void dump_program(const Prog *prog){
    fprintf(stderr,"  code:");
    for(unsigned i=0;i<prog->n;i+=2)fprintf(stderr," %02x%02x",prog->bytes[i],prog->bytes[i+1]);
    fprintf(stderr,"\n");
}
/* The forms that dominate floating point work have to run natively on ordinary values: a fallback
 * would pass the comparison and lose all of the speed. */
static unsigned check_hot_forms_native(QRXFullCPU *c,int (*jit_exec)(QRXFullCPU*,int)){
    /* FMOVEM, FMOVE.X, control registers and FScc in the same block. */
    static const uint16_t rest[][3]={
        {0xF227u,0xE0FCu,0},{0xF21Fu,0xD03Fu,0},{0xF227u,0x6800u|2u<<7,0},{0xF21Fu,0x4800u|5u<<7,0},
        {0xF200u|5u,0xB000u,0},{0xF200u|5u,0x9000u,0},{0xF200u|5u,0xA800u,0},{0xF200u|5u,0x8800u,0},
        {0xF240u|5u,0x0002u,0},{0xF268u,0x000Eu,0x0020u},{0xF228u,0xE0FCu,0x0200u},{0xF228u,0xD03Fu,0x0200u},
    };
    static const uint8_t ops[]={0x40,0x44,0x00,0x58,0x5C,0x5A,0x5E,0x18,0x1A,0x41,0x45,0x60,0x64,0x62,0x66,
        0x63,0x67,0x68,0x6C,0x38,0x3A};
    static const uint8_t fmts[]={1,5,0,4,6};
    Prog prog;prog.n=0;
    FpState f;memset(&f,0,sizeof f);
    for(unsigned i=0;i<8;i++)f.fp[i]=float32_to_floatx80(0x3F800000u+(i+1u)*0x00100000u);
    f.fp[7]=float32_to_floatx80(0);
    for(uint32_t i=0;i<DATA_BYTES;i+=4){
        uint32_t v=0x40000000u+(i&0xFFFFu)*0x100u;
        for(unsigned b=0;b<4;b++)fp_data[i+b]=(uint8_t)(v>>(24u-8u*b));
    }
    /* FDxxx leaves doubles behind that FSxxx would rightly refuse, so every use starts from a single. */
#define LOAD_SINGLE(n,bits) (w16(&prog,0xF23Cu),w16(&prog,0x4000u|1u<<10|(unsigned)(n)<<7),w32(&prog,(bits)))
    for(unsigned o=0;o<sizeof(ops);o++){
        LOAD_SINGLE(1,0x3FC00000u);LOAD_SINGLE(2,0x40200000u);
        w16(&prog,0xF200u);w16(&prog,1u<<10|2u<<7|ops[o]);                 /* FPm,FPn */
        for(unsigned s=0;s<sizeof(fmts);s++){
            /* A double with all its bits is no operand for FSxxx: that one is meant to fall back. */
            if(fmts[s]==5u&&(ops[o]&0x44u)==0x40u)continue;
            LOAD_SINGLE(3,0x40600000u);
            if(fmts[s]==5u){w16(&prog,0xF200u|16u);w16(&prog,0x4000u|5u<<10|3u<<7|ops[o]);} /* (A0) */
            else{w16(&prog,0xF200u|16u|1u);w16(&prog,0x4000u|(unsigned)fmts[s]<<10|3u<<7|ops[o]);} /* (A1) */
        }
        w16(&prog,0xF200u|16u|2u);w16(&prog,0x6000u|1u<<10|3u<<7);          /* FMOVE.S FP3,(A2) */
        w16(&prog,0xF200u|16u|3u);w16(&prog,0x6000u|5u<<10|2u<<7);          /* FMOVE.D FP2,(A3) */
        w16(&prog,0xF280u|1u);w16(&prog,4u);w16(&prog,0x7000u);             /* FBEQ */
        w16(&prog,0xF200u|4u);w16(&prog,0x6000u|0u<<10|4u<<7);              /* FMOVE.L FP4,D4 */
    }
#undef LOAD_SINGLE
    for(unsigned i=0;i<sizeof(rest)/sizeof(rest[0]);i++){
        for(unsigned w=0;w<3;w++)if(w<2||rest[i][w])w16(&prog,rest[i][w]);
    }
    uint32_t d[8]={0};
    current_fp=&f;
    c->jit_exec=jit_exec;
    load_program(c,&prog,d,0);
    uint64_t native0,interp0,native1,interp1;
    qrx_jit_counters(c,NULL,&native0,&interp0);
    bool ok=run_until_loop(c,CODE+prog.n,jit_exec,4000);
    qrx_jit_counters(c,NULL,&native1,&interp1);
    if(!ok||interp1!=interp0||native1==native0){
        fprintf(stderr,"HOT FORMS NOT NATIVE: finished=%d native=%" PRIu64 " interpreted=%" PRIu64 "\n",ok,native1-native0,interp1-interp0);
        return 1;
    }
    return 0;
}
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
    unsigned failures=check_hot_forms_native(&c,jit_exec),handler_runs=0,compared=0,hung=0;
    for(unsigned n=0;n<programs&&failures<8;n++){
        Prog prog;prog.n=0;
        /* FPU programs are longer, so values live on in the host registers of one block. */
        bool fpu=rnd()%2u;
        unsigned count=fpu?2u+rnd()%14u:1u+rnd()%6u;
        for(unsigned i=0;i<count;i++){
            if(fpu&&rnd()%8u)gen_fp(&prog);else gen_one(&prog);
        }
        FpState fstart;fp_state_pick(&fstart);
        current_fp=&fstart;
        data_pick();
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
        FpState fref;fp_state_get(&fref);
        uint32_t ref_pc=m68k_get_reg(NULL,M68K_REG_PC);
        memcpy(ref_mem,c.ram_owned+DATA,DATA_BYTES);
        memcpy((uint8_t*)ref_mem+DATA_BYTES,c.ram_owned+STACK-0x10000u,0x10000u);

        c.jit_exec=jit_exec;
        load_program(&c,&prog,d,ccr);
        bool ok2=run_until_loop(&c,end,jit_exec,400);
        Regs got;snapshot(&got);
        FpState fgot;fp_state_get(&fgot);
        uint32_t got_pc=m68k_get_reg(NULL,M68K_REG_PC);
        if(ok1!=ok2){
            failures++;
            fprintf(stderr,"ONE SIDE HUNG program %u: musashi=%d pc=%08x jit=%d pc=%08x\n",n,ok1,ref_pc,ok2,got_pc);
            dump_program(&prog);
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
        bool same_fp=fp_same(&fref,&fgot);
        if(!same||!same_mem||!same_fp){
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
            for(unsigned i=0;i<8;i++)if(fref.fp[i].high!=fgot.fp[i].high||fref.fp[i].low!=fgot.fp[i].low)
                fprintf(stderr,"  FP%u ref=%04x:%016" PRIx64 " jit=%04x:%016" PRIx64 " start=%04x:%016" PRIx64 "\n",i,fref.fp[i].high,(uint64_t)fref.fp[i].low,fgot.fp[i].high,(uint64_t)fgot.fp[i].low,fstart.fp[i].high,(uint64_t)fstart.fp[i].low);
            if(fref.fpsr!=fgot.fpsr||fref.fpcr!=fgot.fpcr||fref.just_reset!=fgot.just_reset)
                fprintf(stderr,"  FPSR ref=%08x jit=%08x FPCR ref=%08x jit=%08x just_reset ref=%d jit=%d\n",fref.fpsr,fgot.fpsr,fref.fpcr,fgot.fpcr,fref.just_reset,fgot.just_reset);
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
