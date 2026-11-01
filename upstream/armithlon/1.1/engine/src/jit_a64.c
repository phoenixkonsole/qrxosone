#include "qrx68100.h"
#include "qrxvirt.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

typedef struct { uint32_t *words; size_t len; size_t max; } Emit;
static bool emit(Emit *e, uint32_t instruction) {
    if (e->len >= e->max) return false;
    e->words[e->len++] = instruction;
    return true;
}
/* A64 encodings in this SMALL proof-of-concept are fixed, with x0 = QRXCPU*. */
static bool movz(Emit *e, unsigned rd, unsigned imm16) {
    return emit(e, 0x52800000u | ((imm16 & 0xffffu) << 5) | rd);
}
static bool movn(Emit *e, unsigned rd, unsigned imm16) {
    return emit(e, 0x12800000u | ((imm16 & 0xffffu) << 5) | rd);
}
static bool ldrh(Emit *e, unsigned rt, size_t offset) {
    if ((offset & 1u) || offset / 2u > 4095u) return false;
    return emit(e, 0x79400000u | ((uint32_t)(offset/2u) << 10) | rt);
}
static bool strh(Emit *e, unsigned rt, size_t offset) {
    if ((offset & 1u) || offset / 2u > 4095u) return false;
    return emit(e, 0x79000000u | ((uint32_t)(offset/2u) << 10) | rt);
}
static bool strw(Emit *e, unsigned rt, size_t offset) {
    if ((offset & 3u) || offset / 4u > 4095u) return false;
    return emit(e, 0xB9000000u | ((uint32_t)(offset/4u) << 10) | rt);
}
static bool strb(Emit *e, unsigned rt, size_t offset) {
    if (offset > 4095u) return false;
    return emit(e, 0x39000000u | ((uint32_t)offset << 10) | rt);
}
static bool jit_moveq(Emit *e, uint16_t op) {
    const unsigned reg = (op >> 9) & 7u;
    const int32_t value = (int8_t)(op & 0xffu);
    const unsigned nz = (value == 0 ? 4u : 0u) | (value < 0 ? 8u : 0u);
    /* W9 holds sign-extended MOVEQ immediate. */
    if (!(value < 0 ? movn(e, 9, (unsigned)(~(uint32_t)value) & 0xffffu)
                    : movz(e, 9, (unsigned)value))) return false;
    if (!strw(e, 9, offsetof(QRXCPU, d) + reg*sizeof(uint32_t))) return false;
    /* MOVEQ updates N/Z, clears V/C, preserves X, no host helper call. */
    return ldrh(e, 10, offsetof(QRXCPU, sr)) &&
           movz(e, 11, 0xfff0u) &&
           emit(e, 0x0A0B014Au) && /* AND W10,W10,W11 */
           movz(e, 11, nz) &&
           emit(e, 0x2A0B014Au) && /* ORR W10,W10,W11 */
           strh(e, 10, offsetof(QRXCPU, sr));
}

bool qrx_try_jit_microblock(QRXCPU *c, const QRXRAM *ram) {
    if (!c || !ram || !ram->data || c->halted) return false;
    if (c->pc >= ram->size || (c->pc & 1u)) return false;
    enum { MAX_INSNS = 64, MAX_WORDS = 512 };
    uint16_t ops[MAX_INSNS];
    unsigned nops = 0;
    uint32_t pc = c->pc;
    bool terminates = false;
    for (; nops < MAX_INSNS && pc <= ram->size && ram->size - pc >= 2; ++nops, pc += 2) {
        uint16_t op = (uint16_t)((uint16_t)ram->data[pc] << 8 | ram->data[pc + 1]);
        if ((op & 0xf100u) != 0x7000u && op != 0x4e71u && op != 0x4e4fu) return false;
        ops[nops] = op;
        if (op == 0x4e4fu) { ++nops; pc += 2; terminates = true; break; }
    }
    if (!terminates) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const size_t cap = (size_t)page_size;
    int flags = MAP_PRIVATE;
#if defined(MAP_ANON)
    flags |= MAP_ANON;
#else
    flags |= MAP_ANONYMOUS;
#endif
#if defined(__APPLE__)
    flags |= MAP_JIT;
    void *memory = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
#else
    void *memory = mmap(NULL, cap, PROT_READ | PROT_WRITE, flags, -1, 0);
#endif
    if (memory == MAP_FAILED) return false;
    Emit e = { (uint32_t *)memory, 0, cap / sizeof(uint32_t) };
    bool ok = true;
#if defined(__APPLE__)
    pthread_jit_write_protect_np(0);
#endif
    for (unsigned i = 0; i < nops && ok; ++i) {
        const uint16_t op = ops[i];
        if ((op & 0xf100u) == 0x7000u) ok = jit_moveq(&e, op);
        else if (op == 0x4e71u) ok = emit(&e, 0xd503201fu); /* A64 NOP */
    }
    /* Write final PC and test-halt (TRAP #15 at end of this microblock). */
    ok = ok && movz(&e, 9, pc & 0xffffu);
    if (pc >> 16) ok = ok && emit(&e, 0x72A00009u | ((pc >> 16) << 5)); /* MOVK W9, high16 */
    ok = ok && strw(&e, 9, offsetof(QRXCPU, pc));
    ok = ok && movz(&e, 9, 1) && strb(&e, 9, offsetof(QRXCPU, halted));
    ok = ok && emit(&e, 0xd65f03c0u); /* RET */
    if (ok) {
#if defined(__APPLE__)
        sys_icache_invalidate(memory, e.len * sizeof(uint32_t));
        pthread_jit_write_protect_np(1);
#else
        __builtin___clear_cache((char *)memory, (char *)memory + e.len*sizeof(uint32_t));
        if (mprotect(memory, cap, PROT_READ | PROT_EXEC) != 0) ok = false;
#endif
    } else {
#if defined(__APPLE__)
        pthread_jit_write_protect_np(1);
#endif
    }
    if (ok) {
        /* ISO C does not define data-to-function casts; this platform-level JIT
           relies on the AAPCS64 ABI and is tested separately on real AArch64. */
        void (*code)(QRXCPU *) = NULL;
        memcpy(&code, &memory, sizeof(code));
        code(c);
    }
    munmap(memory, cap);
    return ok;
}
/* Conservative one-entry compiled cache. Compiles MOVEQ/NOP straight-line
   sequences only; other opcodes run in the interpreter. JIT never touches guest
   RAM directly. Any RAM write invalidates cache by write epoch. */
void qrx_jit_release(QRXVM *v) {
    if (!v) return;
    if (v->jit_code && v->jit_size) munmap(v->jit_code,v->jit_size);
    v->jit_code=NULL;
    v->jit_size=0;
    v->jit_count=0;
}
bool qrx_jit_vm_block(QRXVM *v, uint64_t remaining_steps) {
    if (!v || !v->jit_enabled || !v->ram.data || v->cpu.halted || !remaining_steps)
        return false;
    if ((v->cpu.pc&1u) || v->cpu.pc>=v->ram.size) return false;
    /* No CPU instruction bytes are executable in the virtual MMIO hole. */
    if (v->cpu.pc>=QRX_IO_BASE && v->cpu.pc<QRX_IO_BASE+QRX_IO_LENGTH) return false;
    if (v->jit_code && v->jit_count && v->jit_pc==v->cpu.pc &&
        v->jit_epoch==v->ram_write_epoch && v->jit_count<=remaining_steps) {
        void (*code)(QRXCPU *)=NULL;
        memcpy(&code,&v->jit_code,sizeof(code));
        code(&v->cpu);
        v->steps+=v->jit_count;
        v->jit_executed++;
        return true;
    }
    enum { MAX_INSTR=32 };
    uint16_t ops[MAX_INSTR];
    uint32_t pc=v->cpu.pc;
    unsigned n=0;
    for (; n<MAX_INSTR && n<remaining_steps; ++n) {
        if ((uint64_t)pc+2>v->ram.size ||
            (pc>=QRX_IO_BASE && pc<QRX_IO_BASE+QRX_IO_LENGTH)) break;
        uint16_t op=(uint16_t)(((uint16_t)v->ram.data[pc]<<8)|v->ram.data[pc+1]);
        if ((op&0xf100u)!=0x7000u && op!=0x4e71u) break;
        ops[n]=op;
        pc+=2;
    }
    if (!n) return false;
    if (!v->jit_code) {
        long page=sysconf(_SC_PAGESIZE);
        if (page<=0) return false;
        size_t cap=(size_t)page;
        int flags=MAP_PRIVATE;
    #if defined(MAP_ANON)
        flags|=MAP_ANON;
    #else
        flags|=MAP_ANONYMOUS;
    #endif
    #if defined(__APPLE__)
        flags|=MAP_JIT;
        void *mem=mmap(NULL,cap,PROT_READ|PROT_WRITE|PROT_EXEC,flags,-1,0);
    #else
        void *mem=mmap(NULL,cap,PROT_READ|PROT_WRITE,flags,-1,0);
    #endif
        if (mem==MAP_FAILED) return false;
        v->jit_code=mem;
        v->jit_size=cap;
    } else {
    #if !defined(__APPLE__)
        if (mprotect(v->jit_code,v->jit_size,PROT_READ|PROT_WRITE)!=0) return false;
    #endif
    }
    Emit e={(uint32_t *)v->jit_code,0,v->jit_size/sizeof(uint32_t)};
    bool ok=true;
    #if defined(__APPLE__)
        pthread_jit_write_protect_np(0);
    #endif
    for (unsigned i=0;i<n && ok;i++) {
        if ((ops[i]&0xf100u)==0x7000u) ok=jit_moveq(&e,ops[i]);
        else ok=emit(&e,0xd503201fu); /* NOP */
    }
    ok=ok && movz(&e,9,pc&0xffffu);
    if (pc>>16) ok=ok && emit(&e,0x72A00009u|((pc>>16)<<5));
    ok=ok && strw(&e,9,offsetof(QRXCPU,pc)) && emit(&e,0xd65f03c0u);
    if (ok) {
    #if defined(__APPLE__)
        sys_icache_invalidate(v->jit_code,e.len*sizeof(uint32_t));
    #else
        __builtin___clear_cache((char *)v->jit_code,(char *)v->jit_code+e.len*sizeof(uint32_t));
        if (mprotect(v->jit_code,v->jit_size,PROT_READ|PROT_EXEC)!=0) ok=false;
    #endif
    }
    #if defined(__APPLE__)
        pthread_jit_write_protect_np(1);
    #endif
    if (!ok) {qrx_jit_release(v);return false;}
    v->jit_count=n;
    v->jit_epoch=v->ram_write_epoch;
    v->jit_pc=v->cpu.pc;
    v->jit_next_pc=pc;
    v->jit_compiled++;
    void (*code)(QRXCPU *)=NULL;
    memcpy(&code,&v->jit_code,sizeof(code));
    code(&v->cpu);
    v->steps+=n;
    v->jit_executed++;
    return true;
}

#else
bool qrx_try_jit_microblock(QRXCPU *c, const QRXRAM *ram) {
    (void)c; (void)ram;
    return false;
}
void qrx_jit_release(QRXVM *v) {(void)v;}
bool qrx_jit_vm_block(QRXVM *v, uint64_t remaining_steps) {
    (void)v; (void)remaining_steps;
    return false;
}
#endif

