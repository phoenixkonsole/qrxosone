/* Compiled with qrx_core1_rename.h: Musashi and the JIT in this library are core 1's own
 * copies. Names here that also exist on core 0 refer to the copy. */
#include "qrx68100_core1.h"
#include "qrx68100_jit.h"
#include "qrx_atomic_ram.h"
#include "m68k.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define QRX_PAGE_ENTRIES (1u << 20)
#define SMP_BASE 0xFF000600u
#define SMP_END  0xFF000640u
#define VEC_BASE 0xFF000A00u
#define VEC_END  0xFF000E00u    /* a table of 256 vectors, all pointing at the trap below */
#define VEC_TRAP 0xFF000E00u
enum { ST_PARKED, ST_START, ST_RUNNING, ST_QUIT };
enum { FEAT_DUAL = 1u, FEAT_NATIVE_JIT = 1u << 3, FEAT_PARALLEL = 1u << 4 };
enum { STATUS_CORE0 = 1u, STATUS_CORE1 = 2u, STATUS_FAULT = 4u };

static struct {
    QRXFullCPU *c0;
    QRXFullCPU c1;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool thread_up, maps_up;
    atomic_int state;
    atomic_bool park, flush, faulted, trapped;
    atomic_uint fault_pc, fault_info;
    atomic_ullong instructions;
    uint32_t pc, sp;
    size_t mapped_ram;
} g;

/* ---------- registers ---------- */
static bool smp_read(unsigned core, uint32_t addr, uint32_t *out)
{
    switch (addr - SMP_BASE) {
    case 0x00: *out = 0x51525844u; return true;
    case 0x04: *out = 0x00020000u; return true;
    case 0x08: *out = FEAT_DUAL | FEAT_NATIVE_JIT | FEAT_PARALLEL; return true;
    case 0x0C: {
        int st = atomic_load(&g.state);
        *out = STATUS_CORE0 | (st == ST_START || st == ST_RUNNING ? STATUS_CORE1 : 0u) |
               (atomic_load(&g.faulted) ? STATUS_FAULT : 0u);
        return true;
    }
    case 0x10: *out = g.pc; return true;
    case 0x14: *out = g.sp; return true;
    case 0x34: *out = core; return true;
    case 0x38: *out = atomic_load(&g.fault_pc); return true;
    case 0x3C: *out = atomic_load(&g.fault_info); return true;
    case 0x28: *out = (uint32_t)atomic_load(&g.instructions); return true;
    }
    *out = 0;
    return true;
}
/* Compilers turn a volatile bit test into a byte access such as BTST, so narrow reads
 * must see the matching part of the big-endian register. */
static bool smp_read_any(unsigned core, uint32_t addr, unsigned width, uint32_t *out)
{
    uint32_t v = 0;
    smp_read(core, addr & ~3u, &v);
    unsigned shift = 8u * (4u - width - (addr & 3u));
    *out = width == 4 ? v : (v >> shift) & (width == 2 ? 0xFFFFu : 0xFFu);
    return true;
}
static bool ensure_thread(void);
static void smp_write(unsigned core, uint32_t addr, uint32_t value)
{
    switch (addr - SMP_BASE) {
    case 0x10: g.pc = value; return;
    case 0x14: g.sp = value; return;
    case 0x18:
        if (value == 2) {             /* park: core 1 stops at its next check */
            atomic_store(&g.park, true);
            if (core == 1) g.c1.vm.cpu.halted = true;
            return;
        }
        if ((value == 1 || value == 3) && core == 0 && atomic_load(&g.state) == ST_PARKED && ensure_thread()) {
            /* 3 keeps core 1's translations; 1 drops them, since 68040 code loaded
             * meanwhile is not snooped across cores. */
            if (value == 1) atomic_store(&g.flush, true);
            if (!g.c0->smp_strong) {
                g.c0->smp_strong = true;      /* from here on core 0 emits ordered accesses */
                if (g.c0->jit_request_flush) g.c0->jit_request_flush(g.c0);
            }
            pthread_mutex_lock(&g.lock);
            atomic_store(&g.park, false);
            atomic_store(&g.faulted, false);
            atomic_store(&g.state, ST_START);
            pthread_cond_signal(&g.wake);
            pthread_mutex_unlock(&g.lock);
        }
        return;
    case 0x30:                         /* software cache coherence, as on two 68040s */
        atomic_store(&g.flush, true);
        if (g.c0->jit_request_flush) g.c0->jit_request_flush(g.c0);
        return;
    }
}
static bool c0_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out)
{
    (void)c;
    if (addr < SMP_BASE || addr >= SMP_END) return false;
    return smp_read_any(0, addr, width, out);
}
static bool c0_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value)
{
    (void)c;
    if (addr < SMP_BASE || addr >= SMP_END) return false;
    if (width == 4 && !(addr & 3u)) smp_write(0, addr, value);
    return true;
}

/* ---------- core 1's bus: RAM, the SMP registers and nothing else ---------- */
static inline uint8_t *ram_ptr(uint32_t addr, unsigned width)
{
    uintptr_t base = g.c1.rpage[addr >> 12];
    if (!base || (uint64_t)addr + width > g.c1.vm.ram.size || (addr & 0xFFFu) > 0x1000u - width) return NULL;
    return (uint8_t *)(base + addr);
}
static inline void code_written(uint32_t addr, unsigned width)
{
    if (!g.c1.wpage[addr >> QRX_WPAGE_SHIFT] && g.c1.jit_notify) g.c1.jit_notify(&g.c1, addr, width);
}
bool qrx_full_bus_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out)
{
    (void)c;
    const uint8_t *p = ram_ptr(addr, width);
    if (p) {
        if (width == 4) { uint32_t v; memcpy(&v, p, 4); *out = __builtin_bswap32(v); }
        else if (width == 2) { uint16_t v; memcpy(&v, p, 2); *out = __builtin_bswap16(v); }
        else *out = *p;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return true;
    }
    if (addr >= SMP_BASE && addr < SMP_END) return smp_read_any(1, addr, width, out);
    if (addr >= VEC_BASE && addr < VEC_END) { *out = width == 4 ? VEC_TRAP : 0; return true; }
    if (addr >= VEC_TRAP && addr < VEC_TRAP + 8u) {
        /* An exception ended here: answer with STOP and let the thread report it. */
        atomic_store(&g.trapped, true);
        *out = width == 4 ? 0x4E722700u : width == 2 ? ((addr & 2u) ? 0x2700u : 0x4E72u) : 0;
        return true;
    }
    if (width > 1 && (addr & 0xFFFu) > 0x1000u - width) {      /* across two RAM pages */
        uint32_t v = 0, b;
        for (unsigned i = 0; i < width; i++) {
            if (!qrx_full_bus_read(c, addr + i, 1, &b)) return false;
            v = v << 8 | b;
        }
        *out = v;
        return true;
    }
    return false;
}
bool qrx_full_bus_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value)
{
    uint8_t *p = ram_ptr(addr, width);
    if (p) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if (width == 4) { uint32_t v = __builtin_bswap32(value); memcpy(p, &v, 4); }
        else if (width == 2) { uint16_t v = __builtin_bswap16((uint16_t)value); memcpy(p, &v, 2); }
        else *p = (uint8_t)value;
        code_written(addr, width);
        return true;
    }
    if (addr >= SMP_BASE && addr < SMP_END) {
        if (width == 4 && !(addr & 3u)) smp_write(1, addr, value);
        return true;
    }
    if (width > 1 && (addr & 0xFFFu) > 0x1000u - width) {
        for (unsigned i = 0; i < width; i++) {
            if (!qrx_full_bus_write(c, addr + i, 1, value >> (8u * (width - 1u - i)))) return false;
        }
        return true;
    }
    return false;
}
int qrx_atomic_cas(unsigned width, unsigned ea, unsigned cmp, unsigned val, unsigned *old)
{
    uint8_t *p = ram_ptr(ea, width);
    int swapped = 0;
    uint32_t o = 0;
    if (!p || !qrx_ram_cas(p, width, cmp, val, &o, &swapped)) return 0;
    *old = o;
    if (swapped) code_written(ea, width);
    return 1;
}
int qrx_atomic_tas(unsigned ea, unsigned *old)
{
    uint8_t *p = ram_ptr(ea, 1);
    if (!p) return 0;
    *old = __atomic_fetch_or(p, 0x80u, __ATOMIC_SEQ_CST);
    code_written(ea, 1);
    return 1;
}
static unsigned cb_read(unsigned a, unsigned w)
{
    uint32_t v = 0;
    if (!qrx_full_bus_read(&g.c1, a, w, &v)) {
        g.c1.mem_fault = true; g.c1.fault_address = a;
        v = 0xFFFFFFFFu;
    }
    return v;
}
static void cb_write(unsigned a, unsigned w, unsigned v)
{
    if (!qrx_full_bus_write(&g.c1, a, w, v)) { g.c1.mem_fault = true; g.c1.fault_address = a; }
}
unsigned int m68k_read_memory_8(unsigned int a) { return cb_read(a, 1); }
unsigned int m68k_read_memory_16(unsigned int a) { return cb_read(a, 2); }
unsigned int m68k_read_memory_32(unsigned int a) { return cb_read(a, 4); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { cb_write(a, 1, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { cb_write(a, 2, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { cb_write(a, 4, v); }

/* ---------- the thread ---------- */
/* Called on core 0's thread while core 1 is parked. Core 1 gets its own write map, because
 * the granules a core gates depend on what that core has translated. */
static bool sync_maps(void)
{
    QRXFullCPU *c0 = g.c0, *c = &g.c1;
    if (!g.maps_up) {
        void *r = mmap(NULL, QRX_PAGE_ENTRIES * sizeof(uintptr_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        void *w = mmap(NULL, QRX_WPAGE_ENTRIES * sizeof(uintptr_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED || w == MAP_FAILED) return false;
        c->rpage = r; c->wpage = w;
        g.maps_up = true;
    }
    if (g.mapped_ram == c0->vm.ram.size) return true;
    /* Only RAM is visible to core 1, so ROM stays absent from both maps. */
    uint32_t pages = (uint32_t)(c0->vm.ram.size >> 12);
    memset(c->rpage, 0, QRX_PAGE_ENTRIES * sizeof(uintptr_t));
    memset(c->wpage, 0, QRX_WPAGE_ENTRIES * sizeof(uintptr_t));
    for (uint32_t p = 0; p < pages; p++) {
        uintptr_t base = (uintptr_t)c0->ram_owned;    /* core 0's map hides page 0 for its ROM overlay */
        c->rpage[p] = base;
        for (uint32_t i = 0; i < (1u << (12u - QRX_WPAGE_SHIFT)); i++) c->wpage[(p << (12u - QRX_WPAGE_SHIFT)) + i] = base;
    }
    c->vm.ram.size = c0->vm.ram.size;
    g.mapped_ram = c0->vm.ram.size;
    return true;
}
static void run_job(void)
{
    QRXFullCPU *c = &g.c1;
    uint64_t start = c->instructions;
    c->mem_fault = false;
    c->vm.cpu.halted = false;
    atomic_store(&g.trapped, false);
    atomic_store(&g.fault_pc, 0);
    atomic_store(&g.fault_info, 0);
    if (atomic_exchange(&g.flush, false) && c->jit_request_flush) c->jit_request_flush(c);
    m68k_pulse_reset();                /* reads vectors that do not matter: set just below */
    c->mem_fault = false;
    m68k_set_reg(M68K_REG_VBR, VEC_BASE);
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_ISP, g.sp);
    m68k_set_reg(M68K_REG_A7, g.sp);
    m68k_set_reg(M68K_REG_PC, g.pc);
    atomic_store(&g.state, ST_RUNNING);
    while (!atomic_load(&g.park) && !c->mem_fault && !c->vm.cpu.halted && !qrx_jit_cpu_stopped()) {
        if (atomic_load(&g.flush) && atomic_exchange(&g.flush, false) && c->jit_request_flush) c->jit_request_flush(c);
        c->jit_exec(c, 200000);
        atomic_store(&g.instructions, c->instructions - start);
    }
    if (atomic_load(&g.trapped)) {
        /* Format 0..7 frames all begin with SR.w, PC.l, format/vector.w. */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7), pc = 0, fv = 0;
        qrx_full_bus_read(c, sp + 2u, 4, &pc);
        qrx_full_bus_read(c, sp + 6u, 2, &fv);
        atomic_store(&g.fault_pc, pc);
        atomic_store(&g.fault_info, (fv & 0xFFFu) >> 2);
        atomic_store(&g.faulted, true);
    } else if (c->mem_fault) {
        atomic_store(&g.fault_pc, m68k_get_reg(NULL, M68K_REG_PC));
        atomic_store(&g.fault_info, c->fault_address);
        atomic_store(&g.faulted, true);
    }
}
static void *core1_main(void *arg)
{
    (void)arg;
    QRXFullCPU *c = &g.c1;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    c->cpu_type = 68040;
    c->fast_bus = true;
    c->smp_strong = true;
    if (!qrx_jit_init(c, 0)) {
        fprintf(stderr, "[CORE1] JIT unavailable, jobs run on Musashi\n");
        c->jit_exec = NULL;
    }
    for (;;) {
        pthread_mutex_lock(&g.lock);
        while (atomic_load(&g.state) == ST_PARKED) pthread_cond_wait(&g.wake, &g.lock);
        int st = atomic_load(&g.state);
        pthread_mutex_unlock(&g.lock);
        if (st == ST_QUIT) break;
        if (c->jit_exec) run_job();
        else atomic_store(&g.faulted, true);
        /* A quit request that came in during the job must not be overwritten. */
        int running = ST_RUNNING;
        if (!atomic_compare_exchange_strong(&g.state, &running, ST_PARKED)) break;
    }
    if (c->jit) qrx_jit_destroy(c);
    return NULL;
}
static bool ensure_thread(void)
{
    if (!sync_maps()) return false;
    if (g.thread_up) return true;
    if (pthread_create(&g.thread, NULL, core1_main, NULL) != 0) return false;
    g.thread_up = true;
    return true;
}
bool qrx_core1_attach(QRXFullCPU *c0)
{
    if (!c0 || !c0->jit || g.c0) return false;
    g.c0 = c0;
    pthread_mutex_init(&g.lock, NULL);
    pthread_cond_init(&g.wake, NULL);
    c0->smp_read = c0_read;
    c0->smp_write = c0_write;
    return true;
}
void qrx_core1_detach(QRXFullCPU *c0)
{
    if (!c0 || g.c0 != c0) return;
    c0->smp_read = NULL;
    c0->smp_write = NULL;
    if (g.thread_up) {
        pthread_mutex_lock(&g.lock);
        atomic_store(&g.park, true);
        atomic_store(&g.state, ST_QUIT);
        pthread_cond_signal(&g.wake);
        pthread_mutex_unlock(&g.lock);
        pthread_join(g.thread, NULL);
        g.thread_up = false;
    }
    if (g.maps_up) {
        munmap(g.c1.rpage, QRX_PAGE_ENTRIES * sizeof(uintptr_t));
        munmap(g.c1.wpage, QRX_WPAGE_ENTRIES * sizeof(uintptr_t));
        g.c1.rpage = NULL; g.c1.wpage = NULL;
        g.maps_up = false;
    }
    g.mapped_ram = 0;
    g.c0 = NULL;
}
