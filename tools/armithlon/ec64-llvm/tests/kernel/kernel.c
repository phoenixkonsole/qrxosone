/* A small kernel for the 68100EC64: what an AROS port will need from the core, the compiler and the
 * linker, all at once. Two cores share a run queue under a spin lock. Tasks run in user state with
 * their own stacks, the timer preempts them, a core with nothing to do sleeps with STOP until a
 * signal. Faults of a task end that task only. The result goes to the console. */
typedef unsigned long u64;
typedef long i64;
typedef unsigned u32;
typedef unsigned char u8;
typedef __SIZE_TYPE__ size_t;

extern u64 core_id(void), core_count(void);
extern void set_vbr(void *), set_timer(u64), signal_cores(u64), set_start_stack(void *), putc_raw(u64);
extern void halt(u64) __attribute__((noreturn));
extern void irq_on(void), irq_off(void), idle_until_irq(void);
extern void resume(void *) __attribute__((noreturn));
extern u64 sys_call(u64 number, u64 arg);
extern void user_privileged(void);
extern void vec_2(void), vec_4(void), vec_5(void), vec_8(void), vec_10(void), vec_11(void), vec_29(void), vec_30(void), vec_32(void), vec_33(void);

void *memset(void *d, int v, size_t n) { u8 *a = d; while (n--) *a++ = (u8)v; return d; }
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }

/* What exception_common leaves on the stack, lowest address first. */
struct regs {
    u64 usp;
    u64 d[8], a[7];
    u64 vector;
    unsigned short sr, format;
    u32 reserved;
    u64 pc;
    u64 fault_address;   /* format 1 only */
};
#define SYS_PUT 1
#define SYS_EXIT 2
#define NTASKS 8
#define STACK 8192
enum { FREE, READY, RUNNING, DEAD };
struct task {
    int state;
    void *sp;
    struct task *next;
    u64 slices;
    u64 ran_on;           /* a bit per core */
    u64 result;
};
static struct task tasks[NTASKS];
static u8 kstack[NTASKS][STACK] __attribute__((aligned(16)));
static u8 ustack[NTASKS][STACK] __attribute__((aligned(16)));
static u8 idle_stack[2][STACK] __attribute__((aligned(16)));
u8 boot_stack[STACK] __attribute__((aligned(16)));
__asm__(".globl boot_stack_top\n.set boot_stack_top, boot_stack + 8192\n");

static volatile u64 run_lock, console_lock;
static struct task *queue_head, *queue_tail;
static struct task *current[2];
static volatile u64 live_tasks;
static volatile u64 ticks[2], signals[2], switches[2], faults;
static volatile u64 shared_count, plain_count;
static volatile u64 core1_up;

static inline void lock(volatile u64 *l) { u64 want = 0; while (!__atomic_compare_exchange_n(l, &want, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) want = 0; }
static inline void unlock(volatile u64 *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static void put_raw_str(const char *s) { while (*s) putc_raw((u64)(u8)*s++); }
static void put_hex(u64 v) { for (int i = 60; i >= 0; i -= 4) putc_raw((u64)"0123456789abcdef"[(v >> i) & 15]); }
static void say(const char *s, u64 v) { lock(&console_lock); put_raw_str(s); put_raw_str(" "); put_hex(v); putc_raw('\n'); unlock(&console_lock); }

static void enqueue(struct task *t) { t->next = 0; if (queue_tail) queue_tail->next = t; else queue_head = t; queue_tail = t; }
static struct task *dequeue(void) { struct task *t = queue_head; if (t) { queue_head = t->next; if (!queue_head) queue_tail = 0; } return t; }

/* Pick something for this core, or its idle frame. Returns the stack to resume. */
static void *schedule(u64 me, void *idle_sp)
{
    struct task *t;
    lock(&run_lock);
    t = dequeue();
    if (t) { t->state = RUNNING; t->slices++; t->ran_on |= 1UL << me; current[me] = t; switches[me]++; }
    else current[me] = 0;
    unlock(&run_lock);
    return t ? t->sp : idle_sp;
}
static void *idle_frame[2];

/* All exceptions land here, with interrupts masked. Returns the stack to go on with. */
void *exception(struct regs *r)
{
    u64 me = core_id();
    struct task *t = current[me];
    switch (r->vector) {
    case 29: ticks[me]++; break;
    case 30: signals[me]++; break;
    case 32:
        if (r->d[0] == SYS_PUT) { lock(&console_lock); putc_raw(r->d[1]); unlock(&console_lock); return r; }
        if (r->d[0] == SYS_EXIT) {
            t->result = r->d[1];
            t->state = DEAD;
            __atomic_fetch_sub(&live_tasks, 1, __ATOMIC_SEQ_CST);
            current[me] = 0;
            if (!idle_frame[me]) { lock(&console_lock); put_raw_str("no idle frame\n"); unlock(&console_lock); halt(3); }
            return schedule(me, idle_frame[me]);
        }
        break;
    case 2: case 4: case 5: case 8: case 10: case 11:
        __atomic_fetch_add(&faults, 1, __ATOMIC_SEQ_CST);
        if (!t || (r->sr & 0x2000)) { say("kernel fault, vector", r->vector); say("at", r->pc); halt(2); }
        /* The task's result says what ended it. */
        t->result = 0xFA000000UL + r->vector + (r->format & 0xF000 ? r->fault_address << 8 : 0);
        t->state = DEAD;
        __atomic_fetch_sub(&live_tasks, 1, __ATOMIC_SEQ_CST);
        current[me] = 0;
        return schedule(me, idle_frame[me]);
    default:
        say("unexpected vector", r->vector);
        halt(4);
    }
    /* Timer or signal: the running task goes to the back of the queue. */
    if (t) {
        t->sp = r;
        lock(&run_lock);
        t->state = READY;
        enqueue(t);
        unlock(&run_lock);
        current[me] = 0;
    } else {
        idle_frame[me] = r;
    }
    return schedule(me, idle_frame[me] ? idle_frame[me] : r);
}

/* ---- user state code. Nothing here may touch kernel functions but sys_call. */
static void user_exit(u64 v) { sys_call(SYS_EXIT, v); for (;;) {} }
static u64 sha_like(u64 seed, u64 rounds)
{
    u64 x = seed;
    for (u64 i = 0; i < rounds; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x += i; }
    return x;
}
#define WORK 40000UL
static void task_counter(u64 seed)
{
    u64 r = sha_like(seed, WORK);
    for (u64 i = 0; i < 20000; i++) { __atomic_fetch_add(&shared_count, 1, __ATOMIC_SEQ_CST); plain_count = plain_count + 1; }
    user_exit(r);
}
static void task_privileged(u64 seed) { (void)seed; user_privileged(); user_exit(1); }
static void task_badread(u64 seed) { (void)seed; volatile u64 *p = (u64 *)0x7000000000UL; user_exit(*p); }
static void task_divzero(u64 seed) { volatile i64 z = 0; user_exit((u64)((i64)seed / z)); }
static void task_print(u64 seed) { (void)seed; for (const char *s = "user says hello\n"; *s; s++) sys_call(SYS_PUT, (u64)(u8)*s); user_exit(0x77); }

static void new_task(int i, void (*fn)(u64), u64 arg)
{
    struct task *t = &tasks[i];
    struct regs *r = (struct regs *)(kstack[i] + STACK - sizeof(struct regs));
    u64 *us = (u64 *)(ustack[i] + STACK - 32);
    memset(r, 0, sizeof *r);
    us[0] = 0;        /* return address: a task never returns */
    us[1] = arg;      /* first argument, in its 8-byte slot */
    r->usp = (u64)us;
    r->sr = 0;        /* user state, interrupts on */
    r->format = 0;
    r->pc = (u64)fn;
    t->sp = r;
    t->state = READY;
    enqueue(t);
    __atomic_fetch_add(&live_tasks, 1, __ATOMIC_SEQ_CST);
}
static void *vectors[256];
static void set_vectors(void)
{
    static void (*const table[][2])(void) = { };
    (void)table;
    vectors[2] = vec_2; vectors[4] = vec_4; vectors[5] = vec_5; vectors[8] = vec_8; vectors[10] = vec_10; vectors[11] = vec_11;
    vectors[29] = vec_29; vectors[30] = vec_30; vectors[32] = vec_32; vectors[33] = vec_33;
}
static void core_loop(u64 me)
{
    set_timer(300);    /* microseconds, so that tasks really get preempted */
    irq_on();
    for (;;) {
        if (me == 0 && __atomic_load_n(&live_tasks, __ATOMIC_SEQ_CST) == 0) {
            irq_off();
            return;
        }
        idle_until_irq();
    }
}
extern void secondary_entry(void);
void kernel_main(void)
{
    u64 me = core_id();
    if (me != 0) halt(5);
    set_vectors();
    /* Core 1 starts through vector 30 with VBR taken from here, on the stack named here. */
    vectors[30] = secondary_entry;
    set_vbr(vectors);
    new_task(0, task_print, 0);
    new_task(1, task_privileged, 0);
    new_task(2, task_badread, 0);
    new_task(3, task_divzero, 12345);
    new_task(4, task_counter, 0x1234567);
    new_task(5, task_counter, 0x89ABCDE);
    new_task(6, task_counter, 0xF00D);
    new_task(7, task_counter, 0xBEEF);
    set_start_stack(idle_stack[1] + STACK);
    signal_cores(2);
    while (!__atomic_load_n(&core1_up, __ATOMIC_SEQ_CST)) {}
    core_loop(0);
    /* Results: a program compares them with the host's computation. */
    say("shared", shared_count);
    say("plain-lost", plain_count < shared_count);
    for (int i = 0; i < NTASKS; i++) say("task", tasks[i].result);
    say("ran-on-both", (tasks[4].ran_on | tasks[5].ran_on | tasks[6].ran_on | tasks[7].ran_on) == 3);
    say("preempted", tasks[4].slices + tasks[5].slices + tasks[6].slices + tasks[7].slices > 4);
    say("core1-ran", switches[1] > 0);
    say("ticks0", ticks[0] > 0);
    say("ticks1", ticks[1] > 0);
    say("faults", faults);
    halt(0);
}
void secondary_c(void)
{
    u64 me = core_id();
    set_vbr(vectors);
    vectors[30] = vec_30;
    __atomic_store_n(&core1_up, 1, __ATOMIC_SEQ_CST);
    core_loop(me);
    halt(6);
}
