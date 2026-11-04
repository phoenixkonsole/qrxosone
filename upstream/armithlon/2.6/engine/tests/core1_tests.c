/* The 68100's second core on a real second host thread: both cores run hand-assembled
 * 68040 code in shared RAM. Exact counters under a TAS spinlock and under CAS prove
 * atomicity and memory order; the timed loop proves the cores really run in parallel. */
#include "qrx68100_core1.h"
#include "qrx68100_jit.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAM_SIZE (8u * 1024u * 1024u)
#define SMP 0xFF000600u
#define STACK0 0x70000u
#define STACK1 0x60000u
#define LOCK 0x2000u
#define COUNTER 0x2004u
#define OUT 0x2008u
#define CODE 0x10000u
#define COUNT 1500000u
static QRXFullCPU c;
static unsigned failures;
#define CHECK(x, ...) do { if (!(x)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static uint32_t rd(uint32_t a) { uint32_t v = 0; qrx_full_bus_read(&c, a, 4, &v); return v; }
static void wr(uint32_t a, uint32_t v) { qrx_full_bus_write(&c, a, 4, v); }
static uint32_t here;
static void w16(unsigned v) { qrx_full_bus_write(&c, here, 2, v); here += 2; }
static void w32(uint32_t v) { w16(v >> 16); w16(v & 0xFFFFu); }
static void put_stop(void) { w16(0x4E72); w16(0x2700); }

static void tas_counter(uint32_t at, uint32_t n)
{
    here = at;
    w16(0x2E3C); w32(n);                       /* move.l #n,d7 */
    uint32_t loop = here;
    w16(0x4AF9); w32(LOCK);                    /* tas LOCK.l */
    w16(0x6B00 | ((loop - (here + 2)) & 0xFFu)); /* bmi.s loop */
    w16(0x52B9); w32(COUNTER);                 /* addq.l #1,COUNTER.l */
    w16(0x4239); w32(LOCK);                    /* clr.b LOCK.l */
    w16(0x5387);                               /* subq.l #1,d7 */
    w16(0x6600 | ((loop - (here + 2)) & 0xFFu)); /* bne.s loop */
    put_stop();
}
static void cas_counter(uint32_t at, uint32_t n)
{
    here = at;
    w16(0x2E3C); w32(n);                       /* move.l #n,d7 */
    uint32_t top = here;
    w16(0x2039); w32(COUNTER);                 /* move.l COUNTER.l,d0 */
    uint32_t retry = here;
    w16(0x2200);                               /* move.l d0,d1 */
    w16(0x5281);                               /* addq.l #1,d1 */
    w16(0x0EF9); w16(0x0040); w32(COUNTER);    /* cas.l d0,d1,COUNTER.l */
    w16(0x6600 | ((retry - (here + 2)) & 0xFFu)); /* bne.s retry */
    w16(0x5387);                               /* subq.l #1,d7 */
    w16(0x6600 | ((top - (here + 2)) & 0xFFu));  /* bne.s top */
    put_stop();
}
static void spin_loop(uint32_t at, uint32_t n)
{
    here = at;
    w16(0x203C); w32(n);                       /* move.l #n,d0 */
    w16(0x5380);                               /* subq.l #1,d0 */
    w16(0x66FC);                               /* bne.s -4 */
    put_stop();
}
/* Message passing: the writer stores DATA, then FLAG, on separate cache lines, so a reader
 * that sees FLAG = n must see DATA >= n. Counts the violations into VIOL. */
#define DATA 0x3000u
#define FLAG 0x3400u
#define VIOL 0x3800u
static void mp_writer(uint32_t at, uint32_t n)
{
    here = at;
    w16(0x7000);                               /* moveq #0,d0 */
    w16(0x2E3C); w32(n);                       /* move.l #n,d7 */
    uint32_t loop = here;
    w16(0x5280);                               /* addq.l #1,d0 */
    w16(0x23C0); w32(DATA);                    /* move.l d0,DATA.l */
    w16(0x23C0); w32(FLAG);                    /* move.l d0,FLAG.l */
    w16(0x5387);                               /* subq.l #1,d7 */
    w16(0x6600 | ((loop - (here + 2)) & 0xFFu));
    put_stop();
}
static void mp_reader(uint32_t at, uint32_t n)
{
    here = at;
    w16(0x7400);                               /* moveq #0,d2: violations */
    w16(0x2E3C); w32(n);                       /* move.l #n,d7 */
    uint32_t loop = here;
    w16(0x2039); w32(FLAG);                    /* move.l FLAG.l,d0 */
    w16(0x2239); w32(DATA);                    /* move.l DATA.l,d1 */
    w16(0xB280);                               /* cmp.l d0,d1 */
    w16(0x6402);                               /* bcc.s +2 */
    w16(0x5282);                               /* addq.l #1,d2 */
    w16(0x5387);                               /* subq.l #1,d7 */
    w16(0x6600 | ((loop - (here + 2)) & 0xFFu));
    w16(0x23C2); w32(VIOL);                    /* move.l d2,VIOL.l */
    put_stop();
}
static void start_core1(uint32_t pc, uint32_t ctl)
{
    wr(SMP + 0x10, pc);
    wr(SMP + 0x14, STACK1);
    wr(SMP + 0x18, ctl);
}
static bool core1_busy(void) { return (rd(SMP + 0x0C) & 2u) != 0; }
static bool wait_core1(void)
{
    double end = now() + 60.0;
    while (core1_busy()) if (now() > end) return false;
    return true;
}
static void run_core0(uint32_t pc)
{
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_A7, STACK0);
    m68k_set_reg(M68K_REG_PC, pc);
    double end = now() + 60.0;
    while (!qrx_jit_cpu_stopped()) {
        c.jit_exec(&c, 500000);
        if (now() > end) { puts("FAIL: core 0 job did not finish (lost atomicity?)"); exit(1); }
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!qrx_full_init(&c, RAM_SIZE, 68040)) { puts("init failed"); return 2; }
    wr(0, STACK0); wr(4, CODE);
    if (!qrx_full_reset(&c)) { puts("reset failed"); return 2; }
    if (!qrx_jit_init(&c, 0)) {
#if defined(__aarch64__)
        puts("JIT unavailable"); return 2;
#else
        puts("SKIP: not an AArch64 host"); return 0;
#endif
    }
    if (!qrx_core1_attach(&c)) { puts("attach failed"); return 2; }
    CHECK(rd(SMP) == 0x51525844u, "magic %08x", rd(SMP));
    CHECK(rd(SMP + 0x0C) == 1u, "only core 0 online at power-on: %x", rd(SMP + 0x0C));

    /* Identity: the same register answers differently on each core. */
    here = CODE;
    w16(0x23F9); w32(SMP + 0x34); w32(OUT);    /* move.l CORE_ID,OUT.l */
    put_stop();
    wr(OUT, 0xDEAD);
    start_core1(CODE, 1);
    CHECK(wait_core1(), "identity job did not finish");
    CHECK(rd(OUT) == 1u, "core 1 id %u", rd(OUT));
    run_core0(CODE);
    CHECK(rd(OUT) == 0u, "core 0 id %u", rd(OUT));

    /* TAS spinlock guarding a plain memory increment. */
    tas_counter(CODE + 0x100, COUNT);
    wr(LOCK, 0); wr(COUNTER, 0);
    start_core1(CODE + 0x100, 1);
    run_core0(CODE + 0x100);
    CHECK(wait_core1(), "TAS job did not finish");
    CHECK(rd(COUNTER) == 2u * COUNT, "TAS counter %u, expected %u", rd(COUNTER), 2u * COUNT);
    printf("TAS spinlock: %u increments, exact\n", rd(COUNTER));

    /* Lock-free CAS.L. */
    cas_counter(CODE + 0x200, COUNT);
    wr(COUNTER, 0);
    start_core1(CODE + 0x200, 1);
    run_core0(CODE + 0x200);
    CHECK(wait_core1(), "CAS job did not finish");
    CHECK(rd(COUNTER) == 2u * COUNT, "CAS counter %u, expected %u", rd(COUNTER), 2u * COUNT);
    printf("CAS.L: %u increments, exact\n", rd(COUNTER));

    /* Memory order, tried a few times because a violation is a matter of timing. */
    mp_writer(CODE + 0x900, 300000000u);
    mp_reader(CODE + 0xA00, 300000000u);
    wr(DATA, 0); wr(FLAG, 0); wr(VIOL, 0xFFFFFFFFu);
    start_core1(CODE + 0xA00, 1);
    run_core0(CODE + 0x900);
    CHECK(wait_core1(), "reader did not finish");
    printf("message passing: %u order violations\n", rd(VIOL));
    CHECK(rd(VIOL) == 0u, "order violations %u", rd(VIOL));

    /* Unaligned long and word accesses are legal on a 68040 but not on ordered AArch64
     * loads and stores. Page-crossing ones must work as well. */
    here = CODE + 0xB00;
    w16(0x207C); w32(OUT + 0x101);             /* movea.l #odd,a0 */
    w16(0x20BC); w32(0x11223344u);             /* move.l #$11223344,(a0) */
    w16(0x2010);                               /* move.l (a0),d0 */
    w16(0x31FC); w16(0x5566); w16(0x2201);     /* move.w #$5566,$2201.w */
    w16(0x3238); w16(0x2201);                  /* move.w $2201.w,d1 */
    w16(0x207C); w32(0x3FFEu);                 /* movea.l #page end - 2,a0 */
    w16(0x20BC); w32(0xA1B2C3D4u);             /* move.l #$a1b2c3d4,(a0) */
    w16(0x2410);                               /* move.l (a0),d2 */
    w16(0x23C0); w32(0x2300);
    w16(0x33C1); w32(0x2304);
    w16(0x23C2); w32(0x2308);
    put_stop();
    for (unsigned core = 0; core < 2; core++) {
        wr(0x2300, 0); wr(0x2304, 0); wr(0x2308, 0);
        if (core) { start_core1(CODE + 0xB00, 1); CHECK(wait_core1(), "unaligned job did not finish"); }
        else run_core0(CODE + 0xB00);
        CHECK(!(rd(SMP + 0x0C) & 4u), "core %u faulted at %08x", core, rd(SMP + 0x38));
        CHECK(rd(0x2300) == 0x11223344u, "core %u unaligned long %08x", core, rd(0x2300));
        CHECK(rd(0x2304) >> 16 == 0x5566u, "core %u unaligned word %08x", core, rd(0x2304));
        CHECK(rd(0x2308) == 0xA1B2C3D4u, "core %u page-crossing long %08x", core, rd(0x2308));
    }

    /* Parallel speed: two loops take about as long as one. */
    uint32_t n = 1500000000u;
    spin_loop(CODE + 0x300, n);
    run_core0(CODE + 0x300);                   /* warm the translation */
    /* The best of a few tries: a busy host must not fail this. */
    double speedup = 0;
    for (int attempt = 0; attempt < 4 && speedup <= 1.5; attempt++) {
        double t0 = now();
        run_core0(CODE + 0x300);
        double one = now() - t0;
        t0 = now();
        start_core1(CODE + 0x300, 3);
        run_core0(CODE + 0x300);
        CHECK(wait_core1(), "parallel job did not finish");
        double both = now() - t0;
        if (2.0 * one / both > speedup) speedup = 2.0 * one / both;
        printf("parallel: one loop %.3f s, both %.3f s, speedup %.2fx, core 1 ran %u instructions\n", one, both, 2.0 * one / both, rd(SMP + 0x28));
    }
    CHECK(speedup > 1.5, "speedup %.2f", speedup);
    CHECK(rd(SMP + 0x28) >= 2u * n - 100u, "core 1 instructions %u", rd(SMP + 0x28));

    /* Without a flush core 1 keeps what it translated, with one it sees new code. */
    here = CODE + 0x400;
    w16(0x23FC); w32(1); w32(OUT);             /* move.l #1,OUT.l */
    put_stop();
    start_core1(CODE + 0x400, 1);
    CHECK(wait_core1(), "code job did not finish");
    CHECK(rd(OUT) == 1u, "first code %u", rd(OUT));
    wr(CODE + 0x402, 2);                       /* new immediate */
    start_core1(CODE + 0x400, 1);
    CHECK(wait_core1(), "code job 2 did not finish");
    CHECK(rd(OUT) == 2u, "flushed code wrote %u", rd(OUT));

    /* A guest exception parks core 1 and says where. */
    here = CODE + 0x500;
    w16(0x4E71);
    uint32_t bad = here;
    w16(0x4AFC);                               /* illegal */
    start_core1(CODE + 0x500, 1);
    CHECK(wait_core1(), "illegal job did not finish");
    CHECK(rd(SMP + 0x0C) & 4u, "fault status %x", rd(SMP + 0x0C));
    CHECK(rd(SMP + 0x38) == bad, "fault pc %08x, expected %08x", rd(SMP + 0x38), bad);
    CHECK(rd(SMP + 0x3C) == 4u, "vector %u", rd(SMP + 0x3C));

    /* Chipset addresses are outside core 1's bus. */
    here = CODE + 0x600;
    w16(0x33FC); w16(0x0F00); w32(0x00DFF180); /* move.w #$f00,COLOR00 */
    put_stop();
    start_core1(CODE + 0x600, 1);
    CHECK(wait_core1(), "chipset job did not finish");
    CHECK(rd(SMP + 0x0C) & 4u, "chipset write must fault");
    CHECK(rd(SMP + 0x3C) == 0x00DFF180u, "fault address %08x", rd(SMP + 0x3C));

    /* A job can park itself, and a later job clears the fault. */
    here = CODE + 0x700;
    w16(0x23FC); w32(2); w32(SMP + 0x18);      /* move.l #2,CTL */
    w16(0x60FE);                               /* bra.s self: never reached in practice */
    start_core1(CODE + 0x700, 1);
    CHECK(wait_core1(), "self-parking job did not finish");
    CHECK(rd(SMP + 0x0C) == 1u, "status after park %x", rd(SMP + 0x0C));

    /* Core 0 can stop a job that never ends. */
    here = CODE + 0x800;
    w16(0x60FE);                               /* bra.s self */
    start_core1(CODE + 0x800, 1);
    wr(SMP + 0x18, 2);
    CHECK(wait_core1(), "endless job could not be parked");

    qrx_core1_detach(&c);
    qrx_jit_destroy(&c);
    qrx_full_destroy(&c);
    if (failures) { printf("%u failures\n", failures); return 1; }
    puts("core 1: all checks passed");
    return 0;
}
