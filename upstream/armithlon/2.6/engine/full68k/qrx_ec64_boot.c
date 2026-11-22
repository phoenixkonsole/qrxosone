/* The 64-bit AROS boot loader for the 68100EC64, see qrx_ec64_boot.h */
#include "qrx_ec64_boot.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define LOAD UINT64_C(0x00100000)       /* the kernel's first section */
#define STUB UINT64_C(0x00008000)       /* sets A0 and jumps to start */
#define STUB_SP UINT64_C(0x00007000)
#define MEM_LOWER UINT64_C(0x00010000)  /* below it: nothing, so that NULL faults stay visible */
#define R_68K_32 1
#define R_68K_PC32 4
#define R_68K64_64 240                  /* this CPU's own 64-bit absolute relocation */
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
#define MAX_BSS 64

typedef struct {
    uint8_t *ram;
    uint64_t ram_size;
    uint64_t bss[MAX_BSS][2];
    unsigned nbss;
    FILE *map;
    char *err;
    size_t errlen;
} loader;

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static void p64(uint8_t *ram, uint64_t a, uint64_t v) { for (int i = 0; i < 8; i++) ram[a + i] = (uint8_t)(v >> (56 - i * 8)); }
static void p32(uint8_t *ram, uint64_t a, uint32_t v) { for (int i = 0; i < 4; i++) ram[a + i] = (uint8_t)(v >> (24 - i * 8)); }
static void p16(uint8_t *ram, uint64_t a, uint16_t v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }
static uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static bool fail(loader *L, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (L->err && L->errlen) vsnprintf(L->err, L->errlen, fmt, ap);
    va_end(ap);
    return false;
}

/* Loads one relocatable ELF at *next and moves *next to its end; *start gets `start` if wanted */
static bool load_elf(loader *L, const char *path, uint64_t *next, uint64_t *start)
{
    FILE *f = fopen(path, "rb");
    if (!f) return fail(L, "%s: cannot open", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)n);
    bool ok = false;
    uint64_t *addr = NULL, *symv = NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return fail(L, "%s: cannot read", path); }
    fclose(f);
    if (n < 64 || memcmp(b, "\177ELF\2\2", 6) || g16(b + 16) != 1) { fail(L, "%s: not a big-endian ELF64 relocatable file", path); goto out; }
    uint64_t shoff = g64(b + 40);
    unsigned shnum = g16(b + 60);
#define SH(i) (b + shoff + (uint64_t)(i) * 64)
    addr = calloc(shnum, sizeof *addr);
    if (!addr) goto out;
    uint64_t at = *next;
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *s = SH(i);
        if (!(g64(s + 8) & SHF_ALLOC) || !g64(s + 32)) continue;
        uint64_t al = g64(s + 48) ? g64(s + 48) : 1, size = g64(s + 32);
        at = align(at, al < 16 ? 16 : al);
        if (at + size > L->ram_size) { fail(L, "%s: does not fit in RAM", path); goto out; }
        addr[i] = at;
        if (g32(s + 4) == SHT_NOBITS) {
            memset(L->ram + at, 0, size);
            if (L->nbss < MAX_BSS - 1) { L->bss[L->nbss][0] = at; L->bss[L->nbss][1] = size; L->nbss++; }
        } else
            memcpy(L->ram + at, b + g64(s + 24), size);
        at += size;
    }
    const uint8_t *symsh = NULL;
    for (unsigned i = 0; i < shnum; i++) if (g32(SH(i) + 4) == SHT_SYMTAB) symsh = SH(i);
    if (!symsh) { fail(L, "%s: no symbol table", path); goto out; }
    const uint8_t *syms = b + g64(symsh + 24);
    unsigned nsyms = (unsigned)(g64(symsh + 32) / 24);
    const char *str = (const char *)b + g64(SH(g32(symsh + 40)) + 24);
    symv = calloc(nsyms ? nsyms : 1, sizeof *symv);
    if (!symv) goto out;
    for (unsigned k = 0; k < nsyms; k++) {
        const uint8_t *y = syms + k * 24;
        unsigned shndx = g16(y + 6);
        uint64_t v = g64(y + 8);
        if (shndx == 0) {
            if (k && (y[4] >> 4) != 2) { fail(L, "%s: undefined symbol %s", path, str + g32(y)); goto out; }
            continue;
        }
        symv[k] = shndx == 0xFFF1 ? v : shndx < shnum ? addr[shndx] + v : v;
        if (start && !strcmp(str + g32(y), "start")) *start = symv[k];
        if (L->map && (y[4] & 0xf) == 2 && shndx < shnum) fprintf(L->map, "%016" PRIx64 " %s\n", symv[k], str + g32(y));
    }
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *s = SH(i);
        if (g32(s + 4) != SHT_RELA) continue;
        unsigned target = g32(s + 44);
        if (target >= shnum || !addr[target]) continue;
        for (uint64_t o = 0; o < g64(s + 32); o += 24) {
            const uint8_t *r = b + g64(s + 24) + o;
            uint64_t where = addr[target] + g64(r), info = g64(r + 8);
            int64_t add = (int64_t)g64(r + 16);
            uint64_t S = symv[info >> 32];
            switch ((unsigned)(info & 0xFFFFFFFF)) {
            case R_68K64_64: p64(L->ram, where, S + (uint64_t)add); break;
            case R_68K_32: p32(L->ram, where, (uint32_t)(S + (uint64_t)add)); break;
            case R_68K_PC32: p32(L->ram, where, (uint32_t)(S + (uint64_t)add - where)); break;
            default: fail(L, "%s: relocation type %u not handled", path, (unsigned)(info & 0xFFFFFFFF)); goto out;
            }
        }
    }
#undef SH
    *next = at;
    ok = true;
out:
    free(symv);
    free(addr);
    free(b);
    return ok;
}

int qec64_aros_load(uint8_t *ram, uint64_t ram_size, const qec64_aros_boot *b, qec64_aros_loaded *out,
                    char *err, size_t errlen)
{
    loader L = { .ram = ram, .ram_size = ram_size, .map = b->map, .err = err, .errlen = errlen };
    uint64_t start = 0, next = LOAD;
    if (ram_size < 16u << 20) { fail(&L, "too little RAM"); return -1; }
    if (!load_elf(&L, b->kernel, &next, &start)) return -1;
    if (!start) { fail(&L, "%s: no start symbol", b->kernel); return -1; }
    for (unsigned i = 0; i < b->nmodules; i++) {
        uint64_t from = align(next, 4096);
        next = from;
        if (!load_elf(&L, b->modules[i], &next, NULL)) return -1;
        if (b->log) fprintf(b->log, "module %s at %016" PRIx64 "-%016" PRIx64 "\n", b->modules[i], from, next);
    }
    uint64_t kernel_end = next;
    /* Boot data after the modules: the BSS list, the command line, the loader's name, the tags */
    uint64_t bssl = align(kernel_end, 4096);
    for (unsigned k = 0; k < L.nbss; k++) { p64(ram, bssl + k * 16, L.bss[k][0]); p64(ram, bssl + k * 16 + 8, L.bss[k][1]); }
    p64(ram, bssl + L.nbss * 16, 0);
    p64(ram, bssl + L.nbss * 16 + 8, 0);
    uint64_t cmd = bssl + (L.nbss + 1) * 16, name = cmd + 512, tags = align(name + 64, 16);
    uint64_t mem_upper = ram_size, disk_at = 0;
    if (b->disk) {
        if (b->disk_size + (64u << 20) > ram_size) { fail(&L, "the disk image does not fit in RAM"); return -1; }
        disk_at = (ram_size - b->disk_size) & ~UINT64_C(4095);
        memcpy(ram + disk_at, b->disk, b->disk_size);
        mem_upper = disk_at;
        snprintf((char *)ram + cmd, 512, "%s qrxdisk=0x%" PRIx64 ",0x%" PRIx64, b->cmdline ? b->cmdline : "", disk_at, b->disk_size);
    } else
        snprintf((char *)ram + cmd, 512, "%s", b->cmdline ? b->cmdline : "");
    strcpy((char *)ram + name, "ARMithlon");
    uint64_t tp = tags;
#define TAG(t, v) do { p64(ram, tp, (t)); p64(ram, tp + 8, (v)); tp += 16; } while (0)
    uint64_t prot_end = align(tags + 16 * 16, 4096);
    TAG(KRN_KernelBase, LOAD);
    TAG(KRN_KernelLowest, LOAD);
    TAG(KRN_KernelHighest, kernel_end);
    TAG(KRN_KernelBss, bssl);
    TAG(KRN_CmdLine, cmd);
    TAG(KRN_BootLoader, name);
    TAG(KRN_ProtAreaStart, LOAD);
    TAG(KRN_ProtAreaEnd, prot_end);
    TAG(KRN_MEMLower, MEM_LOWER);
    TAG(KRN_MEMUpper, mem_upper);
    TAG(0, 0);
#undef TAG
    /* In 32-bit mode: movea.l #tags,a0 ($207C) and jmp start ($4EF9) */
    p16(ram, STUB, 0x207C); p32(ram, STUB + 2, (uint32_t)tags);
    p16(ram, STUB + 6, 0x4EF9); p32(ram, STUB + 8, (uint32_t)start);
    out->pc = STUB;
    out->sp = STUB_SP;
    out->kernel_end = kernel_end;
    out->disk_at = disk_at;
    return 0;
}
