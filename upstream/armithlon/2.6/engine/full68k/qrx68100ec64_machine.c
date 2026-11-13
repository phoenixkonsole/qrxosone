/* Cores of the 68100EC64 on host threads. Core 0 starts running, the others wait until core 0
 * signals them for the first time: then they take over its VBR and the stack it named. */
#include "qrx68100ec64.h"
#include <pthread.h>
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
static void *core_thread(void *arg)
{
    qec64_cpu *c = arg;
    qec64_machine *m = c->machine;
    unsigned id = c->core_id;
    for (;;) {
        if (__atomic_load_n(&m->stopping, __ATOMIC_ACQUIRE)) return NULL;
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
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->wake, NULL);
    for (unsigned i = 0; i < cores; i++) {
        qec64_cpu *c = &m->cpu[i];
        qec64_init(c, ram, ram_size, i);
        c->num_cores = cores;
        c->machine = m;
        c->on_ipi = on_ipi;
        c->on_timer = on_timer;
        c->on_console = on_console;
        c->on_console_in = on_console_in;
    }
    const char *jit = getenv("QRX_EC64_JIT");
    if ((!jit || strcmp(jit, "0")) && qec64_jit_map_new(ram_size, &m->code_map, &m->code_gen))
        for (unsigned i = 0; i < cores; i++) { m->cpu[i].code_map = m->code_map; m->cpu[i].code_gen = m->code_gen; }
    return m;
}
void qec64_machine_free(qec64_machine *m)
{
    for (unsigned i = 0; i < m->cores; i++) qec64_jit_free(&m->cpu[i]);
    free(m->code_map);
    free(m->code_gen);
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->wake);
    free(m);
}
qec64_cpu *qec64_machine_core(qec64_machine *m, unsigned i) { return i < m->cores ? &m->cpu[i] : NULL; }
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
    pthread_mutex_lock(&m->lock);
    while (!m->stopping && now_us() < limit) {
        struct timespec t;
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
    if (which) *which = m->end_core;
    return m->end;
}
