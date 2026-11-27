/* Cores of the 68100EC64 on host threads. Core 0 starts running, the others wait until core 0
 * signals them for the first time: then they take over its VBR and the stack it named. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "qrx68100ec64.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IPI_LEVEL 6u
#define TIMER_LEVEL 5u
#define SLICE 100000u

struct qec64_machine {
    qec64_cpu cpu[QEC64_MAX_CORES];
    pthread_t thread[QEC64_MAX_CORES];
    uint64_t next_timer_us[QEC64_MAX_CORES];
    unsigned cores;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool stopping;
    uint32_t active;                           /* cores that may touch shared memory, see core_wake */
    uint32_t in_gate;                          /* cores inside gf_gate: they run no guest code there */
    uint64_t wake_waits, wake_wait_us;         /* core_wake: times it waited for the others, and how long */
    enum qec64_status end;
    unsigned end_core;
    char *out;
    size_t cap, used;
    const char *in;
    size_t in_len, in_pos;
    void (*sink)(void *ctx, unsigned ch);      /* console output as it comes */
    void *sink_ctx;
    unsigned char queue[4096];                 /* console input pushed while running */
    unsigned q_head, q_tail;
    void *user;
    uint8_t *code_map;                         /* the JIT's, NULL with QRX_EC64_JIT=0 */
    uint32_t *code_gen;
};
static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
/* ---- Memory order between cores.
 * The JIT has two kinds of code (qrx68100ec64_jit.c): plain LDR/STR, and LDAR/STLR, which keeps
 * the order of every access as other cores see it. The ordered kind costs much on cores like the
 * Pi 4's Cortex-A72 (each STLR waits for the stores before it, an LDAR after an STLR for that
 * STLR). It is only needed while another core touches the shared memory too. A core sleeping in
 * STOP or in its idle task (quiet) touches none: its idle loop only writes the board's IDLE
 * register. So a core runs the plain kind while all the others are quiet, the ordered kind
 * otherwise, and a quiet core that is about to take an interrupt (core_wake, from qec64_run)
 * first marks itself active and waits until every other active core runs ordered code.
 * The hand-over, both sides sequentially consistent:
 *   waking core:  active |= me;            then reads each other core's `ordered`
 *   other core:   ordered = false;         then reads `active` again, back to ordered if set
 * so at least one of them sees the other's write: no core starts plain code while another wakes
 * unseen. Going to ordered code, a core first fences (its plain stores become visible) and then
 * sets `ordered`; the waking core reads that with acquire, so it sees those stores.
 * The interpreter's own accesses are plain C, ordered or not, as before. */
static void core_quiet(qec64_cpu *c)
{
    if (!c->active_mask || c->quiet) return;
    c->quiet = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* its stores before the others may go plain */
    __atomic_fetch_and(c->active_mask, ~(1u << c->core_id), __ATOMIC_SEQ_CST);
}
static void core_wake(qec64_cpu *c)
{
    qec64_machine *m = c->machine;
    uint32_t me = 1u << c->core_id;
    c->quiet = false;
    __atomic_store_n(&c->ordered, true, __ATOMIC_SEQ_CST);   /* no plain code before its next check */
    __atomic_fetch_or(&m->active, me, __ATOMIC_SEQ_CST);
    uint64_t t0 = 0;
    for (unsigned k = 0; k < m->cores; k++) {
        if (k == c->core_id) continue;
        qec64_cpu *o = &m->cpu[k];
        unsigned spins = 0;
        /* done: o is not active, or runs ordered code, or stands in the global Forbid's gate,
         * after which it looks at the active mask again before its next block */
        while ((__atomic_load_n(&m->active, __ATOMIC_SEQ_CST) >> k & 1) &&
               !__atomic_load_n(&o->ordered, __ATOMIC_SEQ_CST) &&
               !(__atomic_load_n(&m->in_gate, __ATOMIC_SEQ_CST) >> k & 1) &&
               !__atomic_load_n(&m->stopping, __ATOMIC_ACQUIRE)) {
            if (!t0) { t0 = now_us(); m->wake_waits++; }
            if (++spins > 100) sched_yield();
        }
    }
    if (t0) m->wake_wait_us += now_us() - t0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
static void wake_all(qec64_machine *m)
{
    pthread_mutex_lock(&m->lock);
    pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
}
static void on_ipi(qec64_cpu *self, uint64_t mask)
{
    qec64_machine *m = self->machine;
    for (unsigned i = 0; i < m->cores; i++) {
        if (!(mask >> i & 1)) continue;
        qec64_cpu *t = &m->cpu[i];
        if (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) {
            t->vbr = self->vbr;
            t->vbr_set = true;
            t->m64 = true;
            t->ipl = 0;
            t->a[7] = self->start_sp;
            t->waiting = true;
            __atomic_store_n(&t->started, true, __ATOMIC_RELEASE);
        }
        qec64_raise(t, IPI_LEVEL);
    }
    wake_all(m);
}
static void on_timer(qec64_cpu *self)
{
    qec64_machine *m = self->machine;
    m->next_timer_us[self->core_id] = self->timer_us ? now_us() + self->timer_us : 0;
}
static void on_console(qec64_cpu *self, unsigned ch)
{
    qec64_machine *m = self->machine;
    pthread_mutex_lock(&m->lock);
    if (m->out && m->used < m->cap) m->out[m->used++] = (char)ch;
    if (m->sink) m->sink(m->sink_ctx, ch);
    pthread_mutex_unlock(&m->lock);
}
static int on_console_in(qec64_cpu *self)
{
    qec64_machine *m = self->machine;
    int ch = -1;
    pthread_mutex_lock(&m->lock);
    if (m->in && m->in_pos < m->in_len) ch = (unsigned char)m->in[m->in_pos++];
    else if (m->q_head != m->q_tail) { ch = m->queue[m->q_head]; m->q_head = (m->q_head + 1) % sizeof m->queue; }
    pthread_mutex_unlock(&m->lock);
    return ch;
}
/* Cores asleep in the idle task's IDLE write (board): they touch no shared data, so a global
 * Forbid()'s owner need not wait for them to reach a block boundary (gf_gate). */
static uint32_t idle_asleep;
uint64_t qec64_machine_idle(qec64_cpu *c, uint64_t max_us)
{
    qec64_machine *m = c->machine;
    unsigned id = c->core_id;
    uint64_t start = now_us(), end = start + max_us;
    core_quiet(c);   /* until its next interrupt: the idle loop touches no shared memory */
    __atomic_fetch_or(&idle_asleep, 1u << (id & 31), __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&m->lock);
    while (!__atomic_load_n(&c->pending, __ATOMIC_SEQ_CST) && !m->stopping) {
        uint64_t cur = now_us(), n = m->next_timer_us[id];
        if (n && cur >= n) { m->next_timer_us[id] = n + c->timer_us; qec64_raise(c, TIMER_LEVEL); break; }
        if (cur >= end) break;
        uint64_t wait = end - cur;
        if (n && n - cur < wait) wait = n - cur;
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += (long)wait * 1000;
        t.tv_sec += t.tv_nsec / 1000000000;
        t.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&m->wake, &m->lock, &t);
    }
    pthread_mutex_unlock(&m->lock);
    __atomic_fetch_and(&idle_asleep, ~(1u << (id & 31)), __ATOMIC_SEQ_CST);
    return now_us() - start;
}
/* ---- Global Forbid()/Disable() for AROS SMP.
 * AROS keeps Forbid() and Disable() per core: on another core other tasks go on, and Amiga code
 * that protects shared lists with Forbid() breaks once tasks run on more than one core. Here the
 * machine makes them global. Each core's TLS (A8, arch/m68k64-native/kernel/tls.h) holds its
 * IDNestCnt and TDNestCnt; a core in user state with either >= 0 owns the machine, and the other
 * cores stop before their next block until it lets go. Exceptions:
 *  - a core in supervisor state (interrupts, the scheduler) is never stopped and never owns;
 *  - a core holding a kernel spinlock (the TLS byte at 31 counts them) runs on until it lets go,
 *    or the owner could wait for that lock forever;
 *  - a core with an interrupt to take takes it (Forbid() does not stop interrupts on the Amiga);
 *  - the owner waits at most GF_ACK_US for the others, a stopped core at most GF_PAUSE_US: a
 *    program that waits in Forbid() for another core slows down instead of hanging.
 * On with QRX_EC64_GFORBID=1, and with QRX_EC64_SMP_SPREAD (tasks on all cores) unless
 * QRX_EC64_GFORBID=0. */
#define TLS_IDNEST 28
#define TLS_TDNEST 29
#define TLS_SPINS 31
#define GF_ACK_US 5000u
#define GF_PAUSE_US 50000u
static int gf_owner = -1;
static uint32_t gf_stopped;                    /* cores that are standing in gf_gate */
static uint64_t gf_acquired, gf_ack_timeouts, gf_pause_timeouts;
static void gf_nap(void)
{
    struct timespec t = { 0, 20000 };
    nanosleep(&t, NULL);
}
static bool gf_irq(const qec64_cpu *c)
{
    uint32_t p = __atomic_load_n(&c->pending, __ATOMIC_RELAXED);
    if (!p) return false;
    unsigned level = 31u - (unsigned)__builtin_clz(p);
    return level > c->ipl || level == 7;
}
static bool gf_tls(const qec64_cpu *c, int *spins)
{
    uint64_t t = c->a[8];
    *spins = 0;
    if (!t || t + 40 > c->ram_size) return false;
    *spins = c->ram[t + TLS_SPINS];
    return (int8_t)c->ram[t + TLS_IDNEST] >= 0 || (int8_t)c->ram[t + TLS_TDNEST] >= 0;
}
static void gf_gate_in(qec64_cpu *c)
{
    qec64_machine *m = c->machine;
    int id = (int)c->core_id;
    for (;;) {
        int spins, want = !c->supervisor && gf_tls(c, &spins);
        int o = __atomic_load_n(&gf_owner, __ATOMIC_ACQUIRE);
        if (o == id) {
            if (!want && !c->supervisor) __atomic_store_n(&gf_owner, -1, __ATOMIC_RELEASE);
            return;
        }
        if (o >= 0) {
            if (c->supervisor || spins || gf_irq(c)) return;
            /* stand still until the owner is done */
            uint64_t until = now_us() + GF_PAUSE_US;
            __atomic_fetch_or(&gf_stopped, 1u << id, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&gf_owner, __ATOMIC_ACQUIRE) == o && !m->stopping && !gf_irq(c)) {
                if (now_us() > until) { gf_pause_timeouts++; break; }
                gf_nap();
            }
            __atomic_fetch_and(&gf_stopped, ~(1u << id), __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&gf_owner, __ATOMIC_ACQUIRE) == o) return;   /* timed out or interrupted */
            continue;
        }
        if (!want) return;
        int none = -1;
        if (!__atomic_compare_exchange_n(&gf_owner, &none, id, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        gf_acquired++;
        /* the others reach a block boundary, or sleep, or are not running yet */
        uint64_t until = now_us() + GF_ACK_US;
        for (unsigned k = 0; k < m->cores; k++) {
            if ((int)k == id) continue;
            qec64_cpu *o2 = &m->cpu[k];
            while (__atomic_load_n(&o2->started, __ATOMIC_ACQUIRE) && !o2->waiting && !o2->supervisor &&
                   !(__atomic_load_n(&gf_stopped, __ATOMIC_ACQUIRE) >> k & 1) &&
                   !(__atomic_load_n(&idle_asleep, __ATOMIC_ACQUIRE) >> k & 1) && !m->stopping) {
                if (now_us() > until) { gf_ack_timeouts++; break; }
                gf_nap();
            }
        }
        return;
    }
}
static void gf_gate(qec64_cpu *c)
{
    qec64_machine *m = c->machine;
    uint32_t me = 1u << c->core_id;
    __atomic_fetch_or(&m->in_gate, me, __ATOMIC_SEQ_CST);
    gf_gate_in(c);
    __atomic_fetch_and(&m->in_gate, ~me, __ATOMIC_SEQ_CST);   /* then want_ordered reads the active mask */
}
/* A core going to sleep (STOP) gives the machine back */
static void gf_sleep(qec64_cpu *c)
{
    int id = (int)c->core_id;
    __atomic_compare_exchange_n(&gf_owner, &id, -1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* QRX_EC64_CPUS=3,2,...: guest core k runs only on host CPU number k of the list (the Pi image keeps
 * core 3 for the emulation with isolcpus=3): no migrations, warm caches for each core's JIT code.
 * Cores past the end of the list are not pinned. */
static void pin_core(unsigned id)
{
#if defined(__linux__)
    const char *list = getenv("QRX_EC64_CPUS");
    if (!list) return;
    for (unsigned k = 0; *list; k++) {
        char *e;
        long cpu = strtol(list, &e, 10);
        if (e == list) return;
        if (k == id && cpu >= 0 && cpu < CPU_SETSIZE) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET((int)cpu, &set);
            if (pthread_setaffinity_np(pthread_self(), sizeof set, &set))
                fprintf(stderr, "[EC64] QRX_EC64_CPUS: cannot pin core %u to CPU %ld\n", id, cpu);
            return;
        }
        list = *e == ',' ? e + 1 : e;
    }
#else
    (void)id;
#endif
}

static void *core_thread(void *arg)
{
    qec64_cpu *c = arg;
    qec64_machine *m = c->machine;
    unsigned id = c->core_id;
    pin_core(id);
    for (;;) {
        if (__atomic_load_n(&m->stopping, __ATOMIC_ACQUIRE)) { core_quiet(c); return NULL; }
        if (!__atomic_load_n(&c->started, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&m->lock);
            if (!c->started && !m->stopping) pthread_cond_wait(&m->wake, &m->lock);
            pthread_mutex_unlock(&m->lock);
            continue;
        }
        enum qec64_status st = qec64_jit_run(c, SLICE);
        uint64_t next = m->next_timer_us[id];
        if (next && now_us() >= next) {
            m->next_timer_us[id] = next + c->timer_us;
            qec64_raise(c, TIMER_LEVEL);
        }
        if (st == QEC64_RUNNING) continue;
        if (st == QEC64_WAITING) {
            if (c->gate) gf_sleep(c);
            core_quiet(c);   /* STOP: nothing runs until an interrupt, which wakes it (core_wake) */
            pthread_mutex_lock(&m->lock);
            /* Ask again under the lock: a signal between the run and here must not be missed. */
            while (!__atomic_load_n(&c->pending, __ATOMIC_SEQ_CST) && !m->stopping) {
                struct timespec t;
                uint64_t n = m->next_timer_us[id];
                clock_gettime(CLOCK_REALTIME, &t);
                uint64_t wait = 100000, cur = now_us();
                if (n) {
                    if (cur >= n) { m->next_timer_us[id] = n + c->timer_us; qec64_raise(c, TIMER_LEVEL); break; }
                    if (n - cur < wait) wait = n - cur;
                }
                t.tv_nsec += (long)wait * 1000;
                t.tv_sec += t.tv_nsec / 1000000000;
                t.tv_nsec %= 1000000000;
                pthread_cond_timedwait(&m->wake, &m->lock, &t);
            }
            pthread_mutex_unlock(&m->lock);
            continue;
        }
        core_quiet(c);
        pthread_mutex_lock(&m->lock);
        if (!m->stopping) { m->end = st; m->end_core = id; m->stopping = true; }
        pthread_cond_broadcast(&m->wake);
        pthread_mutex_unlock(&m->lock);
        return NULL;
    }
}
qec64_machine *qec64_machine_new(uint8_t *ram, uint64_t ram_size, unsigned cores)
{
    qec64_machine *m = calloc(1, sizeof *m);
    if (!m || cores == 0 || cores > QEC64_MAX_CORES) { free(m); return NULL; }
    m->cores = cores;
    m->active = 1;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->wake, NULL);
    for (unsigned i = 0; i < cores; i++) {
        qec64_cpu *c = &m->cpu[i];
        qec64_init(c, ram, ram_size, i);
        c->num_cores = cores;
        c->machine = m;
        c->on_ipi = on_ipi;
        c->active_mask = &m->active;
        c->stopping = &m->stopping;
        c->on_wake = core_wake;
        c->quiet = i != 0;          /* core 0 runs from the start, the others wait for their first signal */
        c->on_timer = on_timer;
        c->on_console = on_console;
        c->on_console_in = on_console_in;
    }
    {
        const char *g = getenv("QRX_EC64_GFORBID"), *sp = getenv("QRX_EC64_SMP_SPREAD");
        bool on = g ? atoi(g) != 0 : (sp && atoi(sp) != 0);
        if (on && cores > 1)
            for (unsigned i = 0; i < cores; i++) m->cpu[i].gate = gf_gate;
    }
    const char *jit = getenv("QRX_EC64_JIT");
    if ((!jit || strcmp(jit, "0")) && qec64_jit_map_new(ram_size, &m->code_map, &m->code_gen))
        for (unsigned i = 0; i < cores; i++) { m->cpu[i].code_map = m->code_map; m->cpu[i].code_gen = m->code_gen; }
    return m;
}
void qec64_machine_free(qec64_machine *m)
{
    if (m->cpu[0].gate)
        fprintf(stderr, "[EC64] global Forbid: %llu times, %llu waits for a core cut short, %llu stops cut short\n",
                (unsigned long long)gf_acquired, (unsigned long long)gf_ack_timeouts, (unsigned long long)gf_pause_timeouts);
    if (m->cores > 1 && getenv("QRX_EC64_JIT_STATS"))
        fprintf(stderr, "[EC64] cores waking waited %llu times for the others to order, %llu us in all\n",
                (unsigned long long)m->wake_waits, (unsigned long long)m->wake_wait_us);
    for (unsigned i = 0; i < m->cores; i++) qec64_jit_free(&m->cpu[i]);
    free(m->code_map);
    free(m->code_gen);
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->wake);
    free(m);
}
qec64_cpu *qec64_machine_core(qec64_machine *m, unsigned i) { return i < m->cores ? &m->cpu[i] : NULL; }
void qec64_machine_gforbid_state(qec64_machine *m, int *owner, uint32_t *stopped)
{
    (void)m;
    *owner = __atomic_load_n(&gf_owner, __ATOMIC_RELAXED);
    *stopped = __atomic_load_n(&gf_stopped, __ATOMIC_RELAXED);
}
void qec64_machine_console(qec64_machine *m, char *out, size_t cap) { m->out = out; m->cap = cap; m->used = 0; }
size_t qec64_machine_console_used(const qec64_machine *m) { return m->used; }
void qec64_machine_console_input(qec64_machine *m, const char *in, size_t len) { m->in = in; m->in_len = len; m->in_pos = 0; }
void qec64_machine_console_sink(qec64_machine *m, void (*fn)(void *ctx, unsigned ch), void *ctx)
{
    pthread_mutex_lock(&m->lock);
    m->sink = fn;
    m->sink_ctx = ctx;
    pthread_mutex_unlock(&m->lock);
}
size_t qec64_machine_console_push(qec64_machine *m, const char *s, size_t n)
{
    size_t done = 0;
    pthread_mutex_lock(&m->lock);
    for (; done < n; done++) {
        unsigned next = (m->q_tail + 1) % sizeof m->queue;
        if (next == m->q_head) break;
        m->queue[m->q_tail] = (unsigned char)s[done];
        m->q_tail = next;
    }
    pthread_mutex_unlock(&m->lock);
    return done;
}
void qec64_machine_set_mmio(qec64_machine *m, bool (*fn)(qec64_cpu *, uint64_t, unsigned, bool, uint64_t *), void *user)
{
    m->user = user;
    for (unsigned i = 0; i < m->cores; i++) m->cpu[i].on_mmio = fn;
}
void *qec64_machine_user(const qec64_cpu *c) { return ((const qec64_machine *)c->machine)->user; }
void qec64_machine_stop(qec64_machine *m)
{
    pthread_mutex_lock(&m->lock);
    m->stopping = true;
    pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
}
enum qec64_status qec64_machine_run(qec64_machine *m, uint64_t pc, uint64_t sp, double seconds, unsigned *which)
{
    m->stopping = false;
    m->end = QEC64_RUNNING;
    m->cpu[0].pc = pc;
    m->cpu[0].a[7] = sp;
    m->cpu[0].started = true;
    for (unsigned i = 0; i < m->cores; i++) pthread_create(&m->thread[i], NULL, core_thread, &m->cpu[i]);
    uint64_t limit = now_us() + (uint64_t)(seconds * 1e6);
    /* QRX_EC64_PCSAMPLE=<file>: every 20 ms each running core's pc, its supervisor flag and the
     * 16 bytes at the pc (to find the code in a relocated program) and 64 words of its stack,
     * one line per sample: a
     * profile for a program that runs too long. The values are read while the cores run. */
    const char *sample_path = getenv("QRX_EC64_PCSAMPLE");
    FILE *sample = sample_path ? fopen(sample_path, "w") : NULL;
    pthread_mutex_lock(&m->lock);
    while (!m->stopping && now_us() < limit) {
        struct timespec t;
        if (sample)
            for (unsigned i = 0; i < m->cores; i++) {
                const qec64_cpu *c = &m->cpu[i];
                uint64_t pc = c->pc;
                fprintf(sample, "%u %016llx %d", i, (unsigned long long)pc, (int)c->supervisor);
                for (unsigned k = 0; k < 16 && pc + k < c->ram_size; k++) fprintf(sample, " %02x", c->ram[pc + k]);
                /* then the stack: 64 big-endian 64-bit words from a7, for return addresses */
                uint64_t sp = c->a[7];
                fprintf(sample, " |");
                for (unsigned k = 0; k < 64 && sp + 8 * k + 8 <= c->ram_size; k++) {
                    uint64_t v = 0;
                    for (unsigned b = 0; b < 8; b++) v = v << 8 | c->ram[sp + 8 * k + b];
                    fprintf(sample, " %llx", (unsigned long long)v);
                }
                fputc('\n', sample);
            }
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 20000000;
        t.tv_sec += t.tv_nsec / 1000000000;
        t.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&m->wake, &m->lock, &t);
    }
    m->stopping = true;
    pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
    for (unsigned i = 0; i < m->cores; i++) pthread_join(m->thread[i], NULL);
    if (sample) fclose(sample);
    if (which) *which = m->end_core;
    return m->end;
}
