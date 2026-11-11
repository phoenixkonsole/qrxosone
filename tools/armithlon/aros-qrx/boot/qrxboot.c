/* Boots AROS's core.elf (kernel.resource, exec.library, task.resource) on the 68100EC64 reference
 * machine. Loads the relocatable ELF below 4 GB (the core starts in 32-bit mode), applies its
 * relocations, lays out the boot tags and starts `start` with A0 pointing at them, as a boot loader
 * would. The console is the machine's MOVEC $8F0 register.
 * Usage: qrxboot <core.elf> [seconds] [cores] [RAM in MB] [module.elf...] */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define LOAD UINT64_C(0x00100000)       /* the kernel's first section */
#define STUB UINT64_C(0x00008000)       /* sets A0 and jumps to start */
#define STUB_SP UINT64_C(0x00007000)
#define MEM_LOWER UINT64_C(0x00010000)  /* below it: nothing, so that NULL faults stay visible */
#define R_68K_32 1
#define R_68K_PC32 4
#define R_68K64_64 240
#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 2
#define TAG_USER UINT64_C(0x80000000)
#define KRN_Dummy (TAG_USER + 0x03d00000)
#define KRN_KernelBase (KRN_Dummy + 1)
#define KRN_KernelLowest (KRN_Dummy + 2)
#define KRN_KernelHighest (KRN_Dummy + 3)
#define KRN_KernelBss (KRN_Dummy + 4)
#define KRN_CmdLine (KRN_Dummy + 12)
#define KRN_ProtAreaStart (KRN_Dummy + 13)
#define KRN_ProtAreaEnd (KRN_Dummy + 14)
#define KRN_MEMLower (KRN_Dummy + 17)
#define KRN_MEMUpper (KRN_Dummy + 18)
#define KRN_BootLoader (KRN_Dummy + 22)

static uint8_t *ram;
static uint64_t ram_size;
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static void p64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; i++) ram[a + i] = (uint8_t)(v >> (56 - i * 8)); }
static void p32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; i++) ram[a + i] = (uint8_t)(v >> (24 - i * 8)); }
static void p16(uint64_t a, uint16_t v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }
static uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint64_t tagp;
/* Tag is an IPTR on big-endian 64-bit AROS, see utility/tagitem.h */
static void tag(uint64_t t, uint64_t v) { p64(tagp, t); p64(tagp + 8, v); tagp += 16; }

static uint64_t bss_list[64][2];
static unsigned nbss;

/* Loads one relocatable ELF at next and returns the end; *start gets `start` if it has one */
static uint64_t load_elf(const char *path, uint64_t next, uint64_t *start)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    if (memcmp(b, "\177ELF\2\2", 6) || g16(b + 16) != 1) { puts("not a big-endian ELF64 relocatable file"); exit(2); }
    uint64_t shoff = g64(b + 40);
    unsigned shnum = g16(b + 60);
#define SH(i) (b + shoff + (uint64_t)(i) * 64)
    uint64_t *addr = calloc(shnum, sizeof *addr);
    /* Sections in file order, each at its alignment */
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *s = SH(i);
        if (!(g64(s + 8) & SHF_ALLOC) || !g64(s + 32)) continue;
        uint64_t al = g64(s + 48) ? g64(s + 48) : 1, size = g64(s + 32);
        next = align(next, al < 16 ? 16 : al);
        addr[i] = next;
        if (g32(s + 4) == SHT_NOBITS) { memset(ram + next, 0, size); if (nbss < 63) { bss_list[nbss][0] = next; bss_list[nbss][1] = size; nbss++; } }
        else memcpy(ram + next, b + g64(s + 24), size);
        next += size;
    }
    uint64_t kernel_end = next;
    /* Symbols */
    const uint8_t *symsh = NULL;
    for (unsigned i = 0; i < shnum; i++) if (g32(SH(i) + 4) == SHT_SYMTAB) symsh = SH(i);
    if (!symsh) { puts("no symbol table"); exit(2); }
    const uint8_t *syms = b + g64(symsh + 24);
    unsigned nsyms = (unsigned)(g64(symsh + 32) / 24);
    const char *str = (const char *)b + g64(SH(g32(symsh + 40)) + 24);
    uint64_t *symv = calloc(nsyms, sizeof *symv);
    unsigned undefined = 0;
    for (unsigned k = 0; k < nsyms; k++) {
        const uint8_t *y = syms + k * 24;
        unsigned shndx = g16(y + 6);
        uint64_t v = g64(y + 8);
        if (shndx == 0) { if (k && (y[4] >> 4) != 2) { printf("undefined: %s\n", str + g32(y)); undefined++; } continue; }
        symv[k] = shndx == 0xFFF1 ? v : shndx < shnum ? addr[shndx] + v : v;
        if (start && !strcmp(str + g32(y), "start")) *start = symv[k];
    }
    if (undefined) { printf("%s: %u undefined symbols\n", path, undefined); exit(2); }
    /* Relocations */
    unsigned nrel = 0;
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *s = SH(i);
        if (g32(s + 4) != SHT_RELA) continue;
        unsigned target = g32(s + 44);
        if (!addr[target]) continue;
        for (uint64_t o = 0; o < g64(s + 32); o += 24, nrel++) {
            const uint8_t *r = b + g64(s + 24) + o;
            uint64_t where = addr[target] + g64(r), info = g64(r + 8);
            int64_t add = (int64_t)g64(r + 16);
            uint64_t S = symv[info >> 32];
            switch ((unsigned)(info & 0xFFFFFFFF)) {
            case R_68K64_64: p64(where, S + (uint64_t)add); break;
            case R_68K_32: p32(where, (uint32_t)(S + (uint64_t)add)); break;
            case R_68K_PC32: p32(where, (uint32_t)(S + (uint64_t)add - where)); break;
            default: printf("relocation type %u not handled\n", (unsigned)(info & 0xFFFFFFFF)); exit(2);
            }
        }
    }
    return kernel_end;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <core.elf> [seconds] [cores] [RAM MB]\n", argv[0]); return 2; }
    double seconds = argc > 2 ? atof(argv[2]) : 20.0;
    unsigned cores = argc > 3 ? (unsigned)atoi(argv[3]) : 1;
    ram_size = (argc > 4 ? strtoull(argv[4], NULL, 0) : 512) << 20;
    ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ram == MAP_FAILED) return 2;
    /* core.elf first, then further modules (argv after the options, separated by commas) */
    uint64_t start = 0, kernel_end = load_elf(argv[1], LOAD, &start);
    if (!start) { puts("no start symbol in the kernel"); return 2; }
    for (int i = 5; i < argc; i++) {
        uint64_t from = align(kernel_end, 4096);
        kernel_end = load_elf(argv[i], from, NULL);
        printf("module %s at %016" PRIx64 "-%016" PRIx64 "\n", argv[i], from, kernel_end);
    }
    /* Boot data after the kernel: the BSS list, the command line, the tags */
    uint64_t data = align(kernel_end, 4096), bssl = data;
    for (unsigned k = 0; k < nbss; k++) { p64(bssl + k * 16, bss_list[k][0]); p64(bssl + k * 16 + 8, bss_list[k][1]); }
    p64(bssl + nbss * 16, 0); p64(bssl + nbss * 16 + 8, 0);
    uint64_t cmd = bssl + (nbss + 1) * 16, name = cmd + 64;
    strcpy((char *)ram + cmd, "selftest");
    strcpy((char *)ram + name, "qrxboot");
    uint64_t tags = align(name + 64, 16);
    tagp = tags;
    uint64_t prot_end = align(tags + 16 * 16, 4096);
    tag(KRN_KernelBase, LOAD);
    tag(KRN_KernelLowest, LOAD);
    tag(KRN_KernelHighest, kernel_end);
    tag(KRN_KernelBss, bssl);
    tag(KRN_CmdLine, cmd);
    tag(KRN_BootLoader, name);
    tag(KRN_ProtAreaStart, LOAD);
    tag(KRN_ProtAreaEnd, prot_end);
    tag(KRN_MEMLower, MEM_LOWER);
    tag(KRN_MEMUpper, ram_size);
    tag(0, 0);
    /* In 32-bit mode: movea.l #tags,a0 ($207C) and jmp start ($4EF9) */
    p16(STUB, 0x207C); p32(STUB + 2, (uint32_t)tags);
    p16(STUB + 6, 0x4EF9); p32(STUB + 8, (uint32_t)start);
    printf("kernel %016" PRIx64 "-%016" PRIx64 ", start %016" PRIx64 ", tags %016" PRIx64 ", RAM %" PRIu64 " MB, %u cores\n",
           LOAD, kernel_end, start, tags, ram_size >> 20, cores);
    fflush(stdout);
    qec64_machine *m = qec64_machine_new(ram, ram_size, cores);
    static char out[1 << 20];
    unsigned who = 0;
    qec64_machine_console(m, out, sizeof out - 1);
    enum qec64_status st = qec64_machine_run(m, STUB, STUB_SP, seconds, &who);
    size_t used = qec64_machine_console_used(m);
    out[used] = 0;
    printf("--- console\n%s\n--- end: status %d on core %u\n", out, st, who);
    for (unsigned k = 0; k < cores; k++) {
        qec64_cpu *c = qec64_machine_core(m, k);
        printf("core %u: pc %016" PRIx64 " sp %016" PRIx64 " %s ipl %u, %" PRIu64 " instructions, fault %016" PRIx64 "\n", k, c->pc,
               c->a[7], c->supervisor ? "super" : "user", c->ipl, c->instructions, c->fault_addr);
        printf("        a8 (TLS) %016" PRIx64 ", d0 %016" PRIx64 ", vbr %016" PRIx64 "\n", c->a[8], c->d[0], c->vbr);
    }
    return 0;
}
