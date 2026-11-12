/* 68100EC64 long mode: the rules in docs/armithlon/68100ec64.md, a program assembled by the
 * stock assembler with code, data and stack above 4 GB, and two cores sharing a counter. */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define W(...) (const uint16_t[]){ __VA_ARGS__ }, sizeof((const uint16_t[]){ __VA_ARGS__ }) / 2
#define Q64(v) (uint16_t)((uint64_t)(v) >> 48), (uint16_t)((uint64_t)(v) >> 32), (uint16_t)((uint64_t)(v) >> 16), (uint16_t)(v)
#define RAM_SIZE (UINT64_C(6) << 30)
#define HIGH UINT64_C(0x120000000)
static uint8_t *ram;
static qec64_cpu c;
static unsigned failures;
#define CHECK(x, ...) do { if (!(x)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)

static void put16(uint64_t a, uint16_t v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }
static uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | ram[a + i]; return v; }
static void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; i++) ram[a + i] = (uint8_t)(v >> (56 - i * 8)); }
/* Runs the words at a high address, in long mode, up to a stop. */
static enum qec64_status run(const uint16_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++) put16(HIGH + i * 2, w[i]);
    put16(HIGH + n * 2, 0x4AFC);
    c.pc = HIGH;
    return qec64_run(&c, 100000);
}
static void fresh(void)
{
    qec64_init(&c, ram, RAM_SIZE, 0);
    c.m64 = true;
    c.a[7] = UINT64_C(0x130000000);
}
static void rules(void)
{
    enum qec64_status s;
    fresh();
    s = run(W(0xA008, 0x203C, Q64(0x123456789ABCDEF0)));
    CHECK(s == QEC64_STOPPED && c.d[0] == UINT64_C(0x123456789ABCDEF0), "MOVE.Q #imm64 %d %" PRIx64, s, c.d[0]);
    fresh(); c.d[0] = UINT64_MAX;
    run(W(0x203C, 0xFFFF, 0xFFFF));
    CHECK(c.d[0] == UINT64_C(0xFFFFFFFF), ".L zero extends: %" PRIx64, c.d[0]);
    fresh(); c.d[0] = UINT64_MAX;
    run(W(0x103C, 0x0001));
    CHECK(c.d[0] == UINT64_C(0xFFFFFFFFFFFFFF01), ".B merges: %" PRIx64, c.d[0]);
    fresh(); run(W(0x70FF));
    CHECK(c.d[0] == UINT64_MAX, "MOVEQ sign extends: %" PRIx64, c.d[0]);
    fresh(); run(W(0x207C, 0xFFFF, 0xFFFF));
    CHECK(c.a[0] == UINT64_MAX, "MOVEA.L sign extends: %" PRIx64, c.a[0]);
    fresh(); run(W(0xA008, 0x207C, Q64(0x8000000000000000)));
    CHECK(c.a[0] == UINT64_C(0x8000000000000000), "MOVEA.Q: %" PRIx64, c.a[0]);
    /* flags at 64 bits */
    fresh(); c.d[0] = UINT64_MAX; c.d[1] = 1; run(W(0xA008, 0xD081));
    CHECK(c.d[0] == 0 && c.ccr == 0x15, "ADD.Q carry: %" PRIx64 " ccr %02x", c.d[0], c.ccr);
    fresh(); c.d[0] = INT64_MAX; c.d[1] = 1; run(W(0xA008, 0xD081));
    CHECK(c.d[0] == UINT64_C(0x8000000000000000) && c.ccr == 0x0A, "ADD.Q overflow: ccr %02x", c.ccr);
    fresh(); c.d[0] = 0; c.d[1] = 1; run(W(0xA008, 0x9081));
    CHECK(c.d[0] == UINT64_MAX && c.ccr == 0x19, "SUB.Q borrow: ccr %02x", c.ccr);
    fresh(); c.d[0] = UINT64_C(0x100000000); c.d[1] = 1; run(W(0xA008, 0xB081));
    CHECK((c.ccr & 0x0F) == 0, "CMP.Q compares the high half: ccr %02x", c.ccr);
    fresh(); c.d[0] = UINT64_C(0x100000000); c.d[1] = 1; run(W(0xB081));
    CHECK((c.ccr & 0x09) == 0x09, "CMP.L ignores it: ccr %02x", c.ccr);
    /* the N prefix: MOVE without touching the flags, and nothing else takes it */
    fresh(); c.d[1] = 0; c.ccr = 0x1F; run(W(0xA018, 0x2001));
    CHECK(c.ccr == 0x1F && c.d[0] == 0, "N prefix keeps the flags: %02x", c.ccr);
    fresh(); c.d[1] = 0; c.ccr = 0x1F; run(W(0xA008, 0x2001));
    CHECK(c.ccr == 0x14, "MOVE without it sets them: %02x", c.ccr);
    fresh(); s = run(W(0xA010, 0xD081));
    CHECK(s == QEC64_UNIMPLEMENTED, "N prefix on ADD: %d", s);
    /* PC relative destinations */
    /* MOVE.L D0,(d16,PC): the destination field of MOVE is register 2, mode 7 */
    fresh(); c.d[0] = 0x55;
    { const uint16_t p[] = { 0x25C0, 0x0100, 0x4AFC };
      for (size_t i = 0; i < 3; i++) put16(HIGH + 0x7000 + i * 2, p[i]);
      c.pc = HIGH + 0x7000;
      s = qec64_run(&c, 10);
      CHECK(s == QEC64_STOPPED && (uint32_t)get64(HIGH + 0x7102 - 4) == 0x55, "MOVE to (d16,PC): %d", s); }
    fresh(); c.m64 = false; c.a[7] = 0x20000; c.pc = 0x10000; put16(0x10000, 0x25C0); put16(0x10002, 0x0100);
    s = qec64_run(&c, 1);
    CHECK(s == QEC64_UNIMPLEMENTED, "PC relative destination outside long mode: %d", s);
    /* registers D8 to D15 and A8 to A15 */
    fresh(); run(W(0xA00C, 0x203C, Q64(0x1111222233334444)));
    CHECK(c.d[8] == UINT64_C(0x1111222233334444) && c.d[0] == 0, "R prefix on MOVE: d8 %" PRIx64, c.d[8]);
    fresh(); c.d[8] = 77; run(W(0xA009, 0x2000));
    CHECK(c.d[0] == 77, "B prefix: %" PRIx64, c.d[0]);
    fresh(); c.a[9] = HIGH + 0x1000; put64(HIGH + 0x1000, 99); run(W(0xA009, 0x2011));
    CHECK(c.d[0] == 99, "(A9): %" PRIx64, c.d[0]);
    /* wide versions */
    fresh(); c.d[0] = UINT64_C(0xFFFFFFFF80000000); run(W(0xA008, 0x48C0));
    CHECK(c.d[0] == UINT64_C(0xFFFFFFFF80000000), "EXT.Q: %" PRIx64, c.d[0]);
    fresh(); c.d[0] = UINT64_C(0x1122334455667788); run(W(0xA008, 0x4840));
    CHECK(c.d[0] == UINT64_C(0x5566778811223344), "SWAP.Q: %" PRIx64, c.d[0]);
    fresh(); c.d[0] = 1; run(W(0xA008, 0xE188));
    CHECK(c.d[0] == 256, "LSL.Q: %" PRIx64, c.d[0]);
    fresh(); c.d[0] = UINT64_C(0x8000000000000000); c.d[1] = 63; run(W(0xA008, 0xE2A8));
    CHECK(c.d[0] == 1, "LSR.Q by 63: %" PRIx64, c.d[0]);
    fresh(); c.d[0] = UINT64_C(0x100000000); c.d[1] = UINT64_C(0x100000000); run(W(0xA008, 0x4C01, 0x0401));
    CHECK(c.d[0] == 0 && c.d[1] == 1, "MULU.Q high half: %" PRIx64 ":%" PRIx64, c.d[1], c.d[0]);
    fresh(); c.d[0] = 100; c.d[1] = 7; run(W(0xA008, 0x4C41, 0x0000));
    CHECK(c.d[0] == 14, "DIVU.Q: %" PRIx64, c.d[0]);
    /* the stack holds 8-byte addresses, also above 4 GB */
    fresh();
    s = run(W(0x6100, 0x0002, 0x4AFC, 0x4AFC));
    CHECK(s == QEC64_STOPPED && c.a[7] == UINT64_C(0x130000000) - 8 && get64(c.a[7]) == HIGH + 4, "BSR pushes 8: sp %" PRIx64, c.a[7]);
    fresh(); c.a[0] = HIGH + 0x2000; c.a[1] = HIGH + 0x3000; run(W(0x41D1));
    CHECK(c.a[0] == HIGH + 0x3000, "LEA (A1) keeps 64 bits: %" PRIx64, c.a[0]);
    /* d8(An,Xn.L): the whole index register */
    fresh(); c.a[0] = HIGH; c.d[1] = UINT64_C(0x10000000); put64(HIGH + 0x10000000, 5); run(W(0xA008, 0x2030, 0x1800));
    CHECK(c.d[0] == 5, "index register at 64 bits: %" PRIx64, c.d[0]);
    /* atomics */
    fresh(); c.a[0] = HIGH + 0x4000; put64(HIGH + 0x4000, 10); c.d[0] = 10; c.d[1] = 11; run(W(0xA008, 0x0ED0, 0x0040));
    CHECK(get64(HIGH + 0x4000) == 11 && (c.ccr & 4), "CAS.Q taken");
    fresh(); c.a[0] = HIGH + 0x4000; c.d[0] = 10; c.d[1] = 12; run(W(0xA008, 0x0ED0, 0x0040));
    CHECK(get64(HIGH + 0x4000) == 11 && !(c.ccr & 4) && c.d[0] == 11, "CAS.Q not taken: %" PRIx64, c.d[0]);
    /* edges */
    fresh(); c.a[0] = RAM_SIZE - 4;
    s = run(W(0xA008, 0x2010));
    CHECK(s == QEC64_BUS_FAULT && c.fault_addr == RAM_SIZE - 4, "beyond RAM: %d %" PRIx64, s, c.fault_addr);
    fresh(); c.m64 = false; c.pc = 0;
    put16(0x10000, 0xA008); put16(0x10002, 0x4E71); c.pc = 0x10000;
    s = qec64_run(&c, 1);
    CHECK(s == QEC64_UNIMPLEMENTED, "prefix outside long mode: %d", s);
    /* switching modes */
    qec64_init(&c, ram, RAM_SIZE, 3);
    { const uint16_t p[] = { 0x7001, 0x4E7B, 0x080F, 0x4E7A, 0x180E, 0x4AFC };
      for (size_t i = 0; i < 6; i++) put16(0x10000 + i * 2, p[i]);
      c.pc = 0x10000;
      s = qec64_run(&c, 10);
      CHECK(s == QEC64_STOPPED && c.m64 && c.d[1] == 3, "MOVEC long mode, core id: %d %" PRIx64, s, c.d[1]); }
}
/* Exceptions: a vector table above 4 GB, handlers as code words. */
#define VBR UINT64_C(0x128000000)
#define HANDLER UINT64_C(0x129000000)
#define XSTACK UINT64_C(0x130000000)
static void set_vector(unsigned v, const uint16_t *w, size_t n, unsigned slot)
{
    uint64_t at = HANDLER + slot * 0x100;
    put64(VBR + v * 8, at);
    for (size_t i = 0; i < n; i++) put16(at + i * 2, w[i]);
}
#define VEC(v, slot, ...) set_vector(v, (const uint16_t[]){ __VA_ARGS__ }, sizeof((const uint16_t[]){ __VA_ARGS__ }) / 2, slot)
static void xfresh(void)
{
    fresh();
    c.vbr = VBR; c.vbr_set = true;
    c.ipl = 0;
    c.a[7] = XSTACK;
}
/* moveq #n,d7 ; rte */
#define H_RTE(n) (uint16_t)(0x7E00 | (n)), 0x4E73
/* moveq #n,d7 ; movec d7,$8F1: ends the run */
#define H_HALT(n) (uint16_t)(0x7E00 | (n)), 0x4E7B, 0x78F1
static uint64_t xrun(const uint16_t *w, size_t n, enum qec64_status *st)
{
    for (size_t i = 0; i < n; i++) put16(HIGH + i * 2, w[i]);
    c.pc = HIGH;
    *st = qec64_run(&c, 100000);
    return c.d[7];
}
#define XRUN(st, ...) xrun((const uint16_t[]){ __VA_ARGS__ }, sizeof((const uint16_t[]){ __VA_ARGS__ }) / 2, &(st))
#define END 0x4E7B, 0x08F1   /* movec d0,$8F1 */
static void exceptions(void)
{
    enum qec64_status st;
    uint64_t r;
    /* TRAP returns to the next instruction, with an exact frame */
    xfresh(); VEC(35, 0, H_RTE(9));
    r = XRUN(st, 0x4E43, 0x7001, END);
    CHECK(st == QEC64_HALTED && r == 9 && c.d[0] == 1 && c.a[7] == XSTACK, "TRAP: %d d7 %" PRIu64 " sp %" PRIx64, st, r, c.a[7]);
    CHECK(get64(XSTACK - 8) == HIGH + 2, "TRAP frame PC %" PRIx64, get64(XSTACK - 8));
    CHECK(ram[XSTACK - 14] == 0 && ram[XSTACK - 13] == 35 * 4, "TRAP frame vector %02x%02x", ram[XSTACK - 14], ram[XSTACK - 13]);
    /* a fault reports the instruction that faulted, and the address */
    xfresh(); VEC(2, 0, H_HALT(2)); c.a[0] = RAM_SIZE + 16;
    r = XRUN(st, 0x7001, 0x2010, 0x7002, END);
    CHECK(st == QEC64_HALTED && r == 2 && c.a[7] == XSTACK - 24, "bus fault: %d sp %" PRIx64, st, c.a[7]);
    CHECK(get64(XSTACK - 16) == HIGH + 2 && get64(XSTACK - 8) == RAM_SIZE + 16, "bus fault frame %" PRIx64 " %" PRIx64, get64(XSTACK - 16), get64(XSTACK - 8));
    CHECK(ram[XSTACK - 22] == 0x10, "bus fault format %02x", ram[XSTACK - 22]);
    /* illegal, line A with a reserved prefix, line F */
    xfresh(); VEC(4, 0, H_HALT(4)); r = XRUN(st, 0x4AFC);
    CHECK(r == 4 && st == QEC64_HALTED && get64(XSTACK - 8) == HIGH, "ILLEGAL: %d %" PRIu64, st, r);
    xfresh(); VEC(10, 0, H_HALT(10)); r = XRUN(st, 0xA020, 0x4E71);
    CHECK(r == 10 && st == QEC64_HALTED, "reserved prefix: %d %" PRIu64, st, r);
    xfresh(); VEC(11, 0, H_HALT(11)); r = XRUN(st, 0xF000, 0x4E71);
    CHECK(r == 11 && st == QEC64_HALTED, "line F: %d %" PRIu64, st, r);
    xfresh(); VEC(5, 0, H_HALT(5)); r = XRUN(st, 0x7000, 0x80C0);
    CHECK(r == 5 && st == QEC64_HALTED && get64(XSTACK - 8) == HIGH + 4, "divide by zero: %d %" PRIx64, st, get64(XSTACK - 8));
    /* user state: separate stack, no privileged instructions */
    xfresh(); VEC(8, 0, H_HALT(8));
    c.supervisor = false; c.a[7] = XSTACK - 0x1000; c.other_sp = XSTACK;
    r = XRUN(st, 0x4E71, 0x4E72, 0x2000);
    CHECK(r == 8 && st == QEC64_HALTED && c.supervisor, "privilege: %d %" PRIu64, st, r);
    CHECK(c.a[7] == XSTACK - 16 && c.other_sp == XSTACK - 0x1000, "stacks changed: %" PRIx64 " %" PRIx64, c.a[7], c.other_sp);
    CHECK(get64(XSTACK - 8) == HIGH + 2 && !(ram[XSTACK - 16] & 0x20), "privilege frame %" PRIx64, get64(XSTACK - 8));
    /* RTE back into user state: frame on the stack, then a TRAP from there */
    xfresh(); VEC(32, 0, H_HALT(7));
    c.other_sp = XSTACK - 0x1000;
    put16(XSTACK - 16, 0x0000); put16(XSTACK - 14, 0); put64(XSTACK - 8, HIGH + 2);
    c.a[7] = XSTACK - 16;
    r = XRUN(st, 0x4E73, 0x4E40);
    CHECK(r == 7 && st == QEC64_HALTED && c.other_sp == XSTACK - 0x1000 && c.a[7] == XSTACK - 16, "RTE to user: %d %" PRIx64, st, c.other_sp);
    /* MOVE USP and MOVE SR */
    xfresh(); c.a[1] = 0x1234; r = XRUN(st, 0x4E61, 0x4E6A, 0x40C3, 0x7001, END);
    CHECK(st == QEC64_HALTED && c.a[2] == 0x1234 && (c.d[3] & 0xFFFF) == 0x2000, "MOVE USP, MOVE SR: %" PRIx64 " %" PRIx64, c.a[2], c.d[3]);
    /* ORI to CCR, ANDI to SR */
    xfresh(); r = XRUN(st, 0x003C, 0x0011, 0x027C, 0xF8FF, 0x7001, END);
    CHECK(st == QEC64_HALTED && c.ipl == 0, "ORI CCR, ANDI SR: %d", st);
    /* interrupts: masked, then taken by level, then the top level always */
    xfresh(); VEC(29, 0, H_RTE(5)); c.ipl = 5;
    qec64_raise(&c, 5);
    r = XRUN(st, 0x4E71, 0x7001, END);
    CHECK(st == QEC64_HALTED && c.d[7] == 0, "level 5 masked at 5: d7 %" PRIu64, c.d[7]);
    c.pending = 0;
    xfresh(); VEC(29, 0, H_RTE(5)); c.ipl = 4;
    qec64_raise(&c, 5);
    r = XRUN(st, 0x4E71, 0x7001, END);
    CHECK(st == QEC64_HALTED && r == 5 && c.ipl == 4 && c.a[7] == XSTACK, "level 5 taken: %d d7 %" PRIu64 " ipl %u", st, r, c.ipl);
    xfresh(); VEC(31, 0, H_RTE(3)); c.ipl = 7;
    qec64_raise(&c, 7);
    r = XRUN(st, 0x4E71, 0x7001, END);
    CHECK(st == QEC64_HALTED && r == 3, "level 7 through the mask: %d", st);
    /* STOP waits, an interrupt ends the wait and RTE goes on after it */
    xfresh(); VEC(30, 0, H_RTE(6));
    r = XRUN(st, 0x4E72, 0x2000, 0x7001, END);
    CHECK(st == QEC64_WAITING && c.waiting, "STOP waits: %d", st);
    qec64_raise(&c, 6);
    st = qec64_run(&c, 100);
    CHECK(st == QEC64_HALTED && c.d[7] == 6 && c.d[0] == 1, "woken: %d d7 %" PRIu64, st, c.d[7]);
    /* no stack for the frame */
    xfresh(); c.a[7] = 8; r = XRUN(st, 0x4AFC);
    CHECK(st == QEC64_DOUBLE_FAULT, "no stack: %d", st);
    /* without VBR a core still stops and reports, as all tests above that one rely on */
    fresh();
    { enum qec64_status s2 = run(W(0x4E71)); CHECK(s2 == QEC64_STOPPED, "no VBR: %d", s2); }
}
#define COUNTER UINT64_C(0x150000000)
/* Two cores on the machine: core 0 starts core 1 and core 1 signals back; both timers run. */
static void machine(void)
{
    qec64_machine *m = qec64_machine_new(ram, RAM_SIZE, 2);
    char out[64];
    unsigned who = 9;
    qec64_machine_console(m, out, sizeof out);
    memset(ram + COUNTER, 0, 64);
    put64(VBR + 30 * 8, HANDLER);       /* IPI */
    put64(VBR + 29 * 8, HANDLER + 0x100); /* timer */
    /* IPI handler: core 1 writes 'B' and signals core 0; core 0 ends the run.
       movec $80E,d1; tst.l d1; beq.s done; moveq #66,d2; movec d2,$8F0; moveq #1,d2; movec d2,$80D; stop; bra back (its frame has no place to return to)
       done: moveq #65,d2; movec d2,$8F0; moveq #0,d0; movec d0,$8F1 */
    { const uint16_t h[] = { 0x4E7A, 0x180E, 0x4A81, 0x6712, 0x7442, 0x4E7B, 0x28F0, 0x7401, 0x4E7B, 0x280D, 0x4E72, 0x2000, 0x60FA,
                             0x7441, 0x4E7B, 0x28F0, 0x7000, 0x4E7B, 0x08F1 };
      for (size_t i = 0; i < sizeof h / 2; i++) put16(HANDLER + i * 2, h[i]); }
    /* Timer handler: addq.l #1,(a4); rte. A4 holds the counter on core 0 only. */
    put16(HANDLER + 0x100, 0x5294); put16(HANDLER + 0x102, 0x4E73);
    /* Reset code: long mode, VBR, a 1 ms timer, wait for 5 ticks, start core 1, wait. */
    { const uint16_t b[] = {
        0x7001, 0x4E7B, 0x080F,                         /* moveq #1,d0; movec d0,$80F */
        0xA008, 0x207C, Q64(VBR),                       /* movea.q #VBR,a0 */
        0x4E7B, 0x8801,                                 /* movec a0,$801 */
        0xA008, 0x287C, Q64(COUNTER),                   /* movea.q #COUNTER,a4 */
        0x46FC, 0x2000,                                 /* move #$2000,sr */
        0x203C, 0x0000, 0x03E8, 0x4E7B, 0x080C,         /* move.l #1000,d0; movec d0,$80C */
        0x0C94, 0x0000, 0x0005, 0x6500, 0xFFF8,         /* cmpi.l #5,(a4); bcs.w back */
        0xA008, 0x247C, Q64(XSTACK + 0x10000),          /* movea.q #stack,a2 */
        0x4E7B, 0xA80A,                                 /* movec a2,$80A */
        0x7002, 0x4E7B, 0x080D,                         /* moveq #2,d0; movec d0,$80D */
        0x4E72, 0x2000, 0x60FA };                       /* stop #$2000; bra back */
      for (size_t i = 0; i < sizeof b / 2; i++) put16(0x10000 + i * 2, b[i]); }   /* below 4 GB: reset is in 32-bit mode */
    enum qec64_status st = qec64_machine_run(m, 0x10000, 0x20000, 5.0, &who);
    size_t used = qec64_machine_console_used(m);
    CHECK(st == QEC64_HALTED && who == 0, "machine ended with %d on core %u, pc %" PRIx64 " %" PRIx64, st, who, qec64_machine_core(m, 0)->pc, qec64_machine_core(m, 1)->pc);
    CHECK(used == 2 && out[0] == 'B' && out[1] == 'A', "console %zu: %.*s", used, (int)used, out);
    CHECK((uint32_t)(get64(COUNTER) >> 32) >= 5, "timer ticks %" PRIu64, get64(COUNTER) >> 32);
    printf("machine: two cores, console %.*s, %" PRIu64 " timer ticks\n", (int)used, out, get64(COUNTER) >> 32);
    qec64_machine_free(m);
}
static uint64_t host_fnv(const uint8_t *p, uint64_t n)
{
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    while (n--) { h ^= *p++; h *= UINT64_C(0x100000001B3); }
    return h;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static void assembled_program(void)
{
    static const unsigned char code[] = {
#include "ec64_fnv64.inc"
    };
    const uint64_t data = UINT64_C(0x140000000), n = 1 << 20;
    for (uint64_t i = 0; i < n; i++) ram[data + i] = (uint8_t)(i * 2654435761u >> 13);
    memcpy(ram + HIGH, code, sizeof code);
    fresh();
    c.a[0] = data; c.d[1] = n; c.pc = HIGH;
    double t0 = now();
    enum qec64_status s = qec64_run(&c, UINT64_C(1) << 34);
    double t = now() - t0;
    uint64_t want = host_fnv(ram + data, n);
    CHECK(s == QEC64_STOPPED && c.d[0] == want, "FNV-1a: %d got %" PRIx64 " want %" PRIx64, s, c.d[0], want);
    printf("assembled program above 4 GB: hash %016" PRIx64 ", %" PRIu64 " instructions, %.0f million per second\n", c.d[0], c.instructions, (double)c.instructions / t / 1e6);
}
#define ROUNDS 1000000u
struct job { qec64_cpu cpu; uint64_t code; };
static void *core_main(void *arg)
{
    struct job *j = arg;
    qec64_run(&j->cpu, UINT64_C(1) << 36);
    return NULL;
}
static uint64_t two_cores(const uint16_t *w, size_t n, enum qec64_status *st)
{
    const uint64_t code = HIGH + 0x8000, counter = UINT64_C(0x150000000);
    struct job j[2];
    pthread_t t[2];
    for (size_t i = 0; i < n; i++) put16(code + i * 2, w[i]);
    put64(counter, 0);
    for (unsigned i = 0; i < 2; i++) {
        qec64_init(&j[i].cpu, ram, RAM_SIZE, i);
        j[i].cpu.m64 = true;
        j[i].cpu.a[0] = counter; j[i].cpu.a[7] = UINT64_C(0x130000000) + i * 0x10000;
        j[i].cpu.d[7] = ROUNDS; j[i].cpu.pc = code;
        pthread_create(&t[i], NULL, core_main, &j[i]);
    }
    for (unsigned i = 0; i < 2; i++) { pthread_join(t[i], NULL); st[i] = j[i].cpu.status; }
    return get64(counter);
}
static void smp(void)
{
    enum qec64_status st[2];
    /* MOVE.Q (A0),D0; MOVE.Q D0,D1; ADDQ.Q #1,D1; CAS.Q D0,D1,(A0); BNE back; SUBQ.L #1,D7; BNE back */
    uint64_t v = two_cores(W(0xA008, 0x2010, 0xA008, 0x2200, 0xA008, 0x5281, 0xA008, 0x0ED0, 0x0040, 0x66EC, 0x5387, 0x66E8, 0x4AFC), st);
    CHECK(st[0] == QEC64_STOPPED && st[1] == QEC64_STOPPED && v == 2u * ROUNDS, "CAS.Q on two cores: %" PRIu64 ", status %d %d", v, st[0], st[1]);
    printf("two cores, CAS.Q above 4 GB: counter %" PRIu64 " of %u\n", v, 2u * ROUNDS);
    /* The same without CAS has to lose counts, or this test could not see a missing atomic. */
    v = two_cores(W(0xA008, 0x2010, 0xA008, 0x5280, 0xA008, 0x2080, 0x5387, 0x66F0, 0x4AFC), st);
    printf("two cores, plain add as control: counter %" PRIu64 " of %u\n", v, 2u * ROUNDS);
    CHECK(st[0] == QEC64_STOPPED && st[1] == QEC64_STOPPED, "control program did not finish: %d %d", st[0], st[1]);
    CHECK(v >= ROUNDS && v < 2u * ROUNDS, "the control lost no counts, so the test proves nothing");
}
int main(void)
{
    ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ram == MAP_FAILED) { puts("no 6 GB of address space"); return 1; }
    rules();
    exceptions();
    machine();
    assembled_program();
    smp();
    if (failures) { printf("%u failures\n", failures); return 1; }
    puts("68100EC64 long mode: all checks passed");
    return 0;
}
