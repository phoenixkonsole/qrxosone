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
#ifndef QRX_JIT_KEEP_POSITIVE
#define QRX_JIT_KEEP_POSITIVE 0
#endif
#ifndef QRX_JIT_PAGE_CACHE
#define QRX_JIT_PAGE_CACHE 0
#endif
#ifndef QRX_JIT_LOOP_BATCH
#define QRX_JIT_LOOP_BATCH 0
#endif
#ifndef QRX_JIT_CARRY_CHAIN
#define QRX_JIT_CARRY_CHAIN 0
#endif
#ifndef QRX_JIT_PERSIST_CC
#define QRX_JIT_PERSIST_CC 0
#endif
#ifndef QRX_JIT_MEMORY_CMP
#define QRX_JIT_MEMORY_CMP 0
#endif
#ifndef QRX_JIT_COMPACT_DEOPT
#define QRX_JIT_COMPACT_DEOPT 0
#endif
#ifndef QRX_JIT_FLAG_LIVENESS
#define QRX_JIT_FLAG_LIVENESS 0
#endif
#ifndef QRX_JIT_WRITE_CONTINUE
#define QRX_JIT_WRITE_CONTINUE 0
#endif
#ifndef QRX_JIT_NATIVE_MEMORY
#define QRX_JIT_NATIVE_MEMORY 0
#endif
_Static_assert(!QRX_JIT_PAGE_CACHE || QRX_JIT_NATIVE_MEMORY, "native page cache requires guarded memory");
_Static_assert(!QRX_JIT_CARRY_CHAIN || QRX_JIT_NATIVE_MEMORY, "carry chains require native memory metadata");
_Static_assert(!QRX_JIT_PERSIST_CC || QRX_JIT_NATIVE_MEMORY, "persistent conditions require native memory metadata");
_Static_assert(!QRX_JIT_LOOP_BATCH || (QRX_JIT_NATIVE_MEMORY && QRX_JIT_COMPACT_DEOPT && QRX_JIT_PERSIST_CC), "loop batching requires guarded memory, compact deopt and persistent conditions");
_Static_assert(!QRX_JIT_WRITE_CONTINUE || QRX_JIT_NATIVE_MEMORY, "write continuation requires native memory guards");
_Static_assert(!QRX_JIT_MEMORY_CMP || QRX_JIT_NATIVE_MEMORY, "memory CMP requires native memory guards");
_Static_assert(!QRX_JIT_COMPACT_DEOPT || QRX_JIT_NATIVE_MEMORY, "compact deopt requires native memory guards");
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

typedef struct xj_op { unsigned kind, dst, src, imm_src, index, scale; uint32_t imm, eip;
#if QRX_JIT_NATIVE_MEMORY
    unsigned base,width,sign,memreg,keep_flags,capture_cf;uint32_t end,operand;
#endif
} xj_op;
enum { XJ_MOV = 1, XJ_ADD, XJ_SUB, XJ_AND, XJ_OR, XJ_XOR, XJ_CMP, XJ_TEST, XJ_NOP, XJ_LEA, XJ_SHL, XJ_SHR, XJ_SAR, XJ_LOAD, XJ_INC, XJ_DEC, XJ_ADC, XJ_SBB, XJ_MEMLOAD, XJ_MEMSTORE, XJ_ZEXT, XJ_MEMCMP };
#if QRX_JIT_PAGE_CACHE
struct xj_page_cache {uint32_t tag;uintptr_t page;};
#endif
typedef struct xj_block {
    uint32_t eip, end, last, target;
    unsigned len, count, terminal,native_cc;
#if QRX_JIT_NATIVE_MEMORY
    unsigned memory,has_cf,loop_batch;
#if QRX_JIT_PAGE_CACHE
    uint64_t memory_serial;
    struct xj_page_cache pages[XJ_MAX_OPS];
#endif
#if QRX_JIT_COMPACT_DEOPT
    uint16_t boundaries[XJ_MAX_OPS+1];
#endif
#if QRX_JIT_WRITE_CONTINUE
    uintptr_t host_code_start;
#endif
#endif
    int cc;
    uint8_t bytes[XJ_BYTES];
    void *code;
    unsigned code_size;
    unsigned load,base,index,scale,cf_input;uint32_t disp;
} xj_block;
/* Cold/negative tags stay compact; snapshots exist only for published blocks. */
typedef struct xj_slot { uint32_t eip; unsigned hits; uint8_t retries; xj_block *block; } xj_slot;
struct x86jit { x86jit_stats stats; uint64_t base_icount; int trace,allow_loads; unsigned threshold,min_ops;
#if QRX_JIT_NATIVE_MEMORY
    uint32_t entry_cf;
#if QRX_JIT_PERSIST_CC
    uint32_t branch_taken; /* Data, never patched executable code. */
#if QRX_JIT_LOOP_BATCH
    uint32_t loop_done,loop_limit;
#endif
#endif
#endif
    xj_slot slots[XJ_SLOTS]; };

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
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_CARRY_CHAIN
    int last_cf=-1;
#endif
    b->cc = -1; *last_flags = -1;
    while (n < XJ_MAX_OPS && pos < limit) {
        unsigned start = pos, op = b->bytes[pos++], kind = 0, d = 0, s = 0, imm = 0;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_COMPACT_DEOPT
        b->boundaries[n]=start;
#endif
        uint32_t val = 0;
        unsigned index = 8, scale = 0;
#if QRX_JIT_NATIVE_MEMORY
        unsigned memory_base=8,memory_width=0,memory_sign=0,memory_reg=0;
        uint32_t memory_operand=0;
        unsigned compare=QRX_JIT_MEMORY_CMP&&(op==0x39||op==0x3b||op==0x81||op==0x83);
        if ((op==0x8b || op==0x89 || op==0x8a || op==0x88 || compare ||
             (op==0x0f && pos<limit && (b->bytes[pos]==0xb6 || b->bytes[pos]==0xb7 || b->bytes[pos]==0xbe || b->bytes[pos]==0xbf))) && pos<limit) {
            unsigned second=0,m,mod,rm;
            if(op==0x0f)second=b->bytes[pos++];
            if(pos>=limit){pos=start;break;}
            m=b->bytes[pos++];mod=m>>6;rm=m&7;memory_reg=(m>>3)&7;
            if((mod==3 && op!=0x0f) || (compare&&(op==0x81||op==0x83)&&memory_reg!=7)){pos=start+1; /* ordinary register/group decoding below */}
            else {
                memory_width=op==0x0f?((second&1)?2:1):(op==0x8a||op==0x88)?1:4;
                memory_sign=compare?(op==0x3b):(op==0x0f && (second&8));
                d=memory_reg;s=rm;
                if(mod==3)kind=XJ_ZEXT;
                else {
                    memory_base=rm;
                    if(rm==4){unsigned sib;if(pos>=limit){pos=start;break;}sib=b->bytes[pos++];memory_base=sib&7;index=(sib>>3)&7;scale=sib>>6;if(index==4)index=8;}
                    if(mod==0 && memory_base==5){if(pos+4>limit){pos=start;break;}val=ld32(b->bytes+pos);pos+=4;memory_base=8;}
                    else if(mod==1){if(pos>=limit){pos=start;break;}val=(uint32_t)(int32_t)(int8_t)b->bytes[pos++];}
                    else if(mod==2){if(pos+4>limit){pos=start;break;}val=ld32(b->bytes+pos);pos+=4;}
                    kind=compare?XJ_MEMCMP:(op==0x89||op==0x88)?XJ_MEMSTORE:XJ_MEMLOAD;b->memory=1;
                    if(compare&&(op==0x81||op==0x83)){
                        imm=1;
                        if(op==0x81){if(pos+4>limit){pos=start;break;}memory_operand=ld32(b->bytes+pos);pos+=4;}
                        else {if(pos>=limit){pos=start;break;}memory_operand=(uint32_t)(int32_t)(int8_t)b->bytes[pos++];}
                    }
                }
            }
        }
        if(kind) { /* already decoded memory/extension operation */ }
        else
#endif
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
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_CARRY_CHAIN
            b->has_cf=1;
            if(last_cf>=0)ops[last_cf].capture_cf=1;else b->cf_input=1;
#else
            if(!carry_live){pos=start;break;}
            b->cf_input=1;
#endif
            if(kind==XJ_ADC || kind==XJ_SBB)carry_live=0;
        } else if(kind!=XJ_MOV && kind!=XJ_NOP && kind!=XJ_LEA && kind!=XJ_LOAD && kind!=XJ_MEMLOAD && kind!=XJ_MEMSTORE && kind!=XJ_ZEXT)carry_live=0;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_CARRY_CHAIN
        ops[n].capture_cf=0;
        if(kind!=XJ_INC && kind!=XJ_DEC && kind!=XJ_MOV && kind!=XJ_NOP && kind!=XJ_LEA && kind!=XJ_LOAD && kind!=XJ_MEMLOAD && kind!=XJ_MEMSTORE && kind!=XJ_ZEXT)last_cf=(int)n;
#endif
        ops[n].kind = kind; ops[n].dst = d; ops[n].src = s;
        ops[n].index=index;ops[n].scale=scale;
        ops[n].imm_src = imm; ops[n].imm = val; ops[n].eip = b->eip + start;
        if (kind != XJ_MOV && kind != XJ_NOP && kind != XJ_LEA && kind != XJ_LOAD && kind != XJ_MEMLOAD && kind != XJ_MEMSTORE && kind != XJ_ZEXT) *last_flags = (int)n;
#if QRX_JIT_NATIVE_MEMORY
        ops[n].base=memory_base;ops[n].width=memory_width;ops[n].sign=memory_sign;ops[n].memreg=op==0x8a;ops[n].end=b->eip+pos;ops[n].operand=memory_operand;
#endif
        b->last = b->eip + start; n++;
#if QRX_JIT_NATIVE_MEMORY
        /* A store may modify the following x86 instruction. Never execute a
         * cached successor before the ordinary entry freshness check. */
        if(kind==XJ_MEMSTORE && !QRX_JIT_WRITE_CONTINUE)break;
#endif
    }
    if (n < c->jit->min_ops) { b->len = limit < 16 ? limit : 16; b->count = 0; return 0; }
    /* A failed trailing decode must not consume its opcode or immediate. */
    b->len = pos; b->end = b->eip + pos; b->count = n;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_COMPACT_DEOPT
    b->boundaries[n]=pos;
#endif
    *nops = n - b->terminal;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_FLAG_LIVENESS
    if(b->memory){
        int live=1;
        for(unsigned i=*nops;i-->0;){
            unsigned k=ops[i].kind;
            if(k==XJ_MEMCMP){ops[i].keep_flags=1;live=1;continue;}
            if(k==XJ_MEMLOAD||k==XJ_MEMSTORE)live=1;
            ops[i].keep_flags=0;
            if((k>=XJ_ADD&&k<=XJ_TEST)||(k>=XJ_SHL&&k<=XJ_SAR)||(k>=XJ_INC&&k<=XJ_SBB)){
                ops[i].keep_flags=live;live=0;
            }
        }
    }
#endif
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

#if QRX_JIT_NATIVE_MEMORY
/* Common 68020+ integer instructions only. D2 is saved once by the enclosing
 * block. The current mapping is accessed only after its software-TLB tag hits. */
static void xj_memory_guard(xj_emit *e,const xj_op *o,unsigned *fix
#if QRX_JIT_PAGE_CACHE
    ,struct xj_page_cache *cache
#endif
)
{
    unsigned segment=(o->base==X86_ESP||o->base==X86_EBP)?X86_SS:X86_DS;
#if QRX_JIT_PAGE_CACHE
    unsigned slow_fix,common_fix;
#endif
    if(o->base<8)xj_load(e,2,o->base);else xj_imm(e,2,0);
    if(o->index<8){
        xj_load(e,1,o->index);
        if(o->scale)xj_w16(e,0xe189|(o->scale<<9)); /* lsl.l #scale,d1 */
        xj_w16(e,0xd481); /* add.l d1,d2 */
    }
    if(o->imm){xj_w16(e,0x0682);xj_w32(e,o->imm);}
    xj_w16(e,0xd4a8);xj_w16(e,XJ_OFF(sbase)+segment*4);
#if QRX_JIT_PAGE_CACHE
    xj_w16(e,0x2202);xj_w16(e,0x0281);xj_w32(e,~X86_PAGE_MASK);
    xj_w16(e,0x0081);xj_w32(e,1);
    xj_w16(e,0xb2b9);xj_w32(e,(uintptr_t)&cache->tag);
    xj_w16(e,0x6600);slow_fix=e->n;xj_w16(e,0);
    xj_w16(e,0x2279);xj_w32(e,(uintptr_t)&cache->page);
    xj_w16(e,0x6000);common_fix=e->n;xj_w16(e,0);
    {int delta=(int)e->n-(int)slow_fix;e->p[slow_fix]=delta>>8;e->p[slow_fix+1]=delta;}
#endif
    xj_w16(e,0x2002);xj_w16(e,0xe088);xj_w16(e,0xe888);
    xj_w16(e,0x0280);xj_w32(e,63);xj_w16(e,0xe588);
    xj_w16(e,0x2202);xj_w16(e,0x0281);xj_w32(e,~X86_PAGE_MASK);
    xj_w16(e,0x0081);xj_w32(e,1);
    xj_w16(e,0x43e8);xj_w16(e,o->kind==XJ_MEMSTORE?XJ_OFF(tlb_wtag):XJ_OFF(tlb_rtag));
    xj_w16(e,0xb2b1);xj_w16(e,0x0800); /* cmp.l (a1,d0.l),d1 */
    xj_w16(e,0x6600);fix[0]=e->n;xj_w16(e,0);
    fix[1]=0;
    xj_w16(e,0x43e8);xj_w16(e,o->kind==XJ_MEMSTORE?XJ_OFF(tlb_wptr):XJ_OFF(tlb_rptr));
    xj_w16(e,0x2271);xj_w16(e,0x0800); /* movea.l (a1,d0.l),a1 */
#if QRX_JIT_PAGE_CACHE
    xj_w16(e,0x23c1);xj_w32(e,(uintptr_t)&cache->tag);
    xj_w16(e,0x23c9);xj_w32(e,(uintptr_t)&cache->page);
    {int delta=(int)e->n-(int)common_fix;e->p[common_fix]=delta>>8;e->p[common_fix+1]=delta;}
#endif
    if(o->width>1){
        xj_w16(e,0x2202);xj_w16(e,0x0281);xj_w32(e,X86_PAGE_MASK);
        xj_w16(e,0x0c81);xj_w32(e,X86_PAGE_SIZE-o->width);
        xj_w16(e,0x6200);fix[1]=e->n;xj_w16(e,0);
    }
    xj_w16(e,0x0282);xj_w32(e,X86_PAGE_MASK);xj_w16(e,0xd3c2);
}
static unsigned xj_byte_offset(unsigned reg)
{ return (reg&3)*4+(reg<4?3:2); }
static void xj_extend_value(xj_emit *e,unsigned width,unsigned sign)
{
    if(width==2)xj_w16(e,0xe058); /* little-endian word */
    if(sign){if(width==1)xj_w16(e,0x4880);xj_w16(e,0x48c0);}
}
static void xj_patch_bail(xj_emit *e,unsigned fix,unsigned destination)
{
    int displacement=(int)destination-(int)fix;
    if(displacement < -32768 || displacement > 32767){e->bad=1;return;}
    e->p[fix]=(unsigned)displacement>>8;e->p[fix+1]=(unsigned)displacement;
}
#endif

static int xj_compile_body(x86cpu *c, xj_block *b)
{
    xj_op ops[XJ_MAX_OPS]; unsigned nops = 0, i; int final_flags, loaded = -1,dirty=0;
    uint8_t tmp[XJ_CODE_CAP]; xj_emit e = { tmp, 0, 0 };
    void *code;
#if QRX_JIT_NATIVE_MEMORY
    unsigned fixups[XJ_MAX_OPS][3]={{0}},cc_safe=1;
#if QRX_JIT_LOOP_BATCH
    unsigned loop_start=0;
#endif
#endif
    if (!xj_decode(c, b, ops, &nops, &final_flags)) return 0;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_MEMORY_CMP
    for(unsigned n=(unsigned)(final_flags+1);n<nops;n++)
        if(ops[n].kind==XJ_MEMLOAD||ops[n].kind==XJ_MEMSTORE||ops[n].kind==XJ_MEMCMP)cc_safe=0;
#endif
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_WRITE_CONTINUE
    {x86page *page=pg(c,b->eip);if(!page)return 0;
     b->host_code_start=(uintptr_t)(page->mem+(b->eip&X86_PAGE_MASK));}
#endif
    /* C ABI: argument at 4(sp), only caller-saved D0/D1/A0 are clobbered.
     * No host stack frame, helper call or cached SP. A leading load uses only
     * a same-page host pointer validated before any guest effect. A1 is caller-saved. */
#if QRX_JIT_NATIVE_MEMORY
    if(b->memory || (QRX_JIT_CARRY_CHAIN && b->has_cf))xj_w16(&e,0x2f02); /* preserve caller's D2 */
    xj_w16(&e,0x206f);xj_w16(&e,(b->memory || (QRX_JIT_CARRY_CHAIN && b->has_cf))?8:4);
#else
    xj_w16(&e, 0x206f); xj_w16(&e, 4); /* movea.l 4(sp),a0 */
#endif
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_LOOP_BATCH
    loop_start=e.n;
#endif
    for (i = 0; i < nops; i++) {
        xj_op *o = &ops[i]; unsigned k = o->kind;
        if (k == XJ_NOP) continue;
#if QRX_JIT_NATIVE_MEMORY
        if(k==XJ_ZEXT || k==XJ_MEMLOAD || k==XJ_MEMSTORE || k==XJ_MEMCMP){
            xj_flush_reg(&e,loaded,&dirty);loaded=-1;dirty=0;
            if(k==XJ_ZEXT){
                xj_w16(&e,0x7000);
                xj_w16(&e,o->width==1?0x1028:0x3028);
                xj_w16(&e,o->width==1?xj_byte_offset(o->src):o->src*4+2);
                if(o->sign){if(o->width==1)xj_w16(&e,0x4880);xj_w16(&e,0x48c0);}
                xj_store(&e,0,o->dst*4);
                continue;
            }
            xj_memory_guard(&e,o,fixups[i]
#if QRX_JIT_PAGE_CACHE
                ,&b->pages[i]
#endif
            );
            if(k==XJ_MEMCMP){
                xj_w16(&e,0x2011);xj_w16(&e,0xe058);xj_w16(&e,0x4840);xj_w16(&e,0xe058);
                if(o->imm_src)xj_imm(&e,1,o->operand);
                else if(o->sign){xj_w16(&e,0x2200);xj_load(&e,0,o->dst);}
                else xj_load(&e,1,o->dst);
                xj_store(&e,0,XJ_OFF(lf_a));xj_store(&e,1,XJ_OFF(lf_b));
                xj_w16(&e,0x9081); /* exact 32-bit comparison result */
                if((int)i==final_flags && (cc_safe || QRX_JIT_PERSIST_CC) && b->terminal && b->cc>=0 && b->cc!=10 && b->cc!=11){
                    static const unsigned cc[16]={9,8,5,4,7,6,3,2,11,10,0,0,13,12,15,14};
                    xj_w16(&e,0x50c1|(cc[b->cc]<<8));xj_w16(&e,0x2241);b->native_cc=1;
#if QRX_JIT_PERSIST_CC
                    xj_w16(&e,0x23c9);xj_w32(&e,(uintptr_t)&c->jit->branch_taken);
#endif
                }
#if QRX_JIT_CARRY_CHAIN
                if(o->capture_cf){xj_w16(&e,0x55c1);xj_w16(&e,0x0281);xj_w32(&e,1);xj_w16(&e,0x23c1);xj_w32(&e,(uintptr_t)&c->jit->entry_cf);}
#endif
                xj_store(&e,0,XJ_OFF(lf_res));xj_store_imm(&e,LF_SUB,XJ_OFF(lf_op));xj_store_imm(&e,32,XJ_OFF(lf_sz));
                continue;
            }
            if(k==XJ_MEMSTORE){
                if(o->width==1){xj_w16(&e,0x1028);xj_w16(&e,xj_byte_offset(o->dst));xj_w16(&e,0x1280);}
                else {xj_load(&e,0,o->dst);xj_w16(&e,0xe058);xj_w16(&e,0x4840);xj_w16(&e,0xe058);xj_w16(&e,0x2280);}
#if QRX_JIT_WRITE_CONTINUE
                /* Detect actual host aliases, not merely equal guest virtual
                 * pages. On overlap, leave after this store before executing
                 * any potentially stale decoded successor. */
                xj_w16(&e,0x2209); /* move.l a1,d1 */
                if(o->width>1)xj_w16(&e,0x5081|((o->width-1)<<9));
                xj_w16(&e,0x92b9);xj_w32(&e,(uintptr_t)&b->host_code_start);
                xj_w16(&e,0x0c81);xj_w32(&e,b->len+o->width-1);
                xj_w16(&e,0x6500);fixups[i][2]=e.n;xj_w16(&e,0);
#endif
            } else {
                if(o->width==4){xj_w16(&e,0x2011);xj_w16(&e,0xe058);xj_w16(&e,0x4840);xj_w16(&e,0xe058);}
                else {xj_w16(&e,0x7000);xj_w16(&e,o->width==1?0x1011:0x3011);xj_extend_value(&e,o->width,o->sign);}
                if(o->memreg){xj_w16(&e,0x1140);xj_w16(&e,xj_byte_offset(o->dst));}
                else xj_store(&e,0,o->dst*4);
            }
            continue;
        }
#endif
        if(QRX_JIT_DIRTY_REGS && (k==XJ_CMP||k==XJ_TEST) && (int)i!=final_flags
#if QRX_JIT_NATIVE_MEMORY
           && (!b->memory || (QRX_JIT_FLAG_LIVENESS && !o->keep_flags))
           && (!QRX_JIT_CARRY_CHAIN || !o->capture_cf)
#endif
           )continue;
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
#if QRX_JIT_NATIVE_MEMORY
        if((b->memory || QRX_JIT_CARRY_CHAIN) && (k==XJ_INC||k==XJ_DEC||k==XJ_ADC||k==XJ_SBB)){
            /* Do not overwrite incoming lazy carry before an earlier guard
             * can deoptimize; capture it separately at block entry. */
            xj_w16(&e,0x2401); /* preserve rhs in saved D2 */
            xj_w16(&e,0x2268);xj_w16(&e,XJ_OFF(jit));
            xj_w16(&e,0x2229);xj_w16(&e,offsetof(struct x86jit,entry_cf));
            xj_store(&e,1,XJ_OFF(lf_cf));
            xj_w16(&e,0x2202); /* restore rhs exactly, including a dirty alias */
        }
#endif
        if ((int)i == final_flags
#if QRX_JIT_NATIVE_MEMORY
            || (b->memory && (!QRX_JIT_FLAG_LIVENESS || o->keep_flags))
#endif
            ) {
            xj_store(&e, 0, XJ_OFF(lf_a)); xj_store(&e, 1, XJ_OFF(lf_b));
        }
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_CARRY_CHAIN
        if(o->capture_cf && (k==XJ_ADC||k==XJ_SBB)){
            xj_store(&e,0,XJ_OFF(lf_a));xj_store(&e,1,XJ_OFF(lf_b));
        }
#endif
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
        if((int)i==final_flags
#if QRX_JIT_NATIVE_MEMORY
           && (!b->memory || (QRX_JIT_MEMORY_CMP && (cc_safe || QRX_JIT_PERSIST_CC)))
#endif
           && b->terminal && b->cc>=0 && b->cc!=10 && b->cc!=11 &&
           (k==XJ_ADD || k==XJ_SUB || k==XJ_CMP || k==XJ_AND || k==XJ_OR || k==XJ_XOR || k==XJ_TEST)) {
            static const unsigned host_cc[16]={9,8,5,4,7,6,3,2,11,10,0,0,13,12,15,14};
            xj_w16(&e,0x50c1|(host_cc[b->cc]<<8)); /* Scc d1, low byte only */
            xj_w16(&e,0x2241); /* movea.l d1,a1; survives following register operations */
            b->native_cc=1;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_PERSIST_CC
            xj_w16(&e,0x23c9);xj_w32(&e,(uintptr_t)&c->jit->branch_taken);
#endif
        }
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_CARRY_CHAIN
        if(o->capture_cf){
            if(k==XJ_ADC){
                /* Carry = res<a, or carry-in when res==a. */
                xj_w16(&e,0xb0a8);xj_w16(&e,XJ_OFF(lf_a));
                xj_w16(&e,0x55c1);xj_w16(&e,0x6604);
                xj_w16(&e,0x2228);xj_w16(&e,XJ_OFF(lf_cf));
            }else if(k==XJ_SBB){
                /* Borrow = a<b, or carry-in when a==b. */
                xj_w16(&e,0x2228);xj_w16(&e,XJ_OFF(lf_a));
                xj_w16(&e,0xb2a8);xj_w16(&e,XJ_OFF(lf_b));
                xj_w16(&e,0x55c1);xj_w16(&e,0x6604);
                xj_w16(&e,0x2228);xj_w16(&e,XJ_OFF(lf_cf));
            }else xj_w16(&e,0x55c1); /* SCS preserves original arithmetic CCR */
            xj_w16(&e,0x0281);xj_w32(&e,1);
            xj_w16(&e,0x23c1);xj_w32(&e,(uintptr_t)&c->jit->entry_cf);
        }
#endif
        if ((int)i == final_flags
#if QRX_JIT_NATIVE_MEMORY
            || (b->memory && (!QRX_JIT_FLAG_LIVENESS || o->keep_flags))
#endif
            ) {
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
    if(b->native_cc){
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_PERSIST_CC
        /* Guards and memory operands freely reuse A1 after the producer. */
        xj_w16(&e,0x2039);xj_w32(&e,(uintptr_t)&c->jit->branch_taken);
#else
        xj_w16(&e,0x2009);
#endif
        xj_w16(&e,0x0280);xj_w32(&e,1);
    }
#if QRX_JIT_NATIVE_MEMORY
    else xj_w16(&e,0x7000); /* reserve high-bit returns for precise deoptimization */
#endif
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_LOOP_BATCH
    /* No C callbacks or mapping changes occur inside a native block. Every
     * store retains the host-alias SMC guard on every iteration. Restrict
     * batching to conditional self loops with no entry-CF dependency. */
    if(b->terminal && b->target==b->eip && b->native_cc && !b->cf_input && !b->load){
        unsigned exit_fix,back_fix;
        b->loop_batch=1;
        xj_w16(&e,0x52b9);xj_w32(&e,(uintptr_t)&c->jit->loop_done);
        xj_w16(&e,0x4a80); /* tst.l d0: branch result */
        xj_w16(&e,0x6700);exit_fix=e.n;xj_w16(&e,0);
        xj_w16(&e,0x2239);xj_w32(&e,(uintptr_t)&c->jit->loop_done);
        xj_w16(&e,0xb2b9);xj_w32(&e,(uintptr_t)&c->jit->loop_limit);
        xj_w16(&e,0x6500);back_fix=e.n;xj_w16(&e,0);
        xj_patch_bail(&e,back_fix,loop_start);xj_patch_bail(&e,exit_fix,e.n);
    }
#endif
#if QRX_JIT_NATIVE_MEMORY
    if(b->memory || (QRX_JIT_CARRY_CHAIN && b->has_cf))xj_w16(&e,0x241f);
#endif
    xj_w16(&e, 0x4e75);
#if QRX_JIT_NATIVE_MEMORY
    for(i=0;i<nops;i++)if(fixups[i][0]){
        unsigned target=e.n;
#if QRX_JIT_COMPACT_DEOPT
        if(i<128)xj_w16(&e,0x7000|((~i)&255));else xj_imm(&e,0,~i);
#else
        xj_store_imm(&e,ops[i].eip,XJ_OFF(eip));
        xj_store_imm(&e,i?ops[i-1].eip:ops[i].eip,XJ_OFF(cur_eip));
        xj_imm(&e,0,0x80000000u|i);
#endif
        xj_w16(&e,0x241f);xj_w16(&e,0x4e75);
        xj_patch_bail(&e,fixups[i][0],target);
        if(fixups[i][1])xj_patch_bail(&e,fixups[i][1],target);
#if QRX_JIT_WRITE_CONTINUE
        if(fixups[i][2]){
            target=e.n;
#if QRX_JIT_COMPACT_DEOPT
            if(i+1<128)xj_w16(&e,0x7000|((~(i+1))&255));else xj_imm(&e,0,~(i+1));
#else
            xj_store_imm(&e,ops[i].end,XJ_OFF(eip));
            xj_store_imm(&e,ops[i].eip,XJ_OFF(cur_eip));
            xj_imm(&e,0,0x80000000u|(i+1));
#endif
            xj_w16(&e,0x241f);xj_w16(&e,0x4e75);
            xj_patch_bail(&e,fixups[i][2],target);
        }
#endif
    }
#endif
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
#if QRX_JIT_NATIVE_MEMORY
    if(op==0x88 || op==0x8a || op==0x89 || op==0x8b)return 1;
#if QRX_JIT_MEMORY_CMP
    if(op==0x39||op==0x3b)return 1;
    if((op==0x81||op==0x83)&&((xj_peek(c,pc+1)>>3)&7)==7)return 1;
#endif
    if(op==0x0f){unsigned v=xj_peek(c,pc+1);if(v==0xb6||v==0xb7||v==0xbe||v==0xbf)return 1;}
#endif
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
X86_INLINE int xj_fresh(x86cpu *c, xj_block *b)
{
    x86page *p = pg(c,b->eip);
    unsigned off=b->eip & X86_PAGE_MASK, i=0;
    const uint8_t *live;
    if(!p || b->len > X86_PAGE_SIZE-off) return 0;
    live=p->mem+off;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_WRITE_CONTINUE
    b->host_code_start=(uintptr_t)live;
#endif
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
#if QRX_JIT_KEEP_POSITIVE
    /* Cold probes must not evict useful published code. The compact tag tracks
     * hotness independently; replace a positive entry only after a competing
     * translation has actually compiled and synchronized successfully. */
    if(b && b->eip==pc){
        if(xj_fresh(c,b)){
            if(j->stats.cache_profile)j->stats.hits++;
            goto xj_ready;
        }
        j->stats.invalidations++;j->stats.live_code_bytes-=b->code_size;
        xj_code_free(b->code,b->code_size);free(b);memset(slot,0,sizeof(*slot));
    }
    b=NULL;
    if(!slot->hits || slot->eip!=pc){
        if(slot->hits)j->stats.evictions++;
        slot->eip=pc;slot->hits=1;slot->retries=0;j->stats.misses++;
        return 0;
    }
    if(slot->hits>=j->threshold){
        if(++slot->retries)return 0;
        slot->hits=j->threshold-1;
    }
    if(++slot->hits<j->threshold)return 0;
    b=(xj_block *)calloc(1,sizeof(*b));
    if(!b)return 0;
    b->eip=pc;
    if(!xj_compile(c,b)){free(b);return 0;}
    if(slot->block){
        xj_block *old=slot->block;
        j->stats.live_code_bytes-=old->code_size;
        xj_code_free(old->code,old->code_size);free(old);
    }
    slot->block=b;
xj_ready:
#else
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
#endif
    if(!b || (charged ? b->count-1 > c->left : b->count > c->left) || ((uint32_t)(stop - b->eip)<b->len))return 0;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_PAGE_CACHE
    /* No mapping changes or guest callbacks occur inside generated code.
     * Retained host pointers are invalidated before the next native entry. */
    if(b->memory_serial!=c->memory_serial){
        memset(b->pages,0,sizeof b->pages);b->memory_serial=c->memory_serial;
    }
#endif
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
    if(b->cf_input){
#if QRX_JIT_NATIVE_MEMORY
        if(b->memory || QRX_JIT_CARRY_CHAIN)j->entry_cf=get_cf(c)?X86_CF:0;
        else
#endif
        c->lf_cf=get_cf(c)?X86_CF:0;
    }
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_LOOP_BATCH
    if(b->loop_batch){
        uint64_t available=(uint64_t)c->left+charged;
        uint64_t limit=available/b->count;
        j->loop_done=0;j->loop_limit=limit>256?256:(uint32_t)limit;
    }
#endif
    if (j->stats.timing) t = clock();
    #if QRX_JIT_NATIVE_LOADS
    taken=((unsigned (*)(x86cpu *,const uint8_t *))b->code)(c,load_ptr);
#else
    taken=((unsigned (*)(x86cpu *))b->code)(c);
#endif
#if QRX_JIT_NATIVE_MEMORY
    if(taken&0x80000000u){
#ifdef QRX_JIT_PROFILE_DEOPT
        j->stats.deopts++;
#endif
#if QRX_JIT_COMPACT_DEOPT
        unsigned completed=~taken;
        if(completed){c->eip=b->eip+b->boundaries[completed];c->cur_eip=b->eip+b->boundaries[completed-1];}
#else
        unsigned completed=taken&0x7fffffffu;
#endif
#if QRX_JIT_LOOP_BATCH
        if(b->loop_batch && j->loop_done){
            if(!completed){c->eip=b->eip;c->cur_eip=b->last;}
            completed+=j->loop_done*b->count;
        }
#endif
        if(j->stats.timing)j->stats.native_ticks+=xj_elapsed(&j->stats,t);
        if(!completed){
#ifdef QRX_JIT_PROFILE_DEOPT
            j->stats.zero_deopts++;
#endif
            c->eip=pc+charged;c->cur_eip=pc;return 0;
        }
        c->left-=completed-charged;
        j->stats.native_blocks++;j->stats.native_instructions+=completed;
        return 1;
    }
#endif
    unsigned executed=b->count;
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_LOOP_BATCH
    if(b->loop_batch)executed*=j->loop_done;
#endif
    c->left -= executed-charged; c->cur_eip = b->last;
    c->eip = b->terminal && (b->cc < 0 || (b->native_cc ? taken : cond(c, b->cc))) ? b->target : b->end;
    if (j->stats.timing) j->stats.native_ticks += xj_elapsed(&j->stats,t);
    j->stats.native_blocks++; j->stats.native_instructions += executed;
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
    j->stats.features=(QRX_JIT_NATIVE_MEMORY?1u:0u)|(QRX_JIT_WRITE_CONTINUE?2u:0u)|(QRX_JIT_FLAG_LIVENESS?4u:0u)|(QRX_JIT_KEEP_POSITIVE?8u:0u)|(QRX_JIT_COMPACT_DEOPT?16u:0u)|(QRX_JIT_MEMORY_CMP?32u:0u)|(QRX_JIT_PERSIST_CC?512u:0u)|(QRX_JIT_CARRY_CHAIN?256u:0u)|(QRX_JIT_LOOP_BATCH?1024u:0u)|(QRX_JIT_PAGE_CACHE?2048u:0u)|(QRX_JIT_NATIVE_LOADS?64u:0u)|(j->allow_loads?128u:0u);
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
