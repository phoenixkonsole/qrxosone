/* Differential test of the QRX86 host JIT against the interpreter it is built on:
 * random instruction sequences run through both and must end with the same registers,
 * flags, x87 state and memory. Includes the door to reach its internals. */
#undef NDEBUG
#include "../full68k/qrx86_door.c"
#include <assert.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#ifndef QRX86_HOST_JIT_ACTIVE
int main(void) { puts("skipped: not an AArch64 host"); return 0; }
#else
/* The door's host API and network services are not part of this test. */
const QRXHostApi *qrx_hostapi_find(const char *name) { (void)name; return NULL; }
void qrx_hostapi_context(QRXFullCPU *c, x86cpu *h, uint32_t a) { (void)c; (void)h; (void)a; }
void qrx_hostapi_alloc_result(uint32_t a, uint32_t b) { (void)a; (void)b; }
uint32_t qrx_hostapi_alloc_request;
const QRXHostApi *qrx_d3d_thunks[QW_THUNKS];
void qrx_hostnet_call(uint32_t op, uint32_t *a, uint8_t *ram) { (void)op; (void)a; (void)ram; }
void qrx_host_midi_short(uint32_t m) { (void)m; }
void qrx_host_midi_sysex(const uint8_t *d, uint32_t n) { (void)d; (void)n; }
void qrx_host_midi_reset(void) {}
void qrx_host_set_pointer(uint32_t i) { (void)i; }

enum { CODE = 0x400000u, END = 0x401F00u, DATA = 0x500000u, STACK = 0x600000u };
#define DATA_BASE (DATA + 0xFC0u)   /* 64 bytes below a page end: some accesses cross it */

static uint64_t rng = 0x123456789ABCDEFull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t pick(uint32_t n) { return rnd() % n; }

static uint8_t code[0x1F00];
static unsigned cp;
static void c8(uint32_t v) { code[cp++] = (uint8_t)v; }
static void c32(uint32_t v) { c8(v); c8(v >> 8); c8(v >> 16); c8(v >> 24); }
/* EBX points at data and ESP at the stack: neither may be written. */
static unsigned r32(void) { static const uint8_t r[6] = { 0, 1, 2, 5, 6, 7 }; return r[pick(6)]; }
static unsigned r8(void) { static const uint8_t r[6] = { 0, 1, 2, 4, 5, 6 }; return r[pick(6)]; }
static unsigned rsz(unsigned sz) { return sz == 8 ? r8() : r32(); }
/* mod/rm: a register, or memory at EBX + small displacement, some with an index. */
static void modrm(unsigned reg, unsigned sz, bool allow_mem) {
    if (allow_mem && pick(2)) {
        unsigned d = pick(100);
        switch (pick(4)) {
        case 0: c8(0x43 | reg << 3); c8(d); break;                      /* [ebx+d8] */
        case 1: c8(0x83 | reg << 3); c32(d); break;                     /* [ebx+d32] */
        case 2: c8(0x44 | reg << 3); c8(0x23 | (pick(4) << 6) | (4u << 3)); c8(d); break; /* no index, any scale */
        default: if (d < 4) { c8(0x03 | reg << 3); } else { c8(0x43 | reg << 3); c8(d); } break;
        }
    } else c8(0xC0 | reg << 3 | rsz(sz));
}
static unsigned osz(void) {
    switch (pick(5)) { case 0: return 8; case 1: c8(0x66); return 16; default: return 32; }
}
static void imm_sz(unsigned sz) { if (sz == 8) c8(rnd()); else if (sz == 16) { c8(rnd()); c8(rnd()); } else c32(pick(4) ? rnd() : pick(8)); }

static void gen_one(void);
static void gen_flagged(void) {
    /* Something that sets flags, then something that reads them. */
    unsigned sz;
    switch (pick(4)) {
    case 0: sz = osz(); c8((pick(8) << 3) | (sz == 8 ? 0 : 1)); modrm(rsz(sz), sz, true); break;
    case 1: sz = osz(); c8(0x84 | (sz != 8)); modrm(rsz(sz), sz, true); break;
    case 2: c8(0x40 | pick(2) << 3 | r32()); break;
    default: sz = osz(); c8(sz == 8 ? 0x80 : 0x83); modrm(pick(8), sz, true); c8(rnd()); break;
    }
    if (pick(3) == 0) gen_one();
    switch (pick(4)) {
    case 0: {                                                           /* jcc over one instruction */
        unsigned at;
        c8(0x70 | pick(16)); at = cp; c8(0);
        c8(0xB8 | r32()); c32(rnd());
        code[at] = (uint8_t)(cp - at - 1);
        break;
    }
    case 1: c8(0x0F); c8(0x90 | pick(16)); modrm(0, 8, true); break;    /* setcc */
    case 2: c8(0x0F); c8(0x40 | pick(16)); modrm(r32(), 32, true); break;   /* cmov */
    default: {                                                          /* jcc near */
        unsigned at;
        c8(0x0F); c8(0x80 | pick(16)); at = cp; c32(0);
        c8(0x90);
        code[at] = (uint8_t)(cp - at - 4);
        break;
    }
    }
}
static void gen_one(void) {
    unsigned sz;
    switch (pick(24)) {
    case 0: case 1: sz = osz(); c8((pick(8) << 3) | (pick(2) << 1) | (sz != 8)); modrm(rsz(sz), sz, true); break;
    case 2: sz = osz(); c8(sz == 8 ? 0x80 : 0x81); modrm(pick(8), sz, true); imm_sz(sz); break;
    case 3: sz = osz(); c8(0x88 | (pick(2) << 1) | (sz != 8)); modrm(rsz(sz), sz, true); break;
    case 4: sz = osz(); c8(0xC6 | (sz != 8)); modrm(0, sz, true); imm_sz(sz); break;
    case 5: c8(0xB8 | r32()); c32(rnd()); break;
    case 6: c8(0xB0 | r8()); c8(rnd()); break;
    case 7: c8(0x0F); c8(0xB6 | (pick(2) << 3) | pick(2)); modrm(r32(), 8, true); break;
    case 8: c8(0x8D); c8(0x44 | r32() << 3); c8(0x03 | (pick(4) << 6) | (r32() << 3)); c8(rnd()); break;      /* lea */
    case 9: sz = osz(); c8(0xF6 | (sz != 8)); { unsigned k = 2 + pick(2); modrm(k, sz, true); } break;       /* not neg */
    case 10: sz = osz(); c8(0xFE | (sz != 8)); modrm(pick(2), sz, true); break;                             /* inc dec */
    case 11: sz = osz(); { static const uint8_t s[5] = { 0, 1, 4, 5, 7 }; unsigned k = s[pick(5)];
             switch (pick(3)) { case 0: c8(0xC0 | (sz != 8)); modrm(k, sz, true); c8(pick(40)); break;
             case 1: c8(0xD0 | (sz != 8)); modrm(k, sz, true); break;
             default: c8(0xD2 | (sz != 8)); modrm(k, sz, true); break; } } break;
    case 12: c8(0x0F); c8(0xAF); modrm(r32(), 32, true); break;
    case 13: c8(pick(2) ? 0x69 : 0x6B); { unsigned at = cp - 1; modrm(r32(), 32, true); if (code[at] == 0x69) c32(rnd()); else c8(rnd()); } break;
    case 14: c8(0x50 | r32()); c8(0x58 | r32()); break;
    case 15: c8(0x68); c32(rnd()); c8(0x58 | r32()); break;
    case 16: c8(pick(2) ? 0x99 : 0x98); break;
    case 17: sz = osz(); c8(0x86 | (sz != 8)); modrm(rsz(sz), sz, true); break;
    case 18: c8(0xE8); c32(0); c8(0x58 | r32()); break;                  /* call next; pop */
    case 19: { unsigned at; c8(0xEB); at = cp; c8(0); c8(0x90); code[at] = (uint8_t)(cp - at - 1); break; }
    case 20: sz = osz(); c8(0xA8 | (sz != 8)); imm_sz(sz); break;
    case 21: { unsigned d = 128u + 8u * pick(8);                         /* x87 on its own cells */
        unsigned f = pick(5);
        unsigned m = pick(2) ? 0xD9u : 0xDDu;
        c8(m); c8(0x83); c32(m == 0xD9 ? d + 64 : d);                    /* fld */
        if (f == 0) { c8(0xD8 | (pick(2) ? 0 : 4)); c8(0x83 | ((unsigned[]){0,1,4,5,6,7}[pick(6)]) << 3); c32(0); code[cp-4] = (uint8_t)(128 + 8 * pick(8)); code[cp-6] = code[cp-6]; if (code[cp-6] == 0xD8) code[cp-4] = (uint8_t)(192 + 8 * pick(8)); }
        else if (f == 1) { c8(0xD8); c8(0xC0 | ((unsigned[]){0,1,4,5,6,7}[pick(6)]) << 3 | 0); }
        else if (f == 2) { c8(0xDC); c8(0xC0 | ((unsigned[]){0,1,4,5,6,7}[pick(6)]) << 3 | 0); }
        else if (f == 3) { c8(m); c8(0xC0); }
        c8(pick(2) ? 0xD9 : 0xDD); { unsigned at = cp - 1; c8(0x9B | 0); c32(0); code[cp - 5] = 0x9B; code[cp - 4] = (uint8_t)(code[at] == 0xD9 ? 192 + 8 * pick(8) : 128 + 8 * pick(8)); }
        break; }
    case 22:
        if (pick(2)) { c8(0xF7); modrm(4 + pick(2), 32, false); break; } /* mul, imul */
        /* A short REP string instruction, with the registers put back for what follows */
        c8(0x60);                                                        /* pusha */
        c8(0x8D); c8(0x73); c8(pick(32));                                /* lea esi,[ebx+d] */
        c8(0x8D); c8(0x7B); c8(32 + pick(32));                           /* lea edi,[ebx+d] */
        c8(0xB9); c32(pick(8));                                          /* mov ecx,n */
        c8(0xF3); c8(pick(2) ? 0xA5 : 0xAB);                             /* rep movsd or stosd */
        c8(0x61);                                                        /* popa */
        break;
    default: gen_flagged(); break;
    }
}
static const double fp_vals[8] = { 1.5, -2.25, 1024.0, 0.125, 3.0, -7.0, 65536.0, 0.5 };

static void setup(x86cpu *c) {
    x86_map(c, CODE, 0x2000, 1);
    x86_map(c, DATA, 0x2000, 1);
    x86_map(c, STACK, 0x1000, 1);
}
static void reset(x86cpu *c, uint32_t seed) {
    static const uint32_t regs[8] = { 0x12345678, 0xFFFFFFFF, 0x80000000, DATA_BASE, STACK + 0x800, 0x7F, 0xFF00, 0xAAAA5555 };
    uint8_t d[0x2000];
    uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
    for (unsigned i = 0; i < sizeof d; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; d[i] = (uint8_t)(s >> 24); }
    /* x87 cells: doubles at +128, floats at +192 */
    for (unsigned i = 0; i < 8; i++) {
        float f = (float)fp_vals[i];
        memcpy(d + 0xFC0 + 128 + i * 8, &fp_vals[i], 8);
        memcpy(d + 0xFC0 + 192 + i * 8, &f, 4);
    }
    x86_write_mem(c, DATA, d, sizeof d);
    memset(d, 0, 0x1000);
    x86_write_mem(c, STACK, d, 0x1000);
    x86_write_mem(c, CODE, code, sizeof code);
    x86_write_mem(c, END, "\xCC", 1);
    memcpy(c->r, regs, sizeof regs);
    c->r[X86_ECX] ^= seed * 977u;
    c->eip = CODE;
    x86_set_flags(c, 0x202 | (seed & 1) | ((seed & 2) << 5));
    c->top = 0; c->ftw = 0xFFFF; c->fsw = 0; c->fcw = 0x37F;
    memset(c->st, 0, sizeof c->st);
    c->fault = 0; c->stop = 0;
    c->left = c->lbase = 1000000; c->budget = 0;
}
static bool same_mem(x86cpu *a, x86cpu *b, uint32_t at, uint32_t n) {
    static uint8_t x[0x2000], y[0x2000];
    x86_read_mem(a, at, x, n);
    x86_read_mem(b, at, y, n);
    return !memcmp(x, y, n);
}
static void show(uint32_t seed, x86cpu *a, x86cpu *b) {
    printf("seed %u: fault %d/%d eip %08x/%08x\n", seed, a->fault, b->fault, a->eip, b->eip);
    for (int i = 0; i < 8; i++) printf(" r%d %08x %08x%s\n", i, a->r[i], b->r[i], a->r[i] != b->r[i] ? " <" : "");
    printf(" flags %03x %03x\n", x86_get_flags(a) & 0x8D5, x86_get_flags(b) & 0x8D5);
    printf(" code:");
    for (unsigned i = 0; i < cp; i++) printf(" %02x", code[i]);
    printf("\n");
}

static x86cpu ref;
static QRXFullCPU fake;
static uint8_t fake_ram[0x10000];
static uintptr_t fake_page[QRX_WPAGE_ENTRIES ? 16 : 16];

static void map_tables(void) {
    for (uint32_t a = CODE; a < CODE + 0x2000; a += 0x1000) xj_table_page(a >> 12, pg(&host, a));
    for (uint32_t a = DATA; a < DATA + 0x2000; a += 0x1000) xj_table_page(a >> 12, pg(&host, a));
    xj_table_page(STACK >> 12, pg(&host, STACK));
}
static int run_case(uint32_t seed) {
    reset(&ref, seed);
    (void)x86_run(&ref, END);
    xj_kill_page(CODE >> 12);
    xj_kill_page((CODE >> 12) + 1);
    xj.page_kills[CODE >> 12] = 0;
    xj.page_kills[(CODE >> 12) + 1] = 0;
    reset(&host, seed);
    (void)xj_run(&host, END);
    bool ok = ref.fault == host.fault && ref.eip == host.eip && !memcmp(ref.r, host.r, sizeof ref.r) &&
              (x86_get_flags(&ref) & X86_ARITH) == (x86_get_flags(&host) & X86_ARITH) &&
              ref.top == host.top && ref.ftw == host.ftw && !memcmp(ref.st, host.st, sizeof ref.st) &&
              same_mem(&ref, &host, DATA, 0x2000) && same_mem(&ref, &host, STACK, 0x1000);
    if (!ok) { show(seed, &ref, &host); return 1; }
    return 0;
}

/* Code that patches the instruction it reaches next must see the new bytes. */
static void test_self_modifying(void) {
    cp = 0;
    c8(0xC7); c8(0x05); c32(CODE + 16); c32(0x11223344);    /* mov [imm of next mov], value */
    while (cp < 15) c8(0x90);
    c8(0xB8); c32(0x55555555);                              /* at CODE+15: mov eax,imm; imm at +16 */
    c8(0xE9); c32(END - (CODE + cp + 4));
    memset(code + cp, 0xCC, 8);
    reset(&host, 1);
    xj_kill_page(CODE >> 12);
    xj.page_kills[CODE >> 12] = 0;
    (void)xj_run(&host, END);
    assert(!host.fault);
    assert(host.r[X86_EAX] == 0x11223344);
    /* A write from the 68k side would come through xj_ram_written; this one came
     * through the interpreter's write path. The page must be translatable again. */
    reset(&host, 1);
    (void)xj_run(&host, END);
    assert(host.r[X86_EAX] == 0x11223344);
    puts("self-modifying code seen");
}
/* A loop stays inside linked blocks: the budget must still end it. */
static void test_budget(void) {
    cp = 0;
    c8(0x40);                                               /* inc eax */
    c8(0xEB); c8(0xFD);                                     /* jmp back */
    reset(&host, 1);
    xj_kill_page(CODE >> 12);
    xj.page_kills[CODE >> 12] = 0;
    host.left = host.lbase = 50000;
    (void)xj_run(&host, END);
    assert(host.fault == X86_FAULT_BUDGET);
    assert(host.left == 0);
    puts("budget ends a linked loop");
}
/* A crash in generated code: say where, and what the generated code around it is. */
static void on_crash(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    uint32_t *pc = (uint32_t *)u->uc_mcontext->__ss.__pc;
    fprintf(stderr, "signal %d at pc=%p addr=%p arena=%p..%p\n", sig, (void *)pc, si->si_addr, (void *)xj.arena, (void *)xj.next);
    if ((pc >= xj.arena && pc < xj.next)) {
        fprintf(stderr, "offset %ld words; runtime ends at %ld\n", (long)(pc - xj.arena), (long)(xj.arena_blocks - xj.arena));
        for (int i = -8; i <= 4; i++) fprintf(stderr, "%s%08x\n", i ? "  " : "> ", pc[i]);
    }
    fprintf(stderr, "x86 code:");
    for (unsigned i = 0; i < cp && i < 200; i++) fprintf(stderr, " %02x", code[i]);
    fprintf(stderr, "\n");
    _exit(3);
}
/* REP string instructions run in portions: a block must not go on until they are done. */
static void test_rep_large(void) {
    enum { BIG = 0x800000u, BYTES = 0x400000u };
    x86_map(&host, BIG, BYTES, 1);
    x86_map(&ref, BIG, BYTES, 1);
    for (uint32_t a = BIG; a < BIG + BYTES; a += 0x1000) xj_table_page(a >> 12, pg(&host, a));
    cp = 0;
    memset(code, 0x90, sizeof code);
    c8(0xB9); c32(BYTES / 4);                               /* mov ecx,count */
    c8(0xBF); c32(BIG);                                     /* mov edi,start */
    c8(0xB8); c32(0x12345678);                              /* mov eax,value */
    c8(0xF3); c8(0xAB);                                     /* rep stosd */
    c8(0x89); c8(0xCA);                                     /* mov edx,ecx: must see 0 */
    c8(0xB9); c32(BYTES / 4);                               /* and the same for rep movsd */
    c8(0xBE); c32(BIG);
    c8(0xBF); c32(BIG + BYTES / 2);
    c8(0xD1); c8(0xE9);                                     /* shr ecx,1 */
    c8(0xF3); c8(0xA5);                                     /* rep movsd */
    c8(0x89); c8(0xCD);                                     /* mov ebp,ecx */
    c8(0xE9); c32(END - (CODE + cp + 4));
    xj_kill_page(CODE >> 12);
    xj.page_kills[CODE >> 12] = 0;
    reset(&host, 1);
    host.left = host.lbase = 0x7FFFFFFF;
    (void)xj_run(&host, END);
    assert(!host.fault);
    assert(host.r[X86_EDX] == 0);
    assert(host.r[X86_EBP] == 0);
    assert(x86_rd32(&host, BIG + BYTES - 4) == 0x12345678);
    assert(host.r[X86_EDI] == BIG + BYTES);
    puts("large REP string instructions complete");
}
int main(int argc, char **argv) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    unsigned cases = argc > 1 ? (unsigned)atoi(argv[1]) : 200000u;
    unsigned failed = 0;
    x86_init(&host);
    x86_init(&ref);
    setup(&host);
    setup(&ref);
    fake.wpage = fake_page; fake.rpage = fake_page;
    fake.ram_owned = fake_ram;
    fake.vm.ram.size = sizeof fake_ram;
    assert(xj_init(&fake));
    map_tables();
    for (unsigned i = 0; i < cases && failed < 5; i++) {
        cp = 0;
        memset(code, 0x90, sizeof code);
        for (unsigned n = 1 + pick(30); n; n--) gen_one();
        c8(0xE9); c32(END - (CODE + cp + 4));
        failed += (unsigned)run_case(i + 1);
    }
    test_self_modifying();
    test_budget();
    test_rep_large();
    xj_report();
    printf("%u cases, %u differ\n", cases, failed);
    return failed != 0;
}
#endif
