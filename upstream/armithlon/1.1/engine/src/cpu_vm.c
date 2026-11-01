#include "qrxvirt.h"
#include <stdint.h>

static void logic_flags(QRXCPU *c, uint32_t val, unsigned bits) {
    uint32_t mask = bits == 32 ? UINT32_MAX : (1u << bits)-1u;
    uint32_t sign = 1u << (bits-1u);
    val &= mask;
    c->sr = (uint16_t)((c->sr & 0xfff0u) | (val == 0 ? QRX_CCR_Z : 0) |
                       ((val & sign) ? QRX_CCR_N : 0));
}
static void arithmetic_flags(QRXCPU *c, uint32_t old, uint32_t n, uint32_t result,
                             unsigned bits, bool subtraction) {
    uint32_t mask = bits == 32 ? UINT32_MAX : ((1u<<bits)-1);
    uint32_t sign = 1u<<(bits-1u);
    old &= mask; n &= mask; result &= mask;
    bool carry = subtraction ? old < n : (uint64_t)old+n > mask;
    bool overflow = subtraction ? (((old^n)&(old^result)&sign)!=0) :
                                  (((~(old^n))&(old^result)&sign)!=0);
    unsigned ccr = (result == 0 ? QRX_CCR_Z:0) | (result&sign ? QRX_CCR_N:0) |
                   (overflow ? QRX_CCR_V:0) | (carry ? QRX_CCR_C|QRX_CCR_X:0);
    c->sr=(uint16_t)((c->sr&0xffe0u)|ccr);
}
static uint32_t size_mask(unsigned width) {
    return width == 4 ? UINT32_MAX : (width==2 ? 0xffffu : 0xffu);
}
static uint32_t place_lower(uint32_t old, uint32_t val, unsigned width) {
    uint32_t mask=size_mask(width);
    return (old&~mask)|(val&mask);
}
static bool fetch(QRXVM *v, uint32_t pc, unsigned width, uint32_t *word) {
    /* 68k instructions must be word aligned. Data may be unaligned (68020+). */
    return !(pc&1u) && qrx_vm_read(v,pc,width,word);
}
static bool push32(QRXVM *v, uint32_t x) {
    if (v->cpu.a[7]<4) return false;
    v->cpu.a[7]-=4;
    if (!qrx_vm_write(v,v->cpu.a[7],4,x)) {v->cpu.a[7]+=4; return false;}
    if (v->cpu.sr & QRX_SR_S) v->cpu.ssp=v->cpu.a[7];
    return true;
}
static bool pop32(QRXVM *v, uint32_t *x) {
    if (!qrx_vm_read(v,v->cpu.a[7],4,x)) return false;
    v->cpu.a[7]+=4;
    if (v->cpu.sr & QRX_SR_S) v->cpu.ssp=v->cpu.a[7];
    return true;
}
static bool condition(unsigned cc, uint16_t sr) {
    bool n=(sr&QRX_CCR_N)!=0, z=(sr&QRX_CCR_Z)!=0;
    bool v=(sr&QRX_CCR_V)!=0, c=(sr&QRX_CCR_C)!=0;
    switch (cc) {
        case 0: return true; case 1: return false; /* BSR separately */
        case 2: return !c&&!z; case 3: return c||z;
        case 4: return !c; case 5: return c;
        case 6: return !z; case 7: return z;
        case 8: return !v; case 9: return v;
        case 10: return !n; case 11: return n;
        case 12: return n==v; case 13: return n!=v;
        case 14: return !z&&(n==v); case 15: return z||(n!=v);
    }
    return false;
}
/* Unsupported opcodes dispatch vector #4; invalid memory vector #2. */
bool qrx_vm_step(QRXVM *v) {
    if (!v || !v->ram.data || v->cpu.halted || v->stopped) return false;
    QRXCPU *c=&v->cpu;
    uint32_t pc=c->pc, raw=0, ext=0, target=0;
    unsigned r=0;
    if (pc & 1u) return qrx_vm_exception(v,3,pc);
    if (!fetch(v,pc,2,&raw)) return qrx_vm_exception(v,2,pc);
    uint16_t op=(uint16_t)raw;
    uint32_t next=pc+2;
    /* MOVEQ #imm8,Dn, including the sign-extension and CCR. */
    if ((op&0xf100u)==0x7000u) {
        r=(op>>9)&7; c->d[r]=(uint32_t)(int32_t)(int8_t)(op&255u);
        logic_flags(c,c->d[r],32); goto done;
    }
    if (op==0x4e71u) goto done; /* NOP */
    if (op==0x4e75u) { /* RTS */
        if (!pop32(v,&target)) goto bus_error;
        next=target; goto done;
    }
    if (op==0x4e73u) { /* RTE, simplified qrxvirt six-byte exception frame */
        if (!(c->sr & QRX_SR_S)) return qrx_vm_exception(v,8,pc);
        uint32_t sr=0,ret=0;
        if (!qrx_vm_read(v,c->a[7],2,&sr) || !qrx_vm_read(v,c->a[7]+2,4,&ret)) goto bus_error;
        c->a[7]+=6; c->ssp=c->a[7];
        c->sr=(uint16_t)sr;
        if (!(c->sr&QRX_SR_S)) c->a[7]=c->usp;
        next=ret; goto done;
    }
    if (op==0x4e72u) { /* STOP #SR: supervisor only, no IRQ controller yet */
        if (!(c->sr & QRX_SR_S)) return qrx_vm_exception(v,8,pc);
        if (!fetch(v,next,2,&ext)) goto bus_error;
        c->sr=(uint16_t)ext; next+=2;
        v->stopped=true; goto done;
    }
    if ((op&0xfff0u)==0x4e40u) { /* TRAP #n: real exception, NOT foundation halt */
        v->steps++;
        return qrx_vm_exception(v,32u+(op&15u),next);
    }
    if (op==0x4afcu) return qrx_vm_exception(v,4,pc); /* ILLEGAL */
    if ((op&0xfff8u)==0x4e60u || (op&0xfff8u)==0x4e68u) { /* MOVE USP */
        if (!(c->sr&QRX_SR_S)) return qrx_vm_exception(v,8,pc);
        r=op&7; if (op&8) c->a[r]=c->usp; else c->usp=c->a[r];
        goto done;
    }
    if (op==0x46fcu) { /* MOVE #imm,SR */
        if (!(c->sr&QRX_SR_S)) return qrx_vm_exception(v,8,pc);
        if (!fetch(v,next,2,&ext)) goto bus_error;
        next+=2;
        uint16_t old=c->sr;
        c->sr=(uint16_t)ext;
        if ((old&QRX_SR_S)&&!(c->sr&QRX_SR_S)) {c->ssp=c->a[7]; c->a[7]=c->usp;}
        goto done;
    }
    if (op==0x44fcu) { /* MOVE #imm,CCR */
        if (!fetch(v,next,2,&ext)) goto bus_error;
        c->sr=(uint16_t)((c->sr&0xffe0u)|(ext&0x1fu)); next+=2; goto done;
    }
    if ((op&0xf000u)==0x6000u) { /* BRA/BSR/Bcc, byte/word/020-long */
        unsigned cc=(op>>8)&15;
        int32_t disp=(int8_t)(op&255u);
        if ((op&255u)==0) {
            if (!fetch(v,next,2,&ext)) goto bus_error;
            disp=(int16_t)ext; next+=2;
        } else if ((op&255u)==255u) {
            if (!fetch(v,next,4,&ext)) goto bus_error;
            disp=(int32_t)ext; next+=4;
        }
        if (cc==1u) {if (!push32(v,next)) goto bus_error;}
        if (cc==1u || condition(cc,c->sr)) next=(pc+2u)+(uint32_t)disp;
        goto done;
    }
    /* ADDQ/SUBQ only Dn, An for now. A-register arithmetic does not change CCR. */
    if ((op&0xf000u)==0x5000u && ((op>>6)&3u)!=3u && ((op>>3)&7u)<=1u) {
        unsigned mode=(op>>3)&7u;
        unsigned sc=(op>>6)&3u, width=sc==0?1u:(sc==1?2u:4u);
        bool sub=(op&0x100u)!=0;
        r=op&7;
        uint32_t n=(op>>9)&7u; if (!n) n=8;
        if (mode==1) {
            if (width==1) return qrx_vm_exception(v,4,pc);
            c->a[r]=sub ? c->a[r]-n : c->a[r]+n;
        } else {
            uint32_t mask=size_mask(width);
            uint32_t old=c->d[r]&mask;
            uint32_t result=sub ? old-n : old+n;
            c->d[r]=place_lower(c->d[r],result,width);
            arithmetic_flags(c,old,n,result,width*8,sub);
        }
        goto done;
    }
    if ((op&0xf1ffu)==0x203cu || (op&0xf1ffu)==0x207cu) { /* MOVE.L #imm,Dn/An */
        if (!fetch(v,next,4,&ext)) goto bus_error;
        r=(op>>9)&7; next+=4;
        if ((op&0xf1ffu)==0x207cu) c->a[r]=ext;
        else {c->d[r]=ext; logic_flags(c,ext,32);}
        goto done;
    }
    if ((op&0xf1f8u)==0x2000u) { /* MOVE.L Dn,Dn */
        r=(op>>9)&7; c->d[r]=c->d[op&7];
        logic_flags(c,c->d[r],32); goto done;
    }
    if ((op&0xf1f8u)==0x2010u) { /* MOVE.L (An),Dn */
        if (!qrx_vm_read(v,c->a[op&7],4,&ext)) goto bus_error;
        r=(op>>9)&7; c->d[r]=ext; logic_flags(c,ext,32); goto done;
    }
    if ((op&0xf1f8u)==0x2080u) { /* MOVE.L Dn,(An) */
        r=(op>>9)&7;
        if (!qrx_vm_write(v,c->a[r],4,c->d[op&7])) goto bus_error;
        logic_flags(c,c->d[op&7],32); goto done;
    }
    if ((op&0xf1ffu)==0x20bcu) { /* MOVE.L #imm,(An) */
        if (!fetch(v,next,4,&ext)) goto bus_error;
        r=(op>>9)&7;
        if (!qrx_vm_write(v,c->a[r],4,ext)) goto bus_error;
        next+=4; logic_flags(c,ext,32); goto done;
    }
    if ((op&0xf1ffu)==0x10bcu) { /* MOVE.B #imm,(An) */
        if (!fetch(v,next,2,&ext)) goto bus_error;
        r=(op>>9)&7;
        if (!qrx_vm_write(v,c->a[r],1,ext)) goto bus_error;
        next+=2; logic_flags(c,ext,8); goto done;
    }
    if ((op&0xf1ffu)==0x2039u) { /* MOVE.L (abs.l),Dn */
        if (!fetch(v,next,4,&target) || !qrx_vm_read(v,target,4,&ext)) goto bus_error;
        next+=4; r=(op>>9)&7; c->d[r]=ext; logic_flags(c,ext,32); goto done;
    }
    if ((op&0xfff8u)==0x23c0u) { /* MOVE.L Dn,(abs.l) */
        if (!fetch(v,next,4,&target) || !qrx_vm_write(v,target,4,c->d[op&7])) goto bus_error;
        next+=4; logic_flags(c,c->d[op&7],32); goto done;
    }
    if ((op&0xf1ffu)==0x41f9u) { /* LEA (abs.l),An */
        if (!fetch(v,next,4,&target)) goto bus_error;
        c->a[(op>>9)&7]=target; next+=4; goto done;
    }
    if (op==0x4ef9u || op==0x4eb9u) { /* JMP/JSR abs.l */
        if (!fetch(v,next,4,&target)) goto bus_error;
        next+=4; if (op==0x4eb9u && !push32(v,next)) goto bus_error;
        next=target; goto done;
    }
    if ((op&0xfff8u)==0x4ed0u || (op&0xfff8u)==0x4e90u) { /* JMP/JSR (An) */
        target=c->a[op&7u];
        if ((op&0xfff8u)==0x4e90u && !push32(v,next)) goto bus_error;
        next=target; goto done;
    }
    if ((op&0xfff8u)==0x4a80u) { /* TST.L Dn */
        logic_flags(c,c->d[op&7],32); goto done;
    }
    if ((op&0xfff8u)==0x4280u) { /* CLR.L Dn */
        c->d[op&7]=0; logic_flags(c,0,32); goto done;
    }
    if ((op&0xfff8u)==0x0c80u) { /* CMPI.L #imm,Dn */
        if (!fetch(v,next,4,&ext)) goto bus_error;
        r=op&7; next+=4;
        /* CMP changes NZVC but explicitly preserves X */
        uint16_t x=c->sr&QRX_CCR_X;
        arithmetic_flags(c,c->d[r],ext,c->d[r]-ext,32,true);
        c->sr=(uint16_t)((c->sr&~QRX_CCR_X)|x); goto done;
    }
    /* No unverified decoding: exception to the guest is safer than wrong execution. */
    return qrx_vm_exception(v,4,pc);
bus_error:
    v->steps++;
    return qrx_vm_exception(v,2,pc);
done:
    c->pc=next; v->steps++;
    return true;
}
int qrx_vm_run(QRXVM *v, uint64_t budget) {
    if (!v) return -1;
    for (uint64_t i=0; i<budget;) {
        if (v->cpu.halted || v->stopped) return v->cpu.fault ? -1 : 0;
        if (v->jit_enabled && qrx_jit_vm_block(v, budget-i)) {
            i += v->jit_count;
            continue;
        }
        if (!qrx_vm_step(v)) return -1;
        ++i;
    }
    return 1;
}
