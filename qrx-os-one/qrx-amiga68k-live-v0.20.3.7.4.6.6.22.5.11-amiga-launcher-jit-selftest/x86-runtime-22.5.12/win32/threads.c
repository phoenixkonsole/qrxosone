/* QRX OS One - qrx-win32: Windows threads, as coroutines on one AmigaOS task.
 *
 * Every guest thread has its own x86 registers, stack and TEB, plus a C stack of its own,
 * because it can be switched out from inside any HLE call or nested guest callback.
 * Switches happen at API calls and when a door quantum ends, at least every few ms. */
#ifndef __mc68000__
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#endif
#include "qrxwin.h"
#include <stdlib.h>
#include <string.h>
#ifdef __AROS__
#include <proto/exec.h>
#include <exec/tasks.h>
#endif

#define C (&w->cpu)
#define QT_MAX 32
#define QT_CSTACK (256u * 1024u)
#define QT_GUEST_STACK (1024u * 1024u)
#define QT_SLICE_MS 4u
#define QT_HANDLE_BASE 0x7E200000u

typedef struct qctx { void *sp; } qctx;
typedef struct qthread {
    int used, done, suspended, waiting;
    uint32_t id, start, param, teb, exit_code;
    void (*cfn)(qwin *w);
    void *cstack;
    qctx ctx;
    /* x86 state while switched out */
    uint32_t r[8], eip, eflags, lf_res, lf_a, lf_b, lf_cf, cur_eip, sbase_fs;
    int lf_op, lf_sz, top, depth;
    uint16_t sreg_fs, fsw, fcw, ftw;
    double st[8];
    jmp_buf *jb;
    /* The last errors while switched out; the running thread's are in qwin. */
    uint32_t last_error, net_error;
} qthread;

static qthread g_t[QT_MAX];
static int g_cur, g_live = 1, g_init;
static qwin *g_w;
static uint64_t g_slice_start;

/* ------------------------------------------------------------------ C stack switch */
#if defined(__mc68000__)
void qw_co_switch(qctx *from, qctx *to);
__asm__(
    ".text\n"
    ".globl qw_co_switch\n"
    "qw_co_switch:\n"
    "    move.l 4(%sp),%a0\n"
    "    move.l 8(%sp),%a1\n"
    "    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "    fmovem.x %fp2-%fp7,-(%sp)\n"
    "    move.l %sp,(%a0)\n"
    "    move.l (%a1),%sp\n"
    "    fmovem.x (%sp)+,%fp2-%fp7\n"
    "    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "    rts\n");
static void co_prepare(qctx *c, void *stack, size_t size, void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)(((uintptr_t)stack + size - 16u) & ~(uintptr_t)3u);
    *--sp = 0;                        /* return address of entry: never used */
    *--sp = (uint32_t)(uintptr_t)entry;
    sp -= 11;                         /* d2-d7/a2-a6 */
    memset(sp, 0, 11 * 4);
    sp -= 18;                         /* fp2-fp7, 12 bytes each */
    memset(sp, 0, 18 * 4);
    c->sp = sp;
}
#elif defined(__m68k64__)
/* 68100EC64 long mode (m68k64 clang): arguments in 8-byte stack slots, D2-D7/A2-A6
 * callee-saved, soft float, so no FPU registers to keep. */
void qw_co_switch(qctx *from, qctx *to);
__asm__(
    ".text\n"
    ".globl qw_co_switch\n"
    "qw_co_switch:\n"
    "    move.q (8,%sp),%a0\n"
    "    move.q (16,%sp),%a1\n"
    "    lea (-88,%sp),%sp\n"
    "    movem.q %d2-%d7/%a2-%a6,(0,%sp)\n"
    "    move.q %sp,(0,%a0)\n"
    "    move.q (0,%a1),%sp\n"
    "    movem.q (0,%sp),%d2-%d7/%a2-%a6\n"
    "    lea (88,%sp),%sp\n"
    "    rts\n");
static void co_prepare(qctx *c, void *stack, size_t size, void (*entry)(void))
{
    uint64_t *sp = (uint64_t *)(((uintptr_t)stack + size - 32u) & ~(uintptr_t)15u);
    *--sp = 0;                        /* return address of entry: never used */
    *--sp = (uint64_t)(uintptr_t)entry;
    sp -= 11;                         /* d2-d7/a2-a6 */
    memset(sp, 0, 11 * 8);
    c->sp = sp;
}
#else
#include <ucontext.h>
typedef struct { ucontext_t u; } qhostctx;
static void qw_co_switch(qctx *from, qctx *to)
{
    if (!from->sp) from->sp = calloc(1, sizeof(qhostctx));
    swapcontext(&((qhostctx *)from->sp)->u, &((qhostctx *)to->sp)->u);
}
static void co_prepare(qctx *c, void *stack, size_t size, void (*entry)(void))
{
    qhostctx *h = (qhostctx *)calloc(1, sizeof(qhostctx));
    getcontext(&h->u);
    h->u.uc_stack.ss_sp = stack;
    h->u.uc_stack.ss_size = size;
    h->u.uc_link = NULL;
    makecontext(&h->u, entry, 0);
    c->sp = h;
}
#endif

/* ------------------------------------------------------------------ switching */
static void save_cpu(qwin *w, qthread *t)
{
    x86cpu *c = C;
    memcpy(t->r, c->r, sizeof(t->r));
    t->eip = c->eip; t->eflags = c->eflags; t->cur_eip = c->cur_eip;
    t->lf_op = c->lf_op; t->lf_sz = c->lf_sz; t->lf_res = c->lf_res; t->lf_a = c->lf_a; t->lf_b = c->lf_b; t->lf_cf = c->lf_cf;
    t->sreg_fs = c->sreg[X86_FS]; t->sbase_fs = c->sbase[X86_FS];
    memcpy(t->st, c->st, sizeof(t->st));
    t->top = c->top; t->fsw = c->fsw; t->fcw = c->fcw; t->ftw = c->ftw;
    t->depth = c->depth; t->jb = c->jb;
}
static void load_cpu(qwin *w, qthread *t)
{
    x86cpu *c = C;
    memcpy(c->r, t->r, sizeof(t->r));
    c->eip = t->eip; c->eflags = t->eflags; c->cur_eip = t->cur_eip;
    c->lf_op = t->lf_op; c->lf_sz = t->lf_sz; c->lf_res = t->lf_res; c->lf_a = t->lf_a; c->lf_b = t->lf_b; c->lf_cf = t->lf_cf;
    c->sreg[X86_FS] = t->sreg_fs; c->sbase[X86_FS] = t->sbase_fs;
    memcpy(c->st, t->st, sizeof(t->st));
    c->top = t->top; c->fsw = t->fsw; c->fcw = t->fcw; c->ftw = t->ftw;
    c->depth = t->depth; c->jb = t->jb;
}
static void switch_to(qwin *w, int n)
{
    qthread *from = &g_t[g_cur], *to = &g_t[n];
    if (n == g_cur) return;
    save_cpu(w, from);
    load_cpu(w, to);
    from->last_error = w->last_error; from->net_error = w->net_error;
    w->last_error = to->last_error; w->net_error = to->net_error;
    g_cur = n;
    g_slice_start = w->be->now(w);
    qw_co_switch(&from->ctx, &to->ctx);
    /* Back on this thread. Another one may have ended the process or faulted. */
    {
        x86cpu *c = C;
        if ((w->exiting || c->fault) && c->jb) {
            if (w->exiting) c->stop = 1;
            longjmp(*c->jb, 1);
        }
    }
}
static int next_runnable(int from)
{
    int i;
    for (i = 1; i <= QT_MAX; i++) {
        int k = (from + i) % QT_MAX;
        if (g_t[k].used && !g_t[k].done && !g_t[k].suspended) return k;
    }
    return -1;
}
void qw_thread_yield(qwin *w)
{
    int n;
    if (g_live <= 1) return;
    n = next_runnable(g_cur);
    if (n >= 0) switch_to(w, n);
}
/* From the API path and the door loop: a thread that never blocks must share the CPU too. */
void qw_thread_tick(qwin *w)
{
    if (g_live <= 1) return;
    if (w->be->now(w) - g_slice_start >= QT_SLICE_MS) qw_thread_yield(w);
}
static void yield_hook(x86cpu *c) { qw_thread_tick((qwin *)c->user); }

/* One step of a blocking wait: run the others, and rest when everybody is waiting. */
void qw_thread_block(qwin *w, uint32_t max_rest_ms)
{
    int i, all = 1;
    qevent ev;
    g_t[g_cur].waiting = 1;
    for (i = 0; i < QT_MAX; i++) {
        if (g_t[i].used && !g_t[i].done && !g_t[i].suspended && !g_t[i].waiting) { all = 0; break; }
    }
    if (all && max_rest_ms) {
        int r = w->be->wait(w, max_rest_ms > 2u ? 2u : max_rest_ms, &ev);
        if (r > 0) user_input_event(w, &ev);
    }
    qw_thread_yield(w);
    g_t[g_cur].waiting = 0;
}
void qw_thread_sleep(qwin *w, uint32_t ms)
{
    uint64_t end = w->be->now(w) + ms;
    if (!ms) { qw_thread_yield(w); return; }
    for (;;) {
        uint64_t now = w->be->now(w);
        if (now >= end || w->exiting) return;
        qw_thread_block(w, (uint32_t)(end - now));
    }
}
/* Waiting for window system events must not stop the other threads. */
int qw_be_wait(qwin *w, uint32_t timeout, qevent *ev)
{
    uint64_t end;
    if (g_live <= 1) return w->be->wait(w, timeout, ev);
    end = w->be->now(w) + timeout;
    for (;;) {
        int r = w->be->wait(w, 0, ev);
        uint64_t now;
        if (r) return r;
        now = w->be->now(w);
        if (now >= end || w->exiting) return 0;
        qw_thread_block(w, (uint32_t)(end - now));
    }
}

/* ------------------------------------------------------------------ creation */
static void thread_exit_current(qwin *w)
{
    qthread *t = &g_t[g_cur];
    int n;
    t->done = 1;
    g_live--;
    n = next_runnable(g_cur);
    if (n < 0) n = 0;
    switch_to(w, n);
}
static void thread_entry(void)
{
    qwin *w = g_w;
    qthread *t = &g_t[g_cur];
    if (t->cfn) t->cfn(w);
    else {
        int ok = 0;
        uint32_t arg = t->param;
        t->exit_code = x86_call(C, t->start, 1, &arg, &ok);
    }
    thread_exit_current(w);
}
static void ensure_init(qwin *w)
{
    if (g_init) return;
    g_init = 1;
    g_w = w;
    g_t[0].used = 1;
    g_t[0].id = 0x104;
    g_t[0].teb = QW_TEB;
    g_slice_start = w->be->now(w);
    C->yield_hook = yield_hook;
#ifdef __AROS__
    {   /* The stacks below are ours; keep Exec's stack check out of it. */
        struct Task *me = FindTask(NULL);
        me->tc_SPLower = (APTR)0;
        me->tc_SPUpper = (APTR)~(uintptr_t)0;
    }
#endif
}
static int thread_new(qwin *w, uint32_t start, uint32_t param, void (*cfn)(qwin *), int suspended, uint32_t stack_bytes)
{
    int i;
    qthread *t;
    uint32_t stack, teb, top;
    ensure_init(w);
    for (i = 1; i < QT_MAX; i++) if (!g_t[i].used) break;
    if (i >= QT_MAX) return -1;
    t = &g_t[i];
    if (!stack_bytes || stack_bytes < 64u * 1024u) stack_bytes = QT_GUEST_STACK;
    stack = qw_alloc(w, stack_bytes);
    teb = qw_alloc(w, 0x2000);
    t->cstack = malloc(QT_CSTACK);
    if (!stack || !teb || !t->cstack) { free(t->cstack); t->cstack = NULL; return -1; }
    top = (stack + stack_bytes - 64u) & ~15u;
    memset(x86_host_ptr(C, teb), 0, 0x1000);
    x86_wr32(C, teb + 0x00, 0xFFFFFFFFu);
    x86_wr32(C, teb + 0x04, top);
    x86_wr32(C, teb + 0x08, stack);
    x86_wr32(C, teb + 0x18, teb);
    x86_wr32(C, teb + 0x20, 0x100);
    x86_wr32(C, teb + 0x24, 0x200u + (uint32_t)i * 4u);
    x86_wr32(C, teb + 0x2C, teb + 0x1000);
    { extern void qw_tls_new_thread(qwin *w, uint32_t teb); qw_tls_new_thread(w, teb); }
    x86_wr32(C, teb + 0x30, QW_PEB);
    t->used = 1; t->done = 0; t->waiting = 0; t->suspended = suspended;
    t->id = 0x200u + (uint32_t)i * 4u;
    t->start = start; t->param = param; t->cfn = cfn; t->teb = teb; t->exit_code = 259;  /* STILL_ACTIVE */
    /* Starts from a copy of the creator's FPU modes, on a clean register set. */
    save_cpu(w, t);
    memset(t->r, 0, sizeof(t->r));
    t->r[X86_ESP] = top;
    t->sbase_fs = teb;
    t->depth = 0; t->jb = NULL;
    t->top = 0; t->fsw = 0; t->ftw = 0xFFFF;
    t->last_error = 0; t->net_error = 0;
    t->ctx.sp = NULL;
    co_prepare(&t->ctx, t->cstack, QT_CSTACK, thread_entry);
    g_live++;
    return i;
}
void qw_thread_spawn_service(qwin *w, void (*fn)(qwin *))
{
    thread_new(w, 0, 0, fn, 0, 256u * 1024u);
}

/* ------------------------------------------------------------------ API */
uint32_t qw_thread_current_handle(qwin *w) { ensure_init(w); return QT_HANDLE_BASE + (uint32_t)g_cur; }
uint32_t qw_thread_current_id(void) { return g_init ? g_t[g_cur].id : 0x104; }
uint32_t qw_thread_current_teb(void) { return g_init ? g_t[g_cur].teb : QW_TEB; }
int qw_thread_handle_index(uint32_t h)
{
    uint32_t i = h - QT_HANDLE_BASE;
    return i < QT_MAX && g_t[i].used ? (int)i : -1;
}
int qw_thread_finished(int idx) { return g_t[idx].done; }
#define FN(name) uint32_t name(qwin *w, uint32_t *a)
FN(qw_api_CreateThread)
{
    int i = thread_new(w, a[2], a[3], NULL, (a[4] & 4u) != 0, a[1]);
    if (i < 0) { w->last_error = 8; return 0; }
    if (a[5]) x86_wr32(C, a[5], g_t[i].id);
    return QT_HANDLE_BASE + (uint32_t)i;
}
FN(qw_api_ExitThread)
{
    if (g_cur == 0) {          /* the main thread of a program whose other threads go on is rare */
        w->exit_code = (int)a[0];
        w->exiting = 1;
        C->stop = 1;
        return 0;
    }
    g_t[g_cur].exit_code = a[0];
    thread_exit_current(w);
    return 0;
}
FN(qw_api_ResumeThread)
{
    int i = qw_thread_handle_index(a[0]);
    uint32_t was;
    if (qw_proc_resume(w, a[0])) return 1;
    if (i < 0) return 0xFFFFFFFFu;
    was = (uint32_t)g_t[i].suspended;
    if (g_t[i].suspended) g_t[i].suspended--;
    return was;
}
FN(qw_api_SuspendThread)
{
    int i = a[0] == 0xFFFFFFFEu ? g_cur : qw_thread_handle_index(a[0]);
    uint32_t was;
    if (i < 0) return 0xFFFFFFFFu;
    was = (uint32_t)g_t[i].suspended;
    g_t[i].suspended++;
    if (i == g_cur) {
        while (g_t[i].suspended && !w->exiting) {
            if (next_runnable(g_cur) < 0) { g_t[i].suspended = 0; break; }   /* nobody could resume it */
            qw_thread_yield(w);
        }
    }
    return was;
}
FN(qw_api_TerminateThread)
{
    int i = qw_thread_handle_index(a[0]);
    if (i <= 0 || g_t[i].done) return 1;
    g_t[i].exit_code = a[1];
    if (i == g_cur) { thread_exit_current(w); return 1; }
    g_t[i].done = 1;
    g_live--;
    return 1;
}
FN(qw_api_GetExitCodeThread)
{
    int i = qw_thread_handle_index(a[0]);
    if (a[1]) x86_wr32(C, a[1], i >= 0 ? g_t[i].exit_code : 0u);
    return i >= 0;
}
