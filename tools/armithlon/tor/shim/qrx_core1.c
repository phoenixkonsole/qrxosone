/* X25519 on the second 68100 core, as an optional helper: never required.
 *
 * Core 1 reaches shared RAM and its registers only: no ROM, no libraries, no interrupts. So
 * this file carries its own copy of curve25519-donna, whose calls all stay inside this file.
 *
 * qrx_core1_x25519 returns 0 when core 1 produced the result and -1 when the caller has to do
 * the work on core 0: helper not switched on, no second core, core 1 busy with another program
 * or another of our tasks, a failed self test, a fault or a time limit. After a fault, a time
 * limit or a failed self test the helper stays off until the program ends.
 *
 * Off unless the environment variable QRX_TOR_CORE1 is set. "log" also prints the job count on
 * exit. The keys sit in shared RAM either way; AROS has no memory protection between tasks.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define memcpy c1_memcpy
#define memset c1_memset
#define curve25519_donna c1_donna
static void *c1_memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
static void *c1_memset(void *d, int v, size_t n)
{
    unsigned char *a = d;
    while (n--) *a++ = (unsigned char)v;
    return d;
}
#include "src/ext/curve25519_donna/curve25519-donna.c"
#undef memcpy
#undef memset
#undef curve25519_donna

#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/time.h>
#include <proto/exec.h>
#include <exec/semaphores.h>

#define SMP_BASE 0xFF000600u
#define SMP_MAGIC 0x51525844u
#define STACK_BYTES 16384u
#define DONE 0x58323535u
#define LIMIT_US 2000000L

/* Whole 32-bit accesses: a tested bit would otherwise become a byte access. */
static inline uint32_t smp_rd(unsigned off)
{
    uint32_t v;
    __asm__ volatile("move.l %1,%0" : "=d"(v) : "m"(*(volatile uint32_t *)(SMP_BASE + off)));
    return v;
}
static inline void smp_wr(unsigned off, uint32_t v)
{
    __asm__ volatile("move.l %1,%0" : "=m"(*(volatile uint32_t *)(SMP_BASE + off)) : "d"(v));
}
static struct {
    uint8_t *out;
    const uint8_t *secret, *point;
    volatile uint32_t done;
} job;
static uint8_t c1_stack[STACK_BYTES] __attribute__((aligned(16)));
static struct SignalSemaphore lock;
static volatile int state;   /* 0 not looked at, 1 ready, -1 off */
static int started_once;
static unsigned long jobs_done, jobs_declined;

static void core1_entry(void)
{
    c1_donna(job.out, job.secret, job.point);
    job.done = DONE;
    smp_wr(0x18, 2);
    for (;;) {}
}
static long us_since(const struct timeval *t0)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec - t0->tv_sec) * 1000000L + (t.tv_usec - t0->tv_usec);
}
/* One job with the lock held. 0 on success. */
static int run_job(uint8_t *out, const uint8_t *secret, const uint8_t *point)
{
    struct timeval t0;
    if (smp_rd(0x0C) & 2u) return 1;   /* another program has the core */
    job.out = out; job.secret = secret; job.point = point; job.done = 0;
    __asm__ volatile("" ::: "memory");
    smp_wr(0x10, (uint32_t)(uintptr_t)core1_entry);
    smp_wr(0x14, (uint32_t)(uintptr_t)c1_stack + STACK_BYTES);
    smp_wr(0x18, started_once ? 3 : 1);  /* the first start flushes, this code does not change */
    started_once = 1;
    gettimeofday(&t0, NULL);
    while (job.done != DONE && !(smp_rd(0x0C) & 4u)) {
        if (!(smp_rd(0x0C) & 2u) && job.done != DONE) break;
        if (us_since(&t0) > LIMIT_US) { smp_wr(0x18, 2); state = -1; return -1; }
        sched_yield();
    }
    if (smp_rd(0x0C) & 4u || job.done != DONE) { state = -1; return -1; }
    memset(c1_stack, 0, STACK_BYTES);
    return 0;
}
static void report(void)
{
    printf("qrx_core1: %lu jobs on core 1, %lu left to core 0, state %d\n", jobs_done, jobs_declined, (int)state);
    fflush(stdout);
}
/* RFC 7748, section 6.1: Alice's public key from her private key, then the shared secret. */
static const uint8_t kat_secret[32] = {
    0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
    0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a };
static const uint8_t kat_public[32] = {
    0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
    0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a };
static const uint8_t kat_base[32] = { 9 };
static void look_once(void)
{
    const char *env;
    uint8_t out[32];
    Forbid();
    if (state) { Permit(); return; }
    state = -1;
    InitSemaphore(&lock);
    Permit();
    env = getenv("QRX_TOR_CORE1");
    if (!env || !*env) return;
    if (smp_rd(0) != SMP_MAGIC || !(smp_rd(8) & (1u << 4))) return;
    state = 2;  /* run_job may turn it off */
    ObtainSemaphore(&lock);
    if (run_job(out, kat_secret, kat_base) == 0 && !memcmp(out, kat_public, 32)) state = 1;
    else state = -1;
    ReleaseSemaphore(&lock);
    if (!strcmp(env, "log")) atexit(report);
}
int qrx_core1_x25519(uint8_t *out, const uint8_t *secret, const uint8_t *point)
{
    int r;
    if (!state) look_once();
    if (state != 1 || !AttemptSemaphore(&lock)) { jobs_declined++; return -1; }
    r = state == 1 ? run_job(out, secret, point) : -1;
    if (r == 0) jobs_done++; else jobs_declined++;
    ReleaseSemaphore(&lock);
    return r == 0 ? 0 : -1;
}
