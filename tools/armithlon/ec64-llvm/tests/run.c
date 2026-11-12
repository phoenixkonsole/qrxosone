/* Runs functions from a flat m68k64 code image in the 68100EC64 reference core, with code, data
 * and stack above 4 GB, and compares each result with the same computation done on the host.
 * Arguments go on the stack in 8-byte slots, as M68k64CallingConv.td says.
 * Usage: run <image> <symbol file: "offset name" per line> */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RAM_SIZE (UINT64_C(6) << 30)
#define CODE UINT64_C(0x120000000)
#define STACK UINT64_C(0x130000000)
#define DATA UINT64_C(0x140000000)
#define STOP_AT UINT64_C(0x110000000)
static uint8_t *ram;
static unsigned failures, ran;
static struct { char name[64]; uint64_t off; } syms[64];
static unsigned nsyms;

static void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; i++) ram[a + i] = (uint8_t)(v >> (56 - i * 8)); }
static uint64_t call(const char *name, unsigned n, const uint64_t *args, uint64_t *a0)
{
    qec64_cpu c;
    uint64_t off = UINT64_MAX;
    for (unsigned i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, name)) off = syms[i].off;
    if (off == UINT64_MAX) { printf("%s: not in image\n", name); failures++; return 0; }
    qec64_init(&c, ram, RAM_SIZE, 0);
    c.m64 = true;
    uint64_t sp = STACK - 8 * n;
    for (unsigned i = 0; i < n; i++) put64(sp + 8 * i, args[i]);
    sp -= 8;
    put64(sp, STOP_AT);
    c.a[7] = sp;
    c.pc = CODE + off;
    enum qec64_status st = qec64_run(&c, UINT64_C(1) << 30);
    if (st != QEC64_STOPPED) {
        printf("%s: core stopped with %d at %" PRIx64 " (offset %" PRIx64 "), fault address %" PRIx64 "\n", name, st, c.fault_pc, c.fault_pc - CODE, c.fault_addr);
        failures++;
    }
    if (c.a[7] != sp + 8) { printf("%s: stack off by %" PRId64 "\n", name, (int64_t)(c.a[7] - (sp + 8))); failures++; }
    if (a0) *a0 = c.a[0];
    return c.d[0];
}
#define TEST(name, want, ...) do { const uint64_t a_[] = { __VA_ARGS__ }; uint64_t w_ = (want), g_ = call(name, sizeof a_ / 8, a_, NULL); ran++; \
    if (g_ != w_) { printf("FAIL %s: got %016" PRIx64 " want %016" PRIx64 "\n", name, g_, w_); failures++; } else printf("ok   %s = %016" PRIx64 "\n", name, g_); } while (0)
#define TEST32(name, want, ...) do { const uint64_t a_[] = { __VA_ARGS__ }; uint32_t w_ = (want), g_ = (uint32_t)call(name, sizeof a_ / 8, a_, NULL); ran++; \
    if (g_ != w_) { printf("FAIL %s: got %08x want %08x\n", name, g_, w_); failures++; } else printf("ok   %s = %08x\n", name, g_); } while (0)

static uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    size_t got = fread(ram + CODE, 1, 1 << 20, f);
    fclose(f);
    f = fopen(argv[2], "r");
    if (!f) return 2;
    while (nsyms < 64 && fscanf(f, "%" SCNx64 " %63s", &syms[nsyms].off, syms[nsyms].name) == 2) nsyms++;
    fclose(f);
    printf("image %zu bytes, %u functions\n", got, nsyms);
    ram[STOP_AT] = 0x4A; ram[STOP_AT + 1] = 0xFC;

    const uint64_t a = UINT64_C(0xFEDCBA9876543210), b = UINT64_C(0x0123456789ABCDEF), big = UINT64_C(0x8000000000000123);
    TEST("add64", a + b, a, b);
    TEST("add64", UINT64_C(0x100000000), UINT64_C(0xFFFFFFFF), 1);
    TEST32("add32", 0xFFFFFFFFu + 2u, (uint64_t)0xFFFFFFFFu, 2);
    TEST("logic", (((a ^ b) | 4080) & a) - b, a, b);
    TEST("shifts", (a << 13) ^ (a >> 7) ^ (uint64_t)((int64_t)a >> 13), a, 13);
    TEST("shifts", (big << 40) ^ (big >> 7) ^ (uint64_t)((int64_t)big >> 40), big, 40);
    { uint64_t x = UINT64_C(0x0000123456789ABC), y = 977;
      TEST("muldiv", (x * y) / 7 + (uint64_t)((int64_t)x / (int64_t)y) + x % y, x, y); }
    { uint64_t x = (uint64_t)-5000000000LL, y = 3;
      TEST("muldiv", (x * y) / 7 + (uint64_t)((int64_t)x / (int64_t)y) + x % y, x, y); }
    /* small arguments sit at the low end of their 8-byte slot */
    TEST("widen", (uint64_t)(int64_t)(int32_t)0x80000001u + 0xFFFFu + (uint64_t)(int64_t)(int8_t)0x80, (uint64_t)0x80000001u, (uint64_t)0xFFFFu, (uint64_t)0x80u);
    TEST32("narrow", (uint32_t)a - (uint32_t)(a >> 32), a);
    TEST("cmpsel", ((int64_t)a < (int64_t)b ? a : b) + (a > b), a, b);
    TEST("cmpsel", ((int64_t)b < (int64_t)a ? b : a) + (b > a), b, a);
    TEST("cmpsel", 5, UINT64_C(5), UINT64_C(5));
    TEST("callfib", fib(25), 25);
    { const uint64_t n = 100000;
      uint64_t h = UINT64_C(0xCBF29CE484222325);
      for (uint64_t i = 0; i < n; i++) ram[DATA + i] = (uint8_t)(i * 2654435761u >> 11);
      for (uint64_t i = 0; i < n; i++) { h ^= ram[DATA + i]; h *= UINT64_C(0x100000001B3); }
      TEST("fnv", h, DATA, n); }
    { uint64_t n = UINT64_C(0x100000001), want = 0;
      for (uint64_t i = 0; i < 16; i++) want += i * n + (uint32_t)(i * n);
      TEST("memsum", want, DATA + 0x100000, n); }
    printf("%u calls, %u failures\n", ran, failures);
    return failures ? 1 : 0;
}
