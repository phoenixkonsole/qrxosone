#include "qrx_dyld.h"
#include "qrx_macho.h"
#include "qrx_ppc.h"
#include "qrx_ppc_jit.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;
static uint8_t thin[512], fat[768], memory[0x2000];
static char output[64];
static uint32_t output_len;

static int capture(void *ctx, uint32_t fd, const uint8_t *bytes, uint32_t len)
{
    (void)ctx;
    if (fd != 1u || len > sizeof(output) - output_len)
        return -1;
    memcpy(output + output_len, bytes, len);
    output_len += len;
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void expect(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int deny_access(void *ctx, uint32_t addr, uint32_t size, unsigned access)
{
    (void)ctx; (void)addr; (void)size; (void)access;
    return 0;
}

static void fixture(void)
{
    uint8_t *seg, *thread;
    memset(thin, 0, sizeof(thin));
    put32(thin, 0xfeedfaceu);
    put32(thin + 4, 18u);
    put32(thin + 8, 10u);
    put32(thin + 12, 2u);
    put32(thin + 16, 2u);
    put32(thin + 20, 232u);
    seg = thin + 28;
    put32(seg, 1u); put32(seg + 4, 56u);
    memcpy(seg + 8, "__TEXT", 6);
    put32(seg + 24, 0x1000u); put32(seg + 28, 0x300u);
    put32(seg + 32, 0u); put32(seg + 36, 0x200u);
    put32(seg + 40, 7u); put32(seg + 44, 5u);
    thread = seg + 56;
    put32(thread, 5u); put32(thread + 4, 176u);
    put32(thread + 8, 1u); put32(thread + 12, 40u);
    put32(thread + 16, 0x1120u);
    put32(thin + 0x120, 0x3860002au);
}

static uint32_t be32_of(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* One LC_SEGMENT with exactly one section, 56 + 68 bytes. */
static void dyn_segment(uint8_t *p, const char *seg, uint32_t vmaddr,
                        uint32_t fileoff, uint32_t filesize,
                        uint32_t initprot, const char *sect,
                        uint32_t saddr, uint32_t ssize, uint32_t soff,
                        uint32_t sflags, uint32_t r1)
{
    memset(p, 0, 124u);
    put32(p, 1u);
    put32(p + 4, 124u);
    memcpy(p + 8, seg, strlen(seg));
    put32(p + 24, vmaddr);
    put32(p + 28, 0x1000u);
    put32(p + 32, fileoff);
    put32(p + 36, filesize);
    put32(p + 40, 7u);
    put32(p + 44, initprot);
    put32(p + 48, 1u);
    memcpy(p + 56, sect, strlen(sect));
    memcpy(p + 72, seg, strlen(seg));
    put32(p + 88, saddr);
    put32(p + 92, ssize);
    put32(p + 96, soff);
    put32(p + 100, 2u);
    put32(p + 112, sflags);
    put32(p + 116, r1);
}

/* A dynamically linked PPC executable: two segments, one __la_symbol_ptr
 * section with three slots, LC_SYMTAB/LC_DYSYMTAB, a dylinker and a dylib
 * command.  The text calls the bound pointers instead of a stub, so the test
 * covers exactly what the binder has to produce. */
static void dyn_fixture(uint8_t *img)
{
    static const uint32_t code[] = {
        0x38600001u, /* li r3,1: stdout */
        0x3c800000u, 0x60842100u, /* r4 = 0x2100 */
        0x38a00003u, /* li r5,3 */
        0x3d800000u, 0x618c2000u, /* r12 = 0x2000 */
        0x818c0000u,              /* r12 = *(0x2000) */
        0x7d8903a6u, /* mtctr r12 */
        0x4e800421u, /* bctrl */
        0x3c600000u, 0x60632100u, /* r3 = 0x2100 */
        0x3d800000u, 0x618c2004u, /* r12 = 0x2004 */
        0x818c0000u,              /* r12 = *(r12) */
        0x7d8903a6u,
        0x4e800421u,
        0x38000001u, /* li r0,1 */
        0x44000002u  /* sc: exit */
    };
    unsigned i;

    memset(img, 0, 0x1000u);
    put32(img + 0x00, 0xfeedfaceu);
    put32(img + 0x04, 18u);
    put32(img + 0x08, 10u);
    put32(img + 0x0c, 2u);
    put32(img + 0x10, 7u);
    put32(img + 0x14, 0x260u);
    put32(img + 0x18, 0x85u); /* NOUNDEFS|DYLDLINK|TWOLEVEL */
    dyn_segment(img + 0x01cu, "__TEXT", 0x1000u, 0x0u, 0x400u, 5u,
                "__text", 0x1300u, 0x100u, 0x300u, 0x80000400u, 0u);
    dyn_segment(img + 0x098u, "__DATA", 0x2000u, 0x400u, 0x400u, 3u,
                "__la_symbol_ptr", 0x2000u, 12u, 0x400u, 7u, 0u);
    put32(img + 0x114u, 5u);
    put32(img + 0x118u, 176u);
    put32(img + 0x11cu, 1u);
    put32(img + 0x120u, 40u);
    put32(img + 0x124u, 0x1300u); /* srr0 */
    put32(img + 0x1c4u, 2u);
    put32(img + 0x1c8u, 24u);
    put32(img + 0x1ccu, 0x800u);  /* symoff */
    put32(img + 0x1d0u, 3u);      /* nsyms */
    put32(img + 0x1d4u, 0x880u);  /* stroff */
    put32(img + 0x1d8u, 0x40u);   /* strsize */
    put32(img + 0x1dcu, 0xbu);
    put32(img + 0x1e0u, 80u);
    put32(img + 0x1f4u, 0u);      /* iundefsym (+24) */
    put32(img + 0x1f8u, 2u);      /* nundefsym (+28) */
    put32(img + 0x214u, 0x900u);  /* indirectsymoff (+56) */
    put32(img + 0x218u, 3u);      /* nindirectsyms (+60) */
    put32(img + 0x22cu, 0xeu);
    put32(img + 0x230u, 28u);
    put32(img + 0x234u, 12u);
    memcpy(img + 0x238u, "/usr/lib/dyld", 14u);
    put32(img + 0x248u, 0xcu);
    put32(img + 0x24cu, 52u);
    put32(img + 0x250u, 24u);
    memcpy(img + 0x268u, "/usr/lib/libSystem.B.dylib", 27u);
    for (i = 0; i < sizeof(code) / sizeof(code[0]); i++)
        put32(img + 0x300u + 4u * i, code[i]);
    memcpy(img + 0x500u, "hi\n", 3u);
    put32(img + 0x800u, 1u);              /* "_write" */
    img[0x804] = 0x01;                    /* N_UNDF|N_EXT */
    put32(img + 0x80cu, 8u);              /* "_strlen" */
    img[0x810] = 0x01;
    put32(img + 0x818u, 16u);             /* "_data" */
    img[0x81c] = 0x0f;                    /* N_SECT|N_EXT */
    img[0x81d] = 2u;                      /* section 2 */
    put32(img + 0x820u, 0x2008u);         /* its address */
    memcpy(img + 0x881u, "_write", 7u);
    memcpy(img + 0x888u, "_strlen", 8u);
    memcpy(img + 0x890u, "_data", 6u);
    put32(img + 0x900u, 0u);
    put32(img + 0x904u, 1u);
    put32(img + 0x908u, 2u);
}

int main(void)
{
    static qrx_macho_t img, bad;
    qrx_macho_status_t st;
    fixture();
    st = qrx_macho_parse(&img, thin, sizeof(thin));
    expect("thin PPC executable", st == QRX_MACHO_OK && img.entry == 0x1120u &&
           img.cpu_subtype == 10u && img.segment_count == 1u);
    memset(memory, 0x5a, sizeof(memory));
    st = qrx_macho_map(&img, memory, sizeof(memory));
    expect("segment bytes", st == QRX_MACHO_OK &&
           memcmp(memory + 0x1000, thin, sizeof(thin)) == 0);
    expect("zero fill", memory[0x1200] == 0 && memory[0x12ff] == 0 &&
           memory[0x1300] == 0x5a);
    memset(fat, 0, sizeof(fat));
    put32(fat, 0xcafebabeu); put32(fat + 4, 2u);
    put32(fat + 8, 7u); put32(fat + 16, 0x40u);
    put32(fat + 20, 0x40u);
    put32(fat + 28, 18u); put32(fat + 32, 10u);
    put32(fat + 36, 0x100u); put32(fat + 40, sizeof(thin));
    put32(fat + 44, 8u);
    memcpy(fat + 0x100, thin, sizeof(thin));
    st = qrx_macho_parse(&img, fat, sizeof(fat));
    expect("fat PPC selection", st == QRX_MACHO_OK &&
           img.slice_offset == 0x100u && img.entry == 0x1120u);
    put32(fat + 36, 0xfffffff0u);
    expect("fat out of bounds", qrx_macho_parse(&img, fat, sizeof(fat)) ==
           QRX_MACHO_ERR_FORMAT);
    fixture();
    expect("truncated commands", qrx_macho_parse(&img, thin, 100u) ==
           QRX_MACHO_ERR_TRUNCATED);
    put32(thin + 32, 0u);
    expect("zero command size", qrx_macho_parse(&img, thin, sizeof(thin)) ==
           QRX_MACHO_ERR_COMMAND);
    fixture();
    put32(thin + 28 + 36, 0x301u);
    expect("file larger than segment", qrx_macho_parse(&img, thin, sizeof(thin)) ==
           QRX_MACHO_ERR_SEGMENT);
    fixture();
    put32(thin + 28 + 56 + 16, 0x1300u);
    expect("entry outside executable file bytes", qrx_macho_parse(&img, thin, sizeof(thin)) ==
           QRX_MACHO_ERR_ENTRY);
    fixture();
    expect("restore fixture", qrx_macho_parse(&img, thin, sizeof(thin)) == QRX_MACHO_OK);
    memset(memory, 0x5a, sizeof(memory));
    expect("memory too small", qrx_macho_map(&img, memory, 0x1100u) == QRX_MACHO_ERR_MEMORY &&
           memory[0x1000] == 0x5a);
    bad = img;
    bad.segments[1] = bad.segments[0];
    bad.segments[1].vmaddr = 0x1100u;
    bad.segment_count = 2u;
    expect("overlap rejected before writes", qrx_macho_map(&bad, memory, sizeof(memory)) ==
           QRX_MACHO_ERR_SEGMENT && memory[0x1000] == 0x5a);
    bad = img;
    bad.segments[1] = bad.segments[0];
    bad.segments[1].vmaddr = 0u;
    bad.segments[1].vmsize = 0x1000u;
    bad.segments[1].filesize = 0u;
    bad.segments[1].maxprot = bad.segments[1].initprot = 0u;
    bad.segment_count = 2u;
    expect("pagezero not mapped", qrx_macho_map(&bad, memory, sizeof(memory)) == QRX_MACHO_OK &&
           memory[0] == 0x5a);
    memset(memory, 0x5a, sizeof(memory));
    bad.segments[1].vmsize = 0x1100u;
    expect("overlapping pagezero refused before writes", qrx_macho_map(&bad, memory, sizeof(memory)) ==
           QRX_MACHO_ERR_SEGMENT && memory[0x1000] == 0x5a);
    expect("source aliases memory", qrx_macho_map(&img, thin, sizeof(thin)) == QRX_MACHO_ERR_ARGUMENT);

    {
        /* A static _start: write(1, argv[0], 7), exit(argc). It exercises
         * the loaded instructions, initial stack, sc return and exit code. */
        static const uint32_t code[] = {
            0x80810004u, /* lwz r4,4(r1): argv[0] */
            0x38600001u, /* li r3,1 */
            0x38a00007u, /* li r5,7 */
            0x38000004u, /* li r0,4: write */
            0x44000002u, /* sc */
            0x80610000u, /* lwz r3,0(r1): argc */
            0x38000001u, /* li r0,1: exit */
            0x44000002u  /* sc */
        };
        qrx_macho_run_result_t res;
        qrx_macho_run_status_t rs;
        unsigned i, before = failures;
        fixture();
        for (i = 0; i < sizeof(code) / sizeof(code[0]); i++)
            put32(thin + 0x120u + 4u * i, code[i]);
        expect("process fixture parse", qrx_macho_parse(&img, thin, sizeof(thin)) == QRX_MACHO_OK);
        output_len = 0;
        rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u, 100u,
                           capture, NULL, &res);
        expect("process exit and instruction count", rs == QRX_MACHO_RUN_EXITED &&
               res.exit_code == 1u && res.instructions == 8u);
        expect("write argv[0]", output_len == 7u && memcmp(output, "qrxmach", 7u) == 0);
        expect("argc argv environment stack", memory[0x1fe3] == 1u &&
               memory[0x1fe6] == 0x1fu && memory[0x1fe7] == 0xf8u &&
               memory[0x1fe8] == 0u && memory[0x1fef] == 0u &&
               memory[0x1ff3] == 0u);
        put32(thin + 0x120u + 4u, 0x38600003u); /* invalid fd */
        rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u, 5u,
                           capture, NULL, &res);
        expect("write EBADF", rs == QRX_MACHO_RUN_LIMIT && res.instructions == 5u);
        /* A conditional branch on CR0.SO detects the error in guest code. */
        put32(thin + 0x120u + 20u, 0x41830008u); /* bso +8 */
        put32(thin + 0x120u + 24u, 0x38600063u); /* error not signalled */
        put32(thin + 0x120u + 28u, 0x38000001u);
        put32(thin + 0x120u + 32u, 0x44000002u);
        rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u, 100u,
                           capture, NULL, &res);
        expect("write error CR0.SO and errno", rs == QRX_MACHO_RUN_EXITED &&
               res.exit_code == 9u);
        for (i = 0; i < sizeof(code) / sizeof(code[0]); i++)
            put32(thin + 0x120u + 4u * i, code[i]);
        put32(thin + 0x120u, 0x3880ffffu); /* li r4,-1, invalid pointer */
        rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u, 5u,
                           capture, NULL, &res);
        expect("write invalid pointer does not call output", rs == QRX_MACHO_RUN_LIMIT &&
               output_len == 7u);
        put32(thin + 0x120u + 12u, 0x3800007fu); /* unsupported syscall */
        expect("unsupported syscall stops", qrx_macho_run(&img, memory, sizeof(memory),
               0x1800u, 100u, capture, NULL, &res) == QRX_MACHO_RUN_SYSCALL &&
               res.syscall == 127u);
        put32(thin + 0x120u, 0x48000000u); /* b . */
        expect("instruction budget", qrx_macho_run(&img, memory, sizeof(memory),
               0x1800u, 10u, capture, NULL, &res) == QRX_MACHO_RUN_LIMIT &&
               res.instructions == 10u);
        fixture();
        put32(thin + 16u, 3u); put32(thin + 20u, 248u);
        put32(thin + 260u, 0xeu); put32(thin + 264u, 16u);
        put32(thin + 268u, 12u); memcpy(thin + 272u, "ld", 3u);
        expect("dynamic command parses", qrx_macho_parse(&bad, thin, sizeof(thin)) == QRX_MACHO_OK &&
               bad.has_dylinker && !bad.static_start_supported);
        memset(memory, 0x5a, sizeof(memory));
        expect("dynamic image refused before mapping", qrx_macho_run(&bad, memory,
               sizeof(memory), 0x1800u, 100u, capture, NULL, &res) ==
               QRX_MACHO_RUN_UNSUPPORTED && memory[0x1000] == 0x5a);
        expect("stack collision refused before mapping", qrx_macho_run(&img, memory,
               sizeof(memory), 0x1200u, 100u, capture, NULL, &res) ==
               QRX_MACHO_RUN_MEMORY && memory[0x1000] == 0x5a);
        if (failures == before)
            puts("Darwin static process: argv write, exit and error paths PASS");
    }
    {
        static const struct {
            const char *name;
            uint32_t setup, instruction, fault;
        } cases[] = {
            { "read pagezero", 0x38600000u, 0x80830000u, 0u },
            { "read unmapped gap", 0x38601400u, 0x80830000u, 0x1400u },
            { "read across segment end", 0x386012ffu, 0xa0830000u, 0x12ffu },
            { "write text", 0x38601100u, 0x90830000u, 0x1100u },
            { "fetch stack", 0x38601800u, 0x7c6903a6u, 0x1800u },
            { "FPU read gap", 0x38601400u, 0xc8230000u, 0x1400u },
            { "FPU write text", 0x38601100u, 0xd8230000u, 0x1100u }
        };
        qrx_macho_run_result_t res;
        qrx_macho_run_status_t rs;
        unsigned i, before = failures;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            fixture();
            put32(thin + 0x120u, cases[i].setup);
            put32(thin + 0x124u, cases[i].instruction);
            if (i == 4u)
                put32(thin + 0x128u, 0x4e800420u); /* bctr */
            expect("address fixture parse", qrx_macho_parse(&img, thin, sizeof(thin)) == QRX_MACHO_OK);
            rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u,
                               20u, capture, NULL, &res);
            expect(cases[i].name, rs == QRX_MACHO_RUN_FAULT &&
                   res.fault_addr == cases[i].fault);
        }
        {
            static const uint32_t syscall_code[] = {
                0x38600001u, /* li r3,1 */
                0x38801400u, /* li r4,gap */
                0x38a00004u, /* li r5,4 */
                0x38000004u, /* li r0,write */
                0x44000002u, /* sc */
                0x41830008u, /* bso +8 */
                0x38600063u, /* missing error */
                0x38000001u, /* li r0,exit */
                0x44000002u
            };
            fixture();
            for (i = 0; i < sizeof(syscall_code) / sizeof(syscall_code[0]); i++)
                put32(thin + 0x120u + 4u * i, syscall_code[i]);
            expect("syscall fixture parse", qrx_macho_parse(&img, thin, sizeof(thin)) == QRX_MACHO_OK);
            output_len = 0;
            rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u, 30u,
                               capture, NULL, &res);
            expect("write gap gives EFAULT", rs == QRX_MACHO_RUN_EXITED &&
                   res.exit_code == 14u && output_len == 0u);
            put32(thin + 0x124u, 0x388012feu); /* readable text, ends in gap */
            put32(thin + 0x128u, 0x38a00004u);
            rs = qrx_macho_run(&img, memory, sizeof(memory), 0x1800u, 30u,
                               capture, NULL, &res);
            expect("write crossing segment end gives EFAULT", rs == QRX_MACHO_RUN_EXITED &&
                   res.exit_code == 14u && output_len == 0u);
        }
        {
            qrx_ppc_t ppc;
            qrx_ppc_jit_t jit;
            uint8_t code[16];
            memset(&jit, 0, sizeof(jit));
            memset(code, 0, sizeof(code));
            qrx_ppc_init(&ppc, code, sizeof(code));
            ppc.access = deny_access;
            ppc.cpu.pc = 4u;
            expect("JIT block declines access policy", qrx_ppc_jit_run_block(&jit, &ppc, 1u) == 0);
            expect("JIT runs guarded interpreter", qrx_ppc_run_jit(&ppc, &jit, 1u) ==
                   QRX_PPC_FAULT && ppc.fault_addr == 4u);
        }
        if (failures == before)
            puts("Darwin guest address protections: PASS");
    }
    {
        static uint8_t image[0x1000], guest[0x8000];
        static qrx_macho_t dyn;
        qrx_macho_run_result_t res;
        qrx_macho_run_status_t rs;
        qrx_macho_dynamic_t cfg;
        unsigned before = failures;

        dyn_fixture(image);
        expect("dynamic fixture parses",
               qrx_macho_parse(&dyn, image, sizeof(image)) == QRX_MACHO_OK &&
               dyn.dynamic_start_supported && !dyn.static_start_supported &&
               dyn.section_count == 2u && dyn.dylib_name_count == 1u &&
               dyn.symtab.nsyms == 3u);
        cfg.imports = qrx_dyld_default_imports;
        cfg.import_count = qrx_dyld_default_import_count;
        cfg.trampoline_base = 0x6000u;
        cfg.trampoline_size = 0x200u;
        memset(guest, 0, sizeof(guest));
        output_len = 0;
        rs = qrx_macho_run_dynamic(&dyn, guest, sizeof(guest), 0x7000u, 200u,
                                   capture, NULL, &cfg, &res);
        expect("dynamic write and strlen imports",
               rs == QRX_MACHO_RUN_EXITED && res.exit_code == 3u &&
               res.imports_bound == 2u && res.imports_unresolved == 0u &&
               output_len == 3u && memcmp(output, "hi\n", 3u) == 0);
        expect("dynamic trampoline shared per import",
               be32_of(guest + 0x2000u) == 0x6010u &&
               be32_of(guest + 0x2004u) == 0x601cu);
        expect("dynamic internal symbol bound",
               be32_of(guest + 0x2008u) == 0x2008u);
        expect("dynamic undefined lsymtab count",
               dyn.symtab.nundefsym == 2u);
        {
            static const struct qrx_dyld_import only_exit[] = {
                { "_exit", QRX_DYLD_SEL_EXIT }
            };
            cfg.imports = only_exit;
            cfg.import_count = 1u;
            rs = qrx_macho_run_dynamic(&dyn, guest, sizeof(guest), 0x7000u,
                                       200u, capture, NULL, &cfg, &res);
            expect("dynamic unresolved symbol stops the load",
                   rs == QRX_MACHO_RUN_SYSCALL && res.imports_unresolved == 2u &&
                   strcmp(res.missing_import, "_write") == 0);
            cfg.imports = qrx_dyld_default_imports;
            cfg.import_count = qrx_dyld_default_import_count;
        }
        cfg.trampoline_base = 0x2000u;
        expect("dynamic trampoline area inside a segment refused",
               qrx_macho_run_dynamic(&dyn, guest, sizeof(guest), 0x7000u, 200u,
                                     capture, NULL, &cfg, &res) ==
               QRX_MACHO_RUN_MEMORY);
        cfg.trampoline_base = 0x6f00u;
        cfg.trampoline_size = 0x200u;
        expect("dynamic trampoline area in the stack refused",
               qrx_macho_run_dynamic(&dyn, guest, sizeof(guest), 0x7000u, 200u,
                                     capture, NULL, &cfg, &res) ==
               QRX_MACHO_RUN_MEMORY);
        if (failures == before)
            puts("Darwin dynamic binding: symtab/indirect/lazy pointers PASS");
    }
    printf("Mach-O prototype: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
