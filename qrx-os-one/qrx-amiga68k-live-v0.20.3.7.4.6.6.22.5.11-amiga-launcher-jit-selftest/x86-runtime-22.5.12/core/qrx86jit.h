/* Internal m68k baseline-ISA backend, included by qrx86core.c.
 * Register operations and one prevalidated leading 32-bit load. No code from the legacy threaded JIT.
 * Immutable native blocks; exact byte validation at EVERY entry (including raw
 * host writes). No MMU dependency, direct chaining, asynchronous guest threads,
 * persistent cache, speculative loads, or host CCR == x86 EFLAGS assumption.
 */
#include <time.h>
#if defined(__m68k__) && defined(__AROS__)
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#define XJ_NATIVE 1
#elif defined(__m68k__) && defined(QRX_JIT_POSIX)
#include <sys/mman.h>
#define XJ_NATIVE 1
#else
#define XJ_NATIVE 0
#endif

#ifndef XJ_SLOTS
#define XJ_SLOTS 8192
#endif
_Static_assert(XJ_SLOTS >= 2 && !(XJ_SLOTS & (XJ_SLOTS-1)), "JIT slot count must be a power of two");
#ifndef QRX_JIT_NATIVE_LOADS
#define QRX_JIT_NATIVE_LOADS 0
#endif
#ifndef QRX_JIT_SIZE_LOOP
#ifdef __AROS__
#define QRX_JIT_SIZE_LOOP 0
#else
#define QRX_JIT_SIZE_LOOP 1
#endif
#endif
#ifndef QRX_JIT_TABLE_CANDIDATE
#define QRX_JIT_TABLE_CANDIDATE 1
#endif
#ifndef QRX_JIT_DIRTY_REGS
#ifdef __AROS__
#define QRX_JIT_DIRTY_REGS 1
#else
#define QRX_JIT_DIRTY_REGS 0
#endif
#endif
#ifndef QRX_JIT_FUSED_FETCH
#ifdef __AROS__
#define QRX_JIT_FUSED_FETCH 1
#else
#define QRX_JIT_FUSED_FETCH 0
#endif
#endif
#define XJ_MAX_OPS 24
#define XJ_BYTES (XJ_MAX_OPS * 6)
#define XJ_CODE_CAP 4096

typedef struct xj_op { unsigned kind, dst, src, imm_src, index, scale; uint32_t imm, eip; } xj_op;
enum { XJ_MOV = 1, XJ_ADD, XJ_SUB, XJ_AND, XJ_OR, XJ_XOR, XJ_CMP, XJ_TEST, XJ_NOP, XJ_LEA, XJ_SHL, XJ_SHR, XJ_SAR, XJ_LOAD, XJ_INC, XJ_DEC, XJ_ADC, XJ_SBB };
typedef struct xj_block {
    uint32_t eip, end, last, target;
    unsigned len, count, terminal,native_cc;
    int cc;
    uint8_t bytes[XJ_BYTES];
    void *code;
    unsigned code_size;
    unsigned load,base,index,scale,cf_input;uint32_t disp;
} xj_block;
/* Cold/negative tags stay compact; snapshots exist only for published blocks. */
typedef struct xj_slot { uint32_t eip; unsigned hits; uint8_t retries; xj_block *block; } xj_slot;
struct x86jit { x86jit_stats stats; uint64_t base_icount; int trace,allow_loads; unsigned threshold,min_ops; xj_slot slots[XJ_SLOTS]; };

#if XJ_NATIVE
_Static_assert(sizeof(void *) == 4, "m68k JIT requires a 32-bit C ABI");
_Static_assert(offsetof(x86cpu,lf_b) < 32768, "CPU fields exceed m68k d16 displacement");
static void xj_code_free(void *code, unsigned n)
{
    if (!code) return;
#ifdef __AROS__
    (void)n; FreeVec(code);
#else
    munmap(code, n);
#endif
}

/* Everything emitted uses the common 68000 instruction subset. Profiles still
 * reject 68000/010, ColdFire and unknown CPUs; no unsupported multiply/divide,
 * FPU, CAS, MOVE16, privileged opcode or CPU-specific cache opcode is emitted. */
static int xj_profile(unsigned *p)
{
#ifdef __AROS__
    unsigned a = SysBase->AttnFlags, actual =
        (a & AFF_68060) ? 60 : (a & AFF_68040) ? 40 :
        (a & AFF_68030) ? 30 : (a & AFF_68020) ? 20 : 0;
    if (!actual || (*p && *p != actual)) return 0;
    *p = actual;
#else
    /* POSIX is an explicit test build; caller must select the emulated CPU. */
    if (!*p) return 0;
#endif
    return *p == 20 || *p == 30 || *p == 40 || *p == 60;
}

static int xj_decode(x86cpu *c, xj_block *b, xj_op *ops, unsigned *nops, int *last_flags)
{
    unsigned pos = 0, n = 0, limit = X86_PAGE_SIZE - (b->eip & X86_PAGE_MASK);
    if (limit > XJ_BYTES) limit = XJ_BYTES;
    if (!x86_read_mem(c, b->eip, b->bytes, limit)) return 0;
    int carry_live=1;
    b->cc = -1; *last_flags = -1;
    while (n < XJ_MAX_OPS && pos < limit) {
        unsigned start = pos, op = b->bytes[pos++], kind = 0, d = 0, s = 0, imm = 0;
        uint32_t val = 0;
        unsigned index = 8, scale = 0;
        if (op == 0x90) kind = XJ_NOP;
        else if(op>=0x40 && op<=0x4f){kind=op<0x48?XJ_INC:XJ_DEC;d=op&7;imm=1;val=1;}
        else if (op == 0x8d || (op==0x8b && QRX_JIT_NATIVE_LOADS && c->jit->allow_loads && pos<limit && (b->bytes[pos]&0xc0)!=0xc0)) {
            unsigned m, mod;
            /* A load may only start a block: all guards run before any guest effect. */
            if(op==0x8b && n){pos=start;break;}
            if (pos >= limit) { pos=start; break; }
            m=b->bytes[pos++];mod=m>>6;d=(m>>3)&7;s=m&7;
            if (mod==3) { pos=start; break; }
            if (s==4) {
                unsigned sib;
                if (pos>=limit) { pos=start; break; }
                sib=b->bytes[pos++];s=sib&7;index=(sib>>3)&7;scale=sib>>6;
                if(index==4)index=8;
            }
            if (mod==0 && s==5) {
                if(pos+4>limit){pos=start;break;}
                val=ld32(b->bytes+pos);pos+=4;s=8;
            } else if(mod==1) {
                if(pos>=limit){pos=start;break;}
                val=(uint32_t)(int32_t)(int8_t)b->bytes[pos++];
            } else if(mod==2) {
                if(pos+4>limit){pos=start;break;}
                val=ld32(b->bytes+pos);pos+=4;
            }
            kind=op==0x8d?XJ_LEA:XJ_LOAD;
            if(kind==XJ_LOAD){b->load=1;b->base=s;b->index=index;b->scale=scale;b->disp=val;}
        } else if (op==0xc1 || op==0xd1) {
            unsigned m,g;
            if(pos>=limit || (b->bytes[pos]&0xc0)!=0xc0){pos=start;break;}
            m=b->bytes[pos++];g=(m>>3)&7;d=m&7;
            if(g<4){pos=start;break;}
            val=1;
            if(op==0xc1){if(pos>=limit){pos=start;break;}val=b->bytes[pos++]&31;}
            kind=!val?XJ_NOP:(g==4||g==6)?XJ_SHL:g==5?XJ_SHR:XJ_SAR;imm=1;
        } else if(op==0x0f) {
            unsigned op2;
            if(pos+5>limit || (b->bytes[pos]&0xf0)!=0x80){pos=start;break;}
            op2=b->bytes[pos++];val=ld32(b->bytes+pos);pos+=4;
            b->cc=op2&15;b->target=b->eip+pos+val;
            b->terminal=1;b->last=b->eip+start;n++;break;
        } else if (op >= 0xb8 && op <= 0xbf) {
            if (pos + 4 > limit) { pos = start; break; }
            d = op & 7; val = ld32(b->bytes + pos); pos += 4; kind = XJ_MOV; imm = 1;
        } else if ((op >= 0x70 && op <= 0x7f) || op == 0xeb || op == 0xe9) {
            int32_t rel;
            if (op == 0xe9) { if (pos + 4 > limit) { pos = start; break; } rel = (int32_t)ld32(b->bytes + pos); pos += 4; }
            else { if (pos >= limit) { pos = start; break; } rel = (int8_t)b->bytes[pos++]; }
            b->cc = op < 0x80 ? (int)(op & 15) : -1;
            b->target = b->eip + pos + (uint32_t)rel;
            b->terminal = 1; b->last = b->eip + start; n++;
            break;
        } else if (op == 0x89 || op == 0x8b || op == 0x01 || op == 0x03 ||
                   op == 0x11 || op == 0x13 || op == 0x19 || op == 0x1b ||
                   op == 0x29 || op == 0x2b || op == 0x21 || op == 0x23 ||
                   op == 0x09 || op == 0x0b || op == 0x31 || op == 0x33 ||
                   op == 0x39 || op == 0x3b || op == 0x85 || op == 0x81 || op == 0x83) {
            unsigned m, group;
            if (pos >= limit || (b->bytes[pos] & 0xc0) != 0xc0) { pos = start; break; }
            m = b->bytes[pos++]; d = m & 7; s = (m >> 3) & 7;
            if (op == 0x81 || op == 0x83) {
                group = s; imm = 1;
                /* Carry-consuming groups are admitted only while entry CF is live. */
                if (op == 0x81) { if (pos + 4 > limit) { pos = start; break; } val = ld32(b->bytes + pos); pos += 4; }
                else { if (pos >= limit) { pos = start; break; } val = (uint32_t)(int32_t)(int8_t)b->bytes[pos++]; }
            } else if (op == 0x89 || op == 0x8b) {
                kind = XJ_MOV; group = 0;
                if (op == 0x8b) { unsigned t = d; d = s; s = t; }
            } else if (op == 0x85) { kind = XJ_TEST; group = 0; }
            else { group = op >> 3; if (op & 2) { unsigned t = d; d = s; s = t; } }
            if (!kind) {
                static const unsigned kinds[8] = { XJ_ADD, XJ_OR, XJ_ADC, XJ_SBB, XJ_AND, XJ_SUB, XJ_XOR, XJ_CMP };
                kind = kinds[group];
            }
        } else if (op == 0x05 || op == 0x0d || op == 0x15 || op == 0x1d || op == 0x25 || op == 0x2d || op == 0x35 || op == 0x3d || op == 0xa9) {
            static const unsigned kinds[8] = { XJ_ADD, XJ_OR, XJ_ADC, XJ_SBB, XJ_AND, XJ_SUB, XJ_XOR, XJ_CMP };
            if (pos + 4 > limit) { pos = start; break; }
            kind = op == 0xa9 ? XJ_TEST : kinds[op >> 3];
            val = ld32(b->bytes + pos); pos += 4; imm = 1;
        } else { pos = start; break; } /* prefixes, memory, x87, stacks, faults: interpreter */
        if(kind==XJ_INC || kind==XJ_DEC || kind==XJ_ADC || kind==XJ_SBB) {
            if(!carry_live){pos=start;break;}
            b->cf_input=1;
            if(kind==XJ_ADC || kind==XJ_SBB)carry_live=0;
        } else if(kind!=XJ_MOV && kind!=XJ_NOP && kind!=XJ_LEA && kind!=XJ_LOAD)carry_live=0;
        ops[n].kind = kind; ops[n].dst = d; ops[n].src = s;
        ops[n].index=index;ops[n].scale=scale;
        ops[n].imm_src = imm; ops[n].imm = val; ops[n].eip = b->eip + start;
        if (kind != XJ_MOV && kind != XJ_NOP && kind != XJ_LEA && kind != XJ_LOAD) *last_flags = (int)n;
        b->last = b->eip + start; n++;
    }
    if (n < c->jit->min_ops) { b->len = limit < 16 ? limit : 16; b->count = 0; return 0; }
    /* A failed trailing decode must not consume its opcode or immediate. */
    b->len = pos; b->end = b->eip + pos; b->count = n;
    *nops = n - b->terminal;
    return 1;
}

typedef struct xj_emit { uint8_t *p; unsigned n; int bad; } xj_emit;
static void xj_w16(xj_emit *e, unsigned v)
{
    if (e->n + 2 > XJ_CODE_CAP) { e->bad = 1; return; }
    e->p[e->n++] = v >> 8; e->p[e->n++] = v;
}
static void xj_w32(xj_emit *e, uint32_t v) { xj_w16(e, v >> 16); xj_w16(e, v); }
static void xj_load(xj_emit *e, unsigned dn, unsigned reg)
{ xj_w16(e, 0x2028 | (dn << 9)); xj_w16(e, reg * 4); }
static void xj_imm(xj_emit *e, unsigned dn, uint32_t v)
{ xj_w16(e, 0x203c | (dn << 9)); xj_w32(e, v); }
static void xj_store(xj_emit *e, unsigned dn, unsigned off)
{ xj_w16(e, 0x2140 | dn); xj_w16(e, off); }
static void xj_store_imm(xj_emit *e, uint32_t v, unsigned off)
{ xj_w16(e, 0x217c); xj_w32(e, v); xj_w16(e, off); }
/* One caller-saved register cache. No fault/callback occurs inside a block;
 * dirty guest state is committed before eviction and at every native exit. */
static void xj_flush_reg(xj_emit *e,int loaded,int *dirty)
{
    if(QRX_JIT_DIRTY_REGS && *dirty && loaded>=0)xj_store(e,0,(unsigned)loaded*4);
    *dirty=0;
}
#define XJ_OFF(field) ((unsigned)offsetof(x86cpu, field))

static int xj_compile_body(x86cpu *c, xj_block *b)
{
    xj_op ops[XJ_MAX_OPS]; unsigned nops = 0, i; int final_flags, loaded = -1,dirty=0;
    uint8_t tmp[XJ_CODE_CAP]; xj_emit e = { tmp, 0, 0 };
    void *code;
    if (!xj_decode(c, b, ops, &nops, &final_flags)) return 0;
    /* C ABI: argument at 4(sp), only caller-saved D0/D1/A0 are clobbered.
     * No host stack frame, helper call or cached SP. A leading load uses only
     * a same-page host pointer validated before any guest effect. A1 is caller-saved. */
    xj_w16(&e, 0x206f); xj_w16(&e, 4); /* movea.l 4(sp),a0 */
    for (i = 0; i < nops; i++) {
        xj_op *o = &ops[i]; unsigned k = o->kind;
        if (k == XJ_NOP) continue;
        if(QRX_JIT_DIRTY_REGS && (k==XJ_CMP||k==XJ_TEST) && (int)i!=final_flags)continue;
        if(k==XJ_LOAD) {
            xj_w16(&e,0x226f);xj_w16(&e,8); /* prevalidated host pointer, second argument */
            xj_w16(&e,0x2011); /* move.l (a1),d0, same mapped page */
            xj_w16(&e,0xe058);xj_w16(&e,0x4840);xj_w16(&e,0xe058); /* little endian */
            if(QRX_JIT_DIRTY_REGS)dirty=1;else xj_store(&e,0,o->dst*4);loaded=o->dst;continue;
        }
        if(k==XJ_LEA) {
            xj_flush_reg(&e,loaded,&dirty);
            if(o->index<8) {
                xj_load(&e,0,o->index);
                if(o->scale)xj_w16(&e,0xe188|(o->scale<<9));
                if(o->src<8){xj_load(&e,1,o->src);xj_w16(&e,0xd081);}
                if(o->imm){xj_w16(&e,0x0680);xj_w32(&e,o->imm);}
            } else {
                if(o->src<8)xj_load(&e,0,o->src);else xj_imm(&e,0,0);
                if(o->imm){xj_w16(&e,0x0680);xj_w32(&e,o->imm);}
            }
            if(QRX_JIT_DIRTY_REGS)dirty=1;else xj_store(&e,0,o->dst*4);loaded=o->dst;continue;
        }
        if (k == XJ_MOV) {
            if(o->imm_src) {
                if(loaded!=(int)o->dst)xj_flush_reg(&e,loaded,&dirty);else dirty=0;
                xj_imm(&e,0,o->imm);
            } else if(loaded!=(int)o->src) {
                if(loaded!=(int)o->dst)xj_flush_reg(&e,loaded,&dirty);else dirty=0;
                xj_load(&e,0,o->src);
            } else if(loaded!=(int)o->dst)xj_flush_reg(&e,loaded,&dirty);
            if(QRX_JIT_DIRTY_REGS)dirty=1;else xj_store(&e,0,o->dst*4);
            loaded=o->dst;continue;
        }
        if(loaded!=(int)o->dst){xj_flush_reg(&e,loaded,&dirty);xj_load(&e,0,o->dst);loaded=o->dst;}
        if(k==XJ_CMP||k==XJ_TEST)xj_flush_reg(&e,loaded,&dirty);
        if(o->imm_src)xj_imm(&e,1,o->imm);
        else if(QRX_JIT_DIRTY_REGS && loaded==(int)o->src)xj_w16(&e,0x2200); /* move.l d0,d1 */
        else xj_load(&e,1,o->src);
        if ((int)i == final_flags) {
            xj_store(&e, 0, XJ_OFF(lf_a)); xj_store(&e, 1, XJ_OFF(lf_b));
        }
        switch (k) {
        case XJ_INC: case XJ_ADD: xj_w16(&e, 0xd081); break; /* add.l d1,d0 */
        case XJ_DEC: case XJ_SUB: case XJ_CMP: xj_w16(&e, 0x9081); break;
        case XJ_AND: case XJ_TEST: xj_w16(&e, 0xc081); break;
        case XJ_OR: xj_w16(&e, 0x8081); break;
        case XJ_XOR: xj_w16(&e, 0xb380); break;
        case XJ_ADC:xj_w16(&e,0xd081);xj_w16(&e,0xd0a8);xj_w16(&e,XJ_OFF(lf_cf));break;
        case XJ_SBB:xj_w16(&e,0x9081);xj_w16(&e,0x90a8);xj_w16(&e,XJ_OFF(lf_cf));break;
        case XJ_SHL:xj_w16(&e,0xe3a8);break;
        case XJ_SHR:xj_w16(&e,0xe2a8);break;
        case XJ_SAR:xj_w16(&e,0xe2a0);break;
        default: return 0;
        }
        /* Capture only proven matching arithmetic conditions before stores alter
         * CCR. Parity, shifts and carry-consuming/partial producers use C flags. */
        if((int)i==final_flags && b->terminal && b->cc>=0 && b->cc!=10 && b->cc!=11 &&
           (k==XJ_ADD || k==XJ_SUB || k==XJ_CMP || k==XJ_AND || k==XJ_OR || k==XJ_XOR || k==XJ_TEST)) {
            static const unsigned host_cc[16]={9,8,5,4,7,6,3,2,11,10,0,0,13,12,15,14};
            xj_w16(&e,0x50c1|(host_cc[b->cc]<<8)); /* Scc d1, low byte only */
            xj_w16(&e,0x2241); /* movea.l d1,a1; survives following register operations */
            b->native_cc=1;
        }
        if ((int)i == final_flags) {
            unsigned lf = k==XJ_INC?LF_INC:k==XJ_DEC?LF_DEC:k==XJ_ADC?LF_ADC:k==XJ_SBB?LF_SBB:k == XJ_ADD ? LF_ADD : (k == XJ_SUB || k == XJ_CMP) ? LF_SUB : k==XJ_SHL?LF_SHL:k==XJ_SHR?LF_SHR:k==XJ_SAR?LF_SAR:LF_LOGIC;
            xj_store(&e, 0, XJ_OFF(lf_res));
            xj_store_imm(&e, lf, XJ_OFF(lf_op)); xj_store_imm(&e, 32, XJ_OFF(lf_sz));
        }
        if(k!=XJ_CMP && k!=XJ_TEST) {
            if(QRX_JIT_DIRTY_REGS)dirty=1;else xj_store(&e,0,o->dst*4);
            loaded=o->dst;
        } else {loaded=-1;dirty=0;}
    }
    xj_flush_reg(&e,loaded,&dirty);
    if(b->native_cc){xj_w16(&e,0x2009);xj_w16(&e,0x0280);xj_w32(&e,1);}
    xj_w16(&e, 0x4e75);
    if (e.bad) return 0;
#ifdef __AROS__
    code = AllocVec(e.n, MEMF_PUBLIC);
    if (!code || ((uintptr_t)code & 1)) { if (code) FreeVec(code); return 0; }
    memcpy(code, tmp, e.n);
    CacheClearE(code, e.n, CACRF_ClearI | CACRF_ClearD);
#else
    code = mmap(NULL, e.n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 0;
    memcpy(code, tmp, e.n);
    __builtin___clear_cache((char *)code, (char *)code + e.n);
    if (mprotect(code, e.n, PROT_READ | PROT_EXEC)) { munmap(code, e.n); return 0; }
#endif
    /* Publish only after complete emission and instruction-cache synchronization. */
    b->code_size = e.n; b->code = code;
    c->jit->stats.compilations++; c->jit->stats.code_bytes += e.n;
    c->jit->stats.live_code_bytes += e.n;
    if (c->jit->stats.live_code_bytes > c->jit->stats.peak_code_bytes)
        c->jit->stats.peak_code_bytes = c->jit->stats.live_code_bytes;
    return 1;
}

/* SDK clock() occasionally moves backwards by one tick. Use unsigned deltas
 * to accept genuine counter wrap, reject regressing/error samples explicitly. */
static uint32_t xj_tick_delta(x86jit_stats *stats,uint32_t start,uint32_t end)
{
    uint32_t delta=end-start;
    if(delta>0x7fffffffu){stats->clock_invalid++;return 0;}
    return delta;
}
static uint32_t xj_elapsed(x86jit_stats *stats,clock_t start)
{
    clock_t end=clock();
    if(start==(clock_t)-1 || end==(clock_t)-1){stats->clock_invalid++;return 0;}
    return xj_tick_delta(stats,(uint32_t)start,(uint32_t)end);
}
static int xj_compile(x86cpu *c, xj_block *b)
{
    clock_t t = clock();
    int ok = xj_compile_body(c,b);
    c->jit->stats.compile_ticks += xj_elapsed(&c->jit->stats,t);
    return ok;
}

/* Reject memory operands and unsupported families before touching the cache.
 * The interpreter's fetch window is read without changing EIP or fault state. */
X86_INLINE unsigned xj_peek(x86cpu *c, uint32_t pc)
{
    uint32_t o = pc - c->f_lo;
    x86page *p;
    if(o<c->f_lim)return c->f_base[o];
    p=pg(c,pc);return p?p->mem[pc&X86_PAGE_MASK]:256;
}
X86_INLINE int xj_candidate_op(x86cpu *c,uint32_t pc,unsigned op)
{
#if QRX_JIT_TABLE_CANDIDATE
    static const uint8_t classes[256]={
        [0x40 ... 0x4f]=1,[0x90]=1,[0xb8 ... 0xbf]=1,[0x15]=1,[0x1d]=1,[0x05]=1,[0x0d]=1,[0x25]=1,[0x2d]=1,[0x35]=1,[0x3d]=1,[0xa9]=1,
        [0x11]=2,[0x13]=2,[0x19]=2,[0x1b]=2,[0x89]=2,[0x01]=2,[0x03]=2,[0x29]=2,[0x2b]=2,[0x21]=2,[0x23]=2,[0x09]=2,[0x0b]=2,
        [0x31]=2,[0x33]=2,[0x39]=2,[0x3b]=2,[0x85]=2,[0x81]=3,[0x83]=3,[0xc1]=4,[0xd1]=4,
        [0x8d]=5,[0x8b]=6
    };
    unsigned kind,m;
    if(op>255 || !(kind=classes[op]))return 0;
    if(kind==1)return 1;
    m=xj_peek(c,pc+1);
    if(kind==6)return (QRX_JIT_NATIVE_LOADS && c->jit->allow_loads) || (m&0xc0)==0xc0;
    if(kind==5)return m<256 && (m&0xc0)!=0xc0;
    if((m&0xc0)!=0xc0)return 0;
    if(kind==3)return 1;
    if(kind==4)return ((m>>3)&7)>=4;
    return 1;
#else
    unsigned m;
    if(op>255)return 0;
    if(op<0x40) {
        unsigned form=op&7;
        if(form==5)return 1;
        if(form!=1 && form!=3)return 0;
    } else if((op>=0x40&&op<=0x4f) || (op>=0xb8&&op<=0xbf) || op==0x90 || op==0xa9)return 1;
    else if(op==0x8d){m=xj_peek(c,pc+1);return m<256&&(m&0xc0)!=0xc0;}
    else if(op==0x8b && QRX_JIT_NATIVE_LOADS && c->jit->allow_loads)return 1;
    else if(op==0xc1 || op==0xd1){m=xj_peek(c,pc+1);return (m&0xc0)==0xc0&&((m>>3)&7)>=4;}
    else if(op!=0x89 && op!=0x8b && op!=0x85 && op!=0x81 && op!=0x83)return 0;
    m=xj_peek(c,pc+1);return (m&0xc0)==0xc0;
#endif
}
X86_INLINE int xj_candidate(x86cpu *c)
{ return xj_candidate_op(c,c->eip,xj_peek(c,c->eip)); }

/* Blocks never cross a page. Resolve the current mapping, then compare raw
 * words without a temporary copy or a shared-CRT call on every native entry.
 * memcpy preserves alignment/aliasing rules; all selected CPUs allow unaligned
 * long loads. No guest pointer or host page pointer survives mapping changes. */
X86_INLINE int xj_fresh(x86cpu *c, const xj_block *b)
{
    x86page *p = pg(c,b->eip);
    unsigned off=b->eip & X86_PAGE_MASK, i=0;
    const uint8_t *live;
    if(!p || b->len > X86_PAGE_SIZE-off) return 0;
    live=p->mem+off;
    while(i+4<=b->len) {
        uint32_t a,z;
        __builtin_memcpy(&a,live+i,4);__builtin_memcpy(&z,b->bytes+i,4);
        if(a!=z)return 0;i+=4;
    }
    while(i<b->len){if(live[i]!=b->bytes[i])return 0;i++;}
    return 1;
}
X86_INLINE int xj_try(x86cpu *c,uint32_t stop,uint32_t pc,unsigned charged)
{
    struct x86jit *j = c->jit;
    unsigned slot_index=(pc ^ (pc >> 7)) & (XJ_SLOTS-1);
    xj_slot *slot=&j->slots[slot_index];xj_block *b=slot->block;
    clock_t t = 0;unsigned taken;
    const uint8_t *load_ptr=NULL;
    if(j->stats.cache_profile)j->stats.lookups++;
    if(slot->hits && slot->eip==pc) {
        if(b && !xj_fresh(c,b)) {
            j->stats.invalidations++;j->stats.live_code_bytes-=b->code_size;
            xj_code_free(b->code,b->code_size);free(b);memset(slot,0,sizeof(*slot));b=NULL;
        } else if(j->stats.cache_profile)j->stats.hits++;
    } else if(slot->hits) {
        j->stats.evictions++;
        if(b){j->stats.live_code_bytes-=b->code_size;xj_code_free(b->code,b->code_size);free(b);}
        memset(slot,0,sizeof(*slot));b=NULL;
    }
    if(!slot->hits){j->stats.misses++;slot->eip=pc;slot->hits=1;return 0;}
    if(!b && slot->hits>=j->threshold && !++slot->retries)slot->hits=j->threshold-1;
    if(slot->hits<j->threshold) {
        if(++slot->hits<j->threshold)return 0;
        b=(xj_block *)calloc(1,sizeof(*b));
        if(!b)return 0;
        b->eip=pc;
        if(!xj_compile(c,b)){free(b);return 0;}
        slot->block=b;
    }
    if(!b || (charged ? b->count-1 > c->left : b->count > c->left) || ((uint32_t)(stop - b->eip)<b->len))return 0;
    if(QRX_JIT_NATIVE_LOADS && b->load) {
        uint32_t addr=b->disp+c->sbase[(b->base==X86_ESP||b->base==X86_EBP)?X86_SS:X86_DS];unsigned off;x86page *p;
        if(b->base<8)addr+=c->r[b->base];
        if(b->index<8)addr+=c->r[b->index]<<b->scale;
        off=addr&X86_PAGE_MASK;
        if(off>X86_PAGE_SIZE-4)return 0;
        p=pg(c,addr);if(!p)return 0;
        load_ptr=p->mem+off;
    }
    if (j->trace) {
        unsigned i;
        fprintf(stderr,"QRX86-JIT enter profile=680%u slot=%u guest=%08x host=%p bytes=",
                j->stats.profile,slot_index,b->eip,b->code);
        for(i=0;i<b->code_size;i++) fprintf(stderr,"%02x",((const uint8_t *)b->code)[i]);
        for(i=0;i<8;i++) fprintf(stderr," r%u=%08x",i,c->r[i]);
        fputc('\n',stderr);fflush(stderr); /* survives a host CPU exception; no recovery claim */
    }
    if(b->cf_input)c->lf_cf=get_cf(c)?X86_CF:0;
    if (j->stats.timing) t = clock();
    #if QRX_JIT_NATIVE_LOADS
    taken=((unsigned (*)(x86cpu *,const uint8_t *))b->code)(c,load_ptr);
#else
    taken=((unsigned (*)(x86cpu *))b->code)(c);
#endif
    c->left -= b->count-charged; c->cur_eip = b->last;
    c->eip = b->terminal && (b->cc < 0 || (b->native_cc ? taken : cond(c, b->cc))) ? b->target : b->end;
    if (j->stats.timing) j->stats.native_ticks += xj_elapsed(&j->stats,t);
    j->stats.native_blocks++; j->stats.native_instructions += b->count;
    return 1;
}
#endif

int x86_jit_enable(x86cpu *c, unsigned profile, int timing)
{
#if XJ_NATIVE
    struct x86jit *j;
    if (c->jb || !xj_profile(&profile)) return 0;
    if (c->jit) return c->jit->stats.profile == profile;
    j = (struct x86jit *)calloc(1, sizeof(*j));
    if (!j) return 0;
    j->allow_loads=getenv("QRX86_JIT_LOADS")!=NULL;
    j->min_ops=getenv("QRX86_JIT_MIN_OPS")?(unsigned)strtoul(getenv("QRX86_JIT_MIN_OPS"),NULL,0):3;
    if(j->min_ops<3)j->min_ops=3;if(j->min_ops>XJ_MAX_OPS)j->min_ops=XJ_MAX_OPS;
    j->threshold = getenv("QRX86_JIT_THRESHOLD") ? (unsigned)strtoul(getenv("QRX86_JIT_THRESHOLD"),NULL,0) : 16;
    if (j->threshold < 2) j->threshold = 2;
    j->stats.cache_profile=getenv("QRX86_JIT_CACHE_PROFILE")!=NULL;
    j->base_icount = x86_icount(c);
    j->stats.profile = profile; j->stats.timing = timing != 0; j->trace = getenv("QRX86_JIT_TRACE") != NULL; c->jit = j; return 1;
#else
    (void)c; (void)profile; (void)timing; return 0;
#endif
}
void x86_jit_disable(x86cpu *c)
{
#if XJ_NATIVE
    unsigned i;
    if(c->jit)for(i=0;i<XJ_SLOTS;i++) {
        xj_block *b=c->jit->slots[i].block;
        if(b){xj_code_free(b->code,b->code_size);free(b);}
    }
#endif
    free(c->jit); c->jit = NULL;
}
void x86_jit_snapshot(const x86cpu *c, x86jit_stats *out)
{
    if (c->jit) {
        *out = c->jit->stats;
        /* Includes REP's charged iterations and callback instructions, not just
         * one count per C dispatch. No per-fallback 64-bit increment is needed. */
        out->fallback_instructions = x86_icount(c) - c->jit->base_icount - out->native_instructions;
    } else memset(out, 0, sizeof(*out));
}
