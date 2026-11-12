/* Loads m68k64 ELF object files, applies their relocations and runs test functions in the 68100EC64
 * reference core. Each C test file has int64_t test_<name>(void) functions that return a value the
 * host computes the same way, because the same C source is built for the host too (HOST_TEST).
 * Code, data and stack all sit above 4 GB.
 * Usage: runc <file.o>... */
#include "qrx68100ec64.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RAM_SIZE (UINT64_C(6) << 30)
#define LOAD UINT64_C(0x120000000)
#define STACK UINT64_C(0x138000000)
#define STOP_AT UINT64_C(0x110000000)
#define R_68K_32 1
#define R_68K_16 2
#define R_68K_8 3
#define R_68K_PC32 4
#define R_68K_PC16 5
#define R_68K_PC8 6
#define R_68K64_64 240
static uint8_t *ram;
static uint64_t next_free = LOAD;

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static void p64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; i++) ram[a + i] = (uint8_t)(v >> (56 - i * 8)); }
static void p32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; i++) ram[a + i] = (uint8_t)(v >> (24 - i * 8)); }
static void p16(uint64_t a, uint16_t v) { ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v; }

#define MAXSYM 4096
static struct { char name[96]; uint64_t addr; } globals[MAXSYM];
static unsigned nglobals;
static uint64_t find_global(const char *n)
{
    for (unsigned i = 0; i < nglobals; i++) if (!strcmp(globals[i].name, n)) return globals[i].addr;
    return 0;
}
struct object { uint8_t *b; uint64_t shoff; unsigned shnum; uint64_t addr[256]; };
static struct object objs[8];
static unsigned nobjs;
static const uint8_t *sh(const struct object *o, unsigned i) { return o->b + o->shoff + i * 64; }

static void load_sections(struct object *o, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    o->b = malloc((size_t)n);
    if (fread(o->b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    if (memcmp(o->b, "\177ELF\2\2", 6)) { printf("%s: not big-endian ELF64\n", path); exit(2); }
    o->shoff = g64(o->b + 40);
    o->shnum = g16(o->b + 60);
    for (unsigned i = 0; i < o->shnum && i < 256; i++) {
        const uint8_t *s = sh(o, i);
        uint32_t type = g32(s + 4);
        uint64_t flags = g64(s + 8), size = g64(s + 32), align = g64(s + 48);
        if (!(flags & 2) || (type != 1 && type != 8)) continue;   /* SHF_ALLOC, PROGBITS or NOBITS */
        if (align < 2) align = 2;
        next_free = (next_free + align - 1) & ~(align - 1);
        o->addr[i] = next_free;
        if (type == 1) memcpy(ram + next_free, o->b + g64(s + 24), size);
        next_free += size;
    }
}
static void define_globals(struct object *o)
{
    for (unsigned i = 0; i < o->shnum; i++) {
        const uint8_t *s = sh(o, i);
        if (g32(s + 4) != 2) continue;   /* SHT_SYMTAB */
        const uint8_t *syms = o->b + g64(s + 24), *str = o->b + g64(sh(o, g32(s + 40)) + 24);
        uint64_t n = g64(s + 32) / 24;
        for (uint64_t k = 0; k < n; k++) {
            const uint8_t *y = syms + k * 24;
            unsigned bind = y[4] >> 4, shndx = g16(y + 6);
            if (bind == 0 || shndx == 0 || shndx >= 256) continue;
            if (nglobals == MAXSYM) exit(2);
            strncpy(globals[nglobals].name, (const char *)str + g32(y), 95);
            globals[nglobals++].addr = o->addr[shndx] + g64(y + 8);
        }
    }
}
static unsigned unresolved;
static void relocate(struct object *o)
{
    for (unsigned i = 0; i < o->shnum; i++) {
        const uint8_t *s = sh(o, i);
        if (g32(s + 4) != 4) continue;   /* SHT_RELA */
        unsigned target = g32(s + 44);
        if (!o->addr[target]) continue;
        const uint8_t *symhdr = sh(o, g32(s + 40));
        const uint8_t *syms = o->b + g64(symhdr + 24), *str = o->b + g64(sh(o, g32(symhdr + 40)) + 24);
        const uint8_t *r = o->b + g64(s + 24);
        uint64_t n = g64(s + 32) / 24;
        for (uint64_t k = 0; k < n; k++, r += 24) {
            uint64_t p = o->addr[target] + g64(r), info = g64(r + 8);
            int64_t addend = (int64_t)g64(r + 16);
            const uint8_t *y = syms + (info >> 32) * 24;
            unsigned shndx = g16(y + 6), type = (unsigned)info;
            uint64_t sv;
            if (shndx && shndx < 256) sv = o->addr[shndx] + g64(y + 8);
            else {
                sv = find_global((const char *)str + g32(y));
                if (!sv) { printf("undefined: %s\n", str + g32(y)); unresolved++; continue; }
            }
            uint64_t v = sv + (uint64_t)addend;
            int64_t rel = (int64_t)(v - p);
            switch (type) {
            case R_68K64_64: p64(p, v); break;
            case R_68K_PC32: if (rel != (int32_t)rel) { puts("PC32 out of range"); unresolved++; } p32(p, (uint32_t)rel); break;
            case R_68K_PC16: if (rel != (int16_t)rel) { puts("PC16 out of range"); unresolved++; } p16(p, (uint16_t)rel); break;
            case R_68K_PC8: if (rel != (int8_t)rel) { puts("PC8 out of range"); unresolved++; } ram[p] = (uint8_t)rel; break;
            default: printf("relocation type %u against %s not handled\n", type, str + g32(y)); unresolved++; break;
            }
        }
    }
}
int main(int argc, char **argv)
{
    ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ram == MAP_FAILED) return 2;
    for (int i = 1; i < argc && nobjs < 8; i++) load_sections(&objs[nobjs++], argv[i]);
    for (unsigned i = 0; i < nobjs; i++) define_globals(&objs[i]);
    for (unsigned i = 0; i < nobjs; i++) relocate(&objs[i]);
    if (unresolved) { printf("%u link problems\n", unresolved); return 1; }
    ram[STOP_AT] = 0x4A; ram[STOP_AT + 1] = 0xFC;
    /* What the host computed for the same source, one "name value" per line on stdin. */
    char name[96];
    uint64_t want;
    unsigned ran = 0, bad = 0;
    uint64_t instr = 0;
    while (scanf("%95s %" SCNx64, name, &want) == 2) {
        char sym[120];
        snprintf(sym, sizeof sym, "test_%s", name);
        uint64_t f = find_global(sym);
        qec64_cpu c;
        if (!f) { printf("%s: not found\n", sym); bad++; continue; }
        qec64_init(&c, ram, RAM_SIZE, 0);
        c.m64 = true;
        uint64_t sp = STACK - 8;
        p64(sp, STOP_AT);
        c.a[7] = sp;
        c.pc = f;
        enum qec64_status st = qec64_run(&c, UINT64_C(1) << 33);
        instr += c.instructions;
        ran++;
        if (st != QEC64_STOPPED) {
            printf("FAIL %s: core stopped with %d at %" PRIx64 ", fault address %" PRIx64 "\n", name, st, c.fault_pc, c.fault_addr);
            bad++;
        } else if (c.d[0] != want) {
            printf("FAIL %s: got %016" PRIx64 " want %016" PRIx64 "\n", name, c.d[0], want);
            bad++;
        } else if (c.a[7] != sp + 8) {
            printf("FAIL %s: stack off by %" PRId64 "\n", name, (int64_t)(c.a[7] - (sp + 8)));
            bad++;
        } else printf("ok   %s = %016" PRIx64 "\n", name, c.d[0]);
    }
    printf("%u tests, %u failures, %" PRIu64 " instructions\n", ran, bad, instr);
    return bad ? 1 : 0;
}
