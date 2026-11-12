/* Loads the linked kernel and runs it on two 68100EC64 cores. Compares its report with the numbers
 * the same task code gives on the Mac. Usage: run <kernel.elf> */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RAM_SIZE (UINT64_C(8) << 30)
static uint64_t g64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned g16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

static uint64_t sha_like(uint64_t seed, uint64_t rounds)
{
    uint64_t x = seed;
    for (uint64_t i = 0; i < rounds; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x += i; }
    return x;
}
static unsigned bad;
static void expect(const char **p, const char *key, uint64_t want)
{
    size_t n = strlen(key);
    const char *s = *p, *e = strchr(s, '\n');
    uint64_t got = 0;
    if (!e || strncmp(s, key, n) || s[n] != ' ') { printf("FAIL expected line %s, have: %.40s\n", key, s); bad++; return; }
    sscanf(s + n + 1, "%" SCNx64, &got);
    if (got != want) { printf("FAIL %s: got %" PRIx64 " want %" PRIx64 "\n", key, got, want); bad++; }
    else printf("ok   %s = %" PRIx64 "\n", key, got);
    *p = e + 1;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f || ram == MAP_FAILED) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (memcmp(b, "\177ELF\2\2", 6)) { puts("not big-endian ELF64"); return 2; }
    uint64_t entry = g64(b + 24), ph = g64(b + 32);
    unsigned phn = g16(b + 56);
    for (unsigned i = 0; i < phn; i++) {
        const uint8_t *p = b + ph + i * 56;
        if (g32(p) != 1) continue;
        memcpy(ram + g64(p + 16), b + g64(p + 8), g64(p + 32));
        printf("load %016" PRIx64 " %" PRIu64 " bytes\n", g64(p + 16), g64(p + 40));
    }
    qec64_machine *m = qec64_machine_new(ram, RAM_SIZE, 2);
    static char out[8192];
    unsigned who = 0;
    qec64_machine_console(m, out, sizeof out - 1);
    enum qec64_status st = qec64_machine_run(m, entry, 0x8000, 120.0, &who);
    size_t used = qec64_machine_console_used(m);
    out[used] = 0;
    qec64_cpu *c = qec64_machine_core(m, who);
    printf("--- console\n%s--- end, status %d on core %u, exit %" PRIu64 ", pc %016" PRIx64 ", fault %016" PRIx64 "\n", out, st, who, c->exit_code, c->pc, c->fault_addr);
    printf("instructions: core 0 %" PRIu64 ", core 1 %" PRIu64 "\n", qec64_machine_core(m, 0)->instructions, qec64_machine_core(m, 1)->instructions);
    if (st != QEC64_HALTED || c->exit_code != 0) {
        puts("FAIL did not end cleanly");
        for (unsigned k = 0; k < 2; k++) {
            qec64_cpu *q = qec64_machine_core(m, k);
            printf("core %u pc %016" PRIx64 " sp %016" PRIx64 " sr %d stack:", k, q->pc, q->a[7], q->supervisor);
            for (unsigned i = 0; i < 14 && q->a[7] + i * 8 + 8 < RAM_SIZE; i++) printf(" %" PRIx64, g64(ram + q->a[7] + i * 8));
            puts("");
        }
        return 1;
    }
    const char *p = out;
    /* The console also carries the printing task's line, somewhere before the report. */
    const char *hello = strstr(out, "user says hello\n");
    if (!hello) { puts("FAIL no greeting from user state"); bad++; }
    p = hello ? hello + 16 : out;
    expect(&p, "shared", 4 * 20000);
    /* Whether a plain counter lost counts depends on timing: not checked, only reported above. */
    if ((p = strchr(p, '\n'))) p++; else return 1;
    expect(&p, "task", 0x77);
    expect(&p, "task", 0xFA000000u + 8);
    expect(&p, "task", 0xFA000000u + 2 + (UINT64_C(0x7000000000) << 8));
    expect(&p, "task", 0xFA000000u + 5);
    expect(&p, "task", sha_like(0x1234567, 40000));
    expect(&p, "task", sha_like(0x89ABCDE, 40000));
    expect(&p, "task", sha_like(0xF00D, 40000));
    expect(&p, "task", sha_like(0xBEEF, 40000));
    expect(&p, "ran-on-both", 1);
    expect(&p, "preempted", 1);
    expect(&p, "core1-ran", 1);
    expect(&p, "ticks0", 1);
    expect(&p, "ticks1", 1);
    expect(&p, "faults", 3);
    printf("%u failures\n", bad);
    qec64_machine_free(m);
    return bad ? 1 : 0;
}
