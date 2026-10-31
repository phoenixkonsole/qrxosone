/* QRX OS One 22.5.12 - qrx86core: portable IA-32 user-mode interpreter.
 *
 * Runs 32-bit x86 user code on any host (developed for AROS m68k, big-endian;
 * tested on x86_64 Linux against native 32-bit execution).  Guest memory is
 * little-endian byte storage behind a two-level page table, so the host byte
 * order never matters.  The core knows nothing about operating systems; the
 * personalities (Linux i386 ELF, Win32 PE HLE) plug in through two hooks:
 *   - int_hook : INT n (Linux uses INT 0x80)
 *   - hle_hook : execution reached an address inside the HLE thunk window
 *                (imports bound to host-implemented API functions)
 */
#ifndef QRX86CORE_H
#define QRX86CORE_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#ifndef QRX_JIT_BACKEDGE_DISCOVERY
#define QRX_JIT_BACKEDGE_DISCOVERY 0
#endif

#ifndef QRX_JIT_NATIVE_IMUL
#define QRX_JIT_NATIVE_IMUL 0
#endif

#ifndef QRX_JIT_PAGE_CACHE
#define QRX_JIT_PAGE_CACHE 0
#endif

#define X86_PAGE_SHIFT 12
#define X86_PAGE_SIZE  4096u
#define X86_PAGE_MASK  (X86_PAGE_SIZE - 1u)

enum { X86_EAX, X86_ECX, X86_EDX, X86_EBX, X86_ESP, X86_EBP, X86_ESI, X86_EDI };
enum { X86_ES, X86_CS, X86_SS, X86_DS, X86_FS, X86_GS };

#define X86_CF 0x0001u
#define X86_PF 0x0004u
#define X86_AF 0x0010u
#define X86_ZF 0x0040u
#define X86_SF 0x0080u
#define X86_TF 0x0100u
#define X86_IF 0x0200u
#define X86_DF 0x0400u
#define X86_OF 0x0800u
#define X86_ARITH (X86_CF | X86_PF | X86_AF | X86_ZF | X86_SF | X86_OF)

enum x86_fault {
    X86_OK = 0,
    X86_FAULT_PAGE,        /* access to unmapped / read-only page */
    X86_FAULT_UD,          /* undefined or unsupported instruction */
    X86_FAULT_DE,          /* divide error */
    X86_FAULT_GP,          /* general protection (privileged instruction) */
    X86_FAULT_BUDGET,      /* instruction budget exhausted (watchdog) */
    X86_FAULT_HOST,        /* a hook requested termination with an error */
    X86_FAULT_INT3
};

typedef struct x86cpu x86cpu;
struct x86jit;
typedef struct x86jit_stats {
    uint64_t lookups, hits, misses, invalidations, evictions, compilations;
    uint64_t native_blocks, native_instructions, fallback_instructions, code_bytes;
    uint64_t live_code_bytes, peak_code_bytes;
    uint64_t compile_ticks, native_ticks, interpreter_ticks,clock_invalid;
    uint32_t profile,features,max_ops,code_cap;
    int timing,cache_profile;
#ifdef QRX_JIT_PROFILE_DEOPT
    uint32_t deopts,zero_deopts;
#endif
} x86jit_stats;
/* Opt-in experimental backend. OFF is the default. Profiles: 20/30/40/60,
 * or 0 for AROS hardware detection. Returns 0 if unavailable; never executes
 * generated code on a non-m68k host. Timing is optional (clock() overhead).
 * High-frequency hit/lookup counters require QRX86_JIT_CACHE_PROFILE; zero with
 * cache_profile==0 means unmeasured. Native/fallback instruction counts remain exact. */
int x86_jit_enable(x86cpu *c, unsigned profile, int timing);
void x86_jit_disable(x86cpu *c);
void x86_jit_snapshot(const x86cpu *c, x86jit_stats *out);
typedef int (*x86_int_fn)(x86cpu *c, int vector);      /* return 0 to fault */
typedef int (*x86_hle_fn)(x86cpu *c, uint32_t index);  /* return 0 to fault */

typedef struct x86page { uint8_t *mem; uint32_t flags; } x86page;
#define X86_PG_W 1u
#define X86_PG_EXT 2u      /* mem belongs to the host (frame buffer): never freed */

struct x86cpu {
    uint32_t r[8];
    uint32_t eip;
    uint32_t eflags;          /* authoritative except for X86_ARITH while lf_op != 0 */
    /* lazy arithmetic flags */
    int lf_op, lf_sz;
    uint32_t lf_res, lf_a, lf_b, lf_cf;
    /* segments: only FS/GS have meaningful bases in flat user mode */
    uint16_t sreg[6];
    uint32_t sbase[6];
    struct { uint32_t base, limit; int used; } tls[3];   /* set_thread_area GDT slots 6..8 */
    /* x87 */
    double st[8];
    int top;
    uint16_t fsw, fcw, ftw;
    /* memory */
    x86page *dir[1024];
    /* direct-mapped translation cache (tag = page address | 1); flushed on every
     * map/unmap/protect so it never outlives a page */
#if QRX_JIT_PAGE_CACHE
    uint64_t memory_serial; /* All mapping/permission/TLB invalidations. */
#endif
    uint32_t tlb_rtag[64], tlb_wtag[64];
    uint8_t *tlb_rptr[64], *tlb_wptr[64];
    /* instruction fetch window: host = f_base + (eip - f_lo) while eip - f_lo < f_lim
     * (one code page; f_lim = 0 after a flush) */
    uint32_t f_lo, f_lim;
    uint8_t *f_base;
    uint32_t left;                       /* instructions until the budget fault (32-bit: cheap on m68k) */
    uint32_t lbase;                      /* value of left when icount was last brought up to date */
    /* control */
    uint64_t icount, budget;            /* budget 0 = unlimited; read the count with x86_icount() */
    uint32_t hle_base, hle_count;
    x86_int_fn int_hook;
    x86_hle_fn hle_hook;
    void *user;
    int stop;                            /* set by hooks to leave run() normally */
    int fault;
    uint32_t fault_eip, fault_addr;
    char fault_msg[96];
    int depth;                           /* nested guest calls */
    uint32_t cur_eip;                    /* eip of the instruction being executed */
    jmp_buf *jb;
    struct x86jit *jit;                 /* owned; NEVER share in a CPU clone */
#ifdef QRX_EXEC_PROFILE
    /* Diagnostic build only: dispatcher counts; native block interiors excluded. */
    uint32_t prof_ops[512], prof_pc[4096], prof_pc_hits[4096];
    uint32_t prof_sample_left, prof_pc_collisions;
    uint32_t prof_active_op,prof_operands[512][16];
#endif
};

/* --- setup / memory --- */
void     x86_init(x86cpu *c);
void     x86_free(x86cpu *c);
int      x86_map(x86cpu *c, uint32_t addr, uint32_t size, int writable);   /* zero-filled */
int      x86_is_mapped(x86cpu *c, uint32_t addr);
int      x86_protect(x86cpu *c, uint32_t addr, uint32_t size, int writable);
void     x86_unmap(x86cpu *c, uint32_t addr, uint32_t size);
/* map [addr, addr+size) (page aligned) onto host memory at host (e.g. a display
 * frame buffer); replaces whatever was mapped there; the memory is not freed */
int      x86_map_ext(x86cpu *c, uint32_t addr, uint32_t size, uint8_t *host, int writable);
uint8_t *x86_host_ptr(x86cpu *c, uint32_t addr);       /* NULL if unmapped */
int      x86_write_mem(x86cpu *c, uint32_t addr, const void *src, uint32_t n);
int      x86_read_mem(x86cpu *c, uint32_t addr, void *dst, uint32_t n);
uint32_t x86_rd32(x86cpu *c, uint32_t a);              /* no fault: 0 if unmapped */
uint16_t x86_rd16(x86cpu *c, uint32_t a);
uint8_t  x86_rd8(x86cpu *c, uint32_t a);
int      x86_wr32(x86cpu *c, uint32_t a, uint32_t v);
int      x86_wr16(x86cpu *c, uint32_t a, uint16_t v);
int      x86_wr8(x86cpu *c, uint32_t a, uint8_t v);
int      x86_push(x86cpu *c, uint32_t v);
uint32_t x86_pop(x86cpu *c);

/* --- flags --- */
uint32_t x86_get_flags(x86cpu *c);
void     x86_set_flags(x86cpu *c, uint32_t f);

/* --- execution --- */
/* Run until a hook sets c->stop, a fault happens or eip == stop_eip. Returns c->fault. */
int      x86_run(x86cpu *c, uint32_t stop_eip);
int      x86_step(x86cpu *c);
/* Call guest function addr(args...) from inside a hook.  ESP is restored
 * afterwards, so both cdecl and stdcall callees work.  Returns EAX; *ok = 0 on fault. */
uint32_t x86_call(x86cpu *c, uint32_t addr, int nargs, const uint32_t *args, int *ok);
const char *x86_fault_name(int f);
/* x87 access for HLE functions returning/consuming doubles (ST(0)) */
void     x86_fpu_push(x86cpu *c, double v);
double   x86_fpu_pop(x86cpu *c);
/* Disassemble-free description of where we stopped. */
void     x86_describe_fault(x86cpu *c, char *buf, size_t n);

/* double -> 64-bit integer (truncating) without libgcc __fixdfdi/__fixunsdfdi:
 * the m68k libgcc versions use PC-relative calls that elf2hunk cannot relocate
 * in code hunks larger than 64 KiB.  Caller guarantees |v| < 2^64 (2^63 signed). */
static inline uint64_t x86_d2u64(double v)
{
    double hi = (double)(uint32_t)(v / 4294967296.0);
    return ((uint64_t)(uint32_t)hi << 32) | (uint32_t)(v - hi * 4294967296.0);
}
static inline int64_t x86_d2i64(double v)
{
    return v < 0 ? -(int64_t)x86_d2u64(-v) : (int64_t)x86_d2u64(v);
}

#define X86_CALL_SENTINEL 0xFFFFFFF0u   /* return address used by x86_call */

/* executed instructions: the run loop only decrements left, icount is caught up lazily */
static inline uint64_t x86_icount(const x86cpu *c) { return c->icount + (uint32_t)(c->lbase - c->left); }

#endif
