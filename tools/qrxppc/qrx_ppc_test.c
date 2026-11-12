/*
 * QRXPPC reference test program.
 *
 * Each case places hand-encoded PPC32 instructions at 0x1000, runs the
 * interpreter until sc, and checks registers and big-endian memory.  The same
 * binary is built for the AROS 68k guest and for the host differential run.
 */
#include "qrx_ppc.h"
#include "qrx_ppc_jit.h"
#include "qrx_pef.h"
#include "qrx_mix.h"

#include <stdio.h>
#include <string.h>

#ifdef __AROS__
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#endif

#define CODE_BASE 0x1000u
#define DATA_BASE 0x2000u
#define JIT_CODE_SIZE (64u * 1024u)

static uint8_t memory[QRX_PPC_DEFAULT_MEM_SIZE];
static qrx_ppc_t ppc;
static unsigned failures;

#ifdef __AROS__
static uint8_t memory_jit[QRX_PPC_DEFAULT_MEM_SIZE];
static qrx_ppc_jit_t jit;
static uint8_t *jit_code;
#endif

static void put32(uint32_t addr, uint32_t value)
{
    memory[addr + 0] = (uint8_t)(value >> 24);
    memory[addr + 1] = (uint8_t)(value >> 16);
    memory[addr + 2] = (uint8_t)(value >> 8);
    memory[addr + 3] = (uint8_t)value;
}

static uint32_t get32(uint32_t addr)
{
    return ((uint32_t)memory[addr + 0] << 24) |
           ((uint32_t)memory[addr + 1] << 16) |
           ((uint32_t)memory[addr + 2] << 8) |
           (uint32_t)memory[addr + 3];
}

static void put64(uint32_t addr, uint64_t value)
{
    unsigned i;

    for (i = 0; i < 8; i++)
        memory[addr + 7 - i] = (uint8_t)(value >> (i * 8));
}

static uint64_t get64(uint32_t addr)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < 8; i++)
        value = (value << 8) | memory[addr + i];
    return value;
}

static uint64_t double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void prepare(const uint32_t *code, size_t count)
{
    size_t i;

    memset(memory, 0, sizeof(memory));
    qrx_ppc_init(&ppc, memory, (uint32_t)sizeof(memory));
    for (i = 0; i < count; i++)
        put32(CODE_BASE + (uint32_t)i * 4u, code[i]);
    ppc.cpu.pc = CODE_BASE;
    ppc.cpu.gpr[1] = 0x3000;
    ppc.cpu.gpr[2] = 0x3000;
}

#ifdef __AROS__
static void compare_jit_state(const qrx_ppc_t *want, const qrx_ppc_t *got)
{
    unsigned bad = 0;

    if (want->status != got->status) {
        printf("FAIL jit diff status: interp=%d jit=%d\n",
               (int)want->status, (int)got->status);
        bad = 1;
    }
    if (want->cpu.pc != got->cpu.pc) {
        printf("FAIL jit diff pc: interp=%08lx jit=%08lx\n",
               (unsigned long)want->cpu.pc, (unsigned long)got->cpu.pc);
        bad = 1;
    }
    if (memcmp(want->cpu.gpr, got->cpu.gpr, sizeof(want->cpu.gpr)) != 0) {
        printf("FAIL jit diff gpr: interp=%08lx/%08lx/%08lx jit=%08lx/%08lx/%08lx\n",
               (unsigned long)want->cpu.gpr[3],
               (unsigned long)want->cpu.gpr[4],
               (unsigned long)want->cpu.gpr[5],
               (unsigned long)got->cpu.gpr[3],
               (unsigned long)got->cpu.gpr[4],
               (unsigned long)got->cpu.gpr[5]);
        bad = 1;
    }
    if (memcmp(want->cpu.fpr, got->cpu.fpr, sizeof(want->cpu.fpr)) != 0) {
        printf("FAIL jit diff fpr\n");
        bad = 1;
    }
    if (want->cpu.cr != got->cpu.cr) {
        printf("FAIL jit diff cr: interp=%08lx jit=%08lx\n",
               (unsigned long)want->cpu.cr, (unsigned long)got->cpu.cr);
        bad = 1;
    }
    if (want->cpu.lr != got->cpu.lr || want->cpu.ctr != got->cpu.ctr ||
        want->cpu.xer != got->cpu.xer ||
        want->cpu.msr != got->cpu.msr || want->cpu.srr0 != got->cpu.srr0 ||
        want->cpu.srr1 != got->cpu.srr1 ||
        want->cpu.fpscr != got->cpu.fpscr) {
        printf("FAIL jit diff special registers\n");
        bad = 1;
    }
    if (want->fault_addr != got->fault_addr) {
        printf("FAIL jit diff fault_addr: interp=%08lx jit=%08lx\n",
               (unsigned long)want->fault_addr,
               (unsigned long)got->fault_addr);
        bad = 1;
    }
    if (want->insns != got->insns) {
        printf("FAIL jit diff insns: interp=%lu jit=%lu\n",
               (unsigned long)want->insns, (unsigned long)got->insns);
        bad = 1;
    }
    if (memcmp(memory, memory_jit, sizeof(memory)) != 0) {
        printf("FAIL jit diff memory\n");
        bad = 1;
    }
    if (bad)
        failures++;
}

static void expect_jit_block(const char *what, unsigned want)
{
    unsigned got = jit.nblocks ? jit.blocks[0].insns : 0;

    if (jit.nblocks != 1 || got != want) {
        printf("FAIL %s jit block: nblocks=%lu insns=%lu want=%u\n",
               what, (unsigned long)jit.nblocks, (unsigned long)got, want);
        failures++;
    }
}

static void expect_jit_source(const char *what, uint32_t insn)
{
    uint8_t raw[4];
    unsigned i;
    unsigned j;

    raw[0] = (uint8_t)(insn >> 24);
    raw[1] = (uint8_t)(insn >> 16);
    raw[2] = (uint8_t)(insn >> 8);
    raw[3] = (uint8_t)insn;
    for (i = 0; i < jit.nblocks; i++) {
        for (j = 0; j + 4u <= jit.blocks[i].source_len; j += 4u) {
            if (memcmp(&jit.blocks[i].source[j], raw, sizeof(raw)) == 0)
                return;
        }
    }
    printf("FAIL %s jit source %08lx\n", what, (unsigned long)insn);
    failures++;
}
#endif

static qrx_ppc_status_t run(void)
{
#ifdef __AROS__
    qrx_ppc_t initial = ppc;
#endif
    qrx_ppc_status_t status;

#ifdef __AROS__
    if (jit_code)
        memcpy(memory_jit, memory, sizeof(memory));
#endif
    status = qrx_ppc_run(&ppc, 10000);
#ifdef __AROS__
    if (jit_code) {
        qrx_ppc_t native = initial;

        native.mem.data = memory_jit;
        qrx_ppc_jit_reset(&jit);
        qrx_ppc_run_jit(&native, &jit, 10000);
        compare_jit_state(&ppc, &native);
    }
#endif
    return status;
}

static qrx_ppc_status_t start(const uint32_t *code, size_t count)
{
    prepare(code, count);
    return run();
}

static qrx_ppc_status_t run_fpscr(uint32_t fpscr)
{
    ppc.cpu.fpscr = fpscr;
    return run();
}

static void expect_u32(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: got %08lx want %08lx\n",
               what, (unsigned long)got, (unsigned long)want);
        failures++;
    }
}

static void expect_u64(const char *what, uint64_t got, uint64_t want)
{
    if (got != want) {
        printf("FAIL %s: got %08x%08x want %08x%08x\n",
               what,
               (unsigned)(got >> 32), (unsigned)got,
               (unsigned)(want >> 32), (unsigned)want);
        failures++;
    }
}

static void expect_status(const char *what, qrx_ppc_status_t got,
                          qrx_ppc_status_t want)
{
    if (got != want) {
        printf("FAIL %s: status %d want %d (pc=%08lx)\n",
               what, (int)got, (int)want, (unsigned long)ppc.cpu.pc);
        failures++;
    }
}

static void report(const char *name, unsigned before)
{
    printf("%s: %s\n", name, failures == before ? "PASS" : "FAIL");
}

static void test_arithmetic(void)
{
    static const uint32_t code[] = {
        0x3860002Au, /* li r3,42 */
        0x38800007u, /* li r4,7 */
        0x7CA32214u, /* add r5,r3,r4 */
        0x38C02000u, /* li r6,0x2000 */
        0x90A60000u, /* stw r5,0(r6) */
        0x38E0FFFFu, /* li r7,-1 */
        0x90E60004u, /* stw r7,4(r6) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("arith", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("arith r3", ppc.cpu.gpr[3], 42);
    expect_u32("arith r4", ppc.cpu.gpr[4], 7);
    expect_u32("arith r5", ppc.cpu.gpr[5], 49);
    expect_u32("arith r7", ppc.cpu.gpr[7], 0xFFFFFFFFu);
    expect_u32("arith mem0", get32(DATA_BASE), 49);
    expect_u32("arith mem4", get32(DATA_BASE + 4), 0xFFFFFFFFu);
    report("arithmetic", before);
}

static void test_loop(void)
{
    static const uint32_t code[] = {
        0x38600000u, /* li r3,0 */
        0x3880000Au, /* li r4,10 */
        0x7C8903A6u, /* mtctr r4 */
        0x7C632214u, /* add r3,r3,r4 */
        0x3884FFFFu, /* addi r4,r4,-1 */
        0x42003FF8u, /* bdnz loop */
        0x38C02000u, /* li r6,0x2000 */
        0x90660000u, /* stw r3,0(r6) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("loop", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("loop r3", ppc.cpu.gpr[3], 55);
    expect_u32("loop r4", ppc.cpu.gpr[4], 0);
    expect_u32("loop ctr", ppc.cpu.ctr, 0);
    expect_u32("loop mem0", get32(DATA_BASE), 55);
    report("loop", before);
}

static void test_compare_branch(void)
{
    static const uint32_t code[] = {
        0x38600005u, /* li r3,5 */
        0x38800005u, /* li r4,5 */
        0x7C032000u, /* cmpw r3,r4 */
        0x41820008u, /* beq +8 */
        0x38A00001u, /* li r5,1 (skipped) */
        0x38A00002u, /* li r5,2 */
        0x38C02000u, /* li r6,0x2000 */
        0x90A60000u, /* stw r5,0(r6) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("branch", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("branch r5", ppc.cpu.gpr[5], 2);
    expect_u32("branch cr", (ppc.cpu.cr >> 28) & 0xFu, 2);
    expect_u32("branch mem0", get32(DATA_BASE), 2);
    report("compare_branch", before);
}

static void test_compare_cr(void)
{
    static const uint32_t code[] = {
        0x38600005u, /* li r3,5 */
        0x38800005u, /* li r4,5 */
        0x7C032000u, /* cmpw r3,r4 */
        0x2C830005u, /* cmpwi cr1,r3,5 */
        0x7D032040u, /* cmplw cr2,r3,r4 */
        0x29830005u, /* cmplwi cr3,r3,5 */
        0x38A0FFFFu, /* li r5,-1 */
        0x2E050000u, /* cmpwi cr4,r5,0 */
        0x2A850000u, /* cmplwi cr5,r5,0 */
        0x3CC00000u, /* lis r6,0 */
        0x60C6FFFFu, /* ori r6,r6,0xFFFF */
        0x2C260000u, /* cmpi L=1 r6,0 */
        0x28A60000u, /* cmpli L=1 cr1,r6,0 */
        0x7F032000u, /* cmpw cr6,r3,r4 */
        0x7F832040u, /* cmplw cr7,r3,r4 */
        0x44000002u  /* sc */
    };
    static const uint32_t so[] = {
        0x38600005u, /* li r3,5 */
        0x38800005u, /* li r4,5 */
        0x2C830005u, /* cmpwi cr1,r3,5 */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    expect_status("compare_cr", run(), QRX_PPC_TRAP);
    expect_u32("compare_cr r5", ppc.cpu.gpr[5], 0xFFFFFFFFu);
    expect_u32("compare_cr r6", ppc.cpu.gpr[6], 0x0000FFFFu);
    expect_u32("compare_cr cr0", (ppc.cpu.cr >> 28) & 0xFu, 8u);
    expect_u32("compare_cr cr1", (ppc.cpu.cr >> 24) & 0xFu, 4u);
    expect_u32("compare_cr cr2", (ppc.cpu.cr >> 20) & 0xFu, 2u);
    expect_u32("compare_cr cr3", (ppc.cpu.cr >> 16) & 0xFu, 2u);
    expect_u32("compare_cr cr4", (ppc.cpu.cr >> 12) & 0xFu, 8u);
    expect_u32("compare_cr cr5", (ppc.cpu.cr >> 8) & 0xFu, 4u);
    expect_u32("compare_cr cr6", (ppc.cpu.cr >> 4) & 0xFu, 2u);
    expect_u32("compare_cr cr7", ppc.cpu.cr & 0xFu, 2u);
#ifdef __AROS__
    expect_jit_block("compare_cr", sizeof(code) / sizeof(code[0]));
#endif

    prepare(so, sizeof(so) / sizeof(so[0]));
    ppc.cpu.xer = 0x80000000u;
    expect_status("compare_so", run(), QRX_PPC_TRAP);
    expect_u32("compare_so cr1", (ppc.cpu.cr >> 24) & 0xFu, 3u);
#ifdef __AROS__
    expect_jit_block("compare_so", sizeof(so) / sizeof(so[0]));
#endif
    report("compare_cr", before);
}

static void test_load_store(void)
{
    static const uint32_t code[] = {
        0x3C601234u, /* lis r3,0x1234 */
        0x60635678u, /* ori r3,r3,0x5678 */
        0x38C02000u, /* li r6,0x2000 */
        0x90660000u, /* stw r3,0(r6) */
        0xA0860000u, /* lhz r4,0(r6) */
        0x88A60000u, /* lbz r5,0(r6) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("loadstore", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("loadstore r3", ppc.cpu.gpr[3], 0x12345678u);
    expect_u32("loadstore r4", ppc.cpu.gpr[4], 0x1234u);
    expect_u32("loadstore r5", ppc.cpu.gpr[5], 0x12u);
    expect_u32("loadstore mem0", get32(DATA_BASE), 0x12345678u);
    report("load_store", before);
}

static void test_mul_div(void)
{
    static const uint32_t code[] = {
        0x3860FFF9u, /* li r3,-7 */
        0x38800003u, /* li r4,3 */
        0x7CA321D6u, /* mullw r5,r3,r4 */
        0x7CC523D6u, /* divw r6,r5,r4 */
        0x38E02000u, /* li r7,0x2000 */
        0x90A70000u, /* stw r5,0(r7) */
        0x90C70004u, /* stw r6,4(r7) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("muldiv", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("muldiv r5", ppc.cpu.gpr[5], 0xFFFFFFEBu);
    expect_u32("muldiv r6", ppc.cpu.gpr[6], 0xFFFFFFF9u);
    expect_u32("muldiv mem0", get32(DATA_BASE), 0xFFFFFFEBu);
    expect_u32("muldiv mem4", get32(DATA_BASE + 4), 0xFFFFFFF9u);
    report("mul_div", before);
}

static void test_rotate(void)
{
    static const uint32_t code[] = {
        0x3C601234u, /* lis r3,0x1234 */
        0x60635678u, /* ori r3,r3,0x5678 */
        0x5464400Eu, /* rlwinm r4,r3,8,0,7 */
        0x38A02000u, /* li r5,0x2000 */
        0x90850000u, /* stw r4,0(r5) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("rotate", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("rotate r4", ppc.cpu.gpr[4], 0x34000000u);
    expect_u32("rotate mem0", get32(DATA_BASE), 0x34000000u);
    report("rotate", before);
}

static void test_logical_cr(void)
{
    static const uint32_t code[] = {
        0x386000F0u, /* li r3,0xF0 */
        0x3880000Fu, /* li r4,0x0F */
        0x7CA32378u, /* or r5,r3,r4 */
        0x70A600FFu, /* andi. r6,r5,0xFF */
        0x41820008u, /* beq +8 */
        0x38E00001u, /* li r7,1 (skipped) */
        0x38E00002u, /* li r7,2 */
        0x39002000u, /* li r8,0x2000 */
        0x90E80000u, /* stw r7,0(r8) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    expect_status("logical", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("logical r5", ppc.cpu.gpr[5], 0xFFu);
    expect_u32("logical r6", ppc.cpu.gpr[6], 0xFFu);
    expect_u32("logical r8", ppc.cpu.gpr[8], 0x2000u);
    expect_u32("logical r7", ppc.cpu.gpr[7], 2);
    expect_u32("logical mem0", get32(DATA_BASE), 2);
    report("logical_cr", before);
}

static void test_jit_cr0(void)
{
    static const uint32_t add_eq[] = {
        0x38600000u, /* li r3,0 */
        0x38800000u, /* li r4,0 */
        0x7CA32215u, /* add. r5,r3,r4 */
        0x44000002u  /* sc */
    };
    static const uint32_t add_lt[] = {
        0x3860FFFFu, /* li r3,-1 */
        0x38800000u, /* li r4,0 */
        0x7CA32215u, /* add. r5,r3,r4 */
        0x44000002u  /* sc */
    };
    static const uint32_t add_gt_so[] = {
        0x38600001u, /* li r3,1 */
        0x38800002u, /* li r4,2 */
        0x7CA32215u, /* add. r5,r3,r4 */
        0x44000002u  /* sc */
    };
    static const uint32_t andi_eq[] = {
        0x386000FFu, /* li r3,0xFF */
        0x70640000u, /* andi. r4,r3,0 */
        0x44000002u  /* sc */
    };
    static const uint32_t andis_lt[] = {
        0x3C608000u, /* lis r3,0x8000 */
        0x74648000u, /* andis. r4,r3,0x8000 */
        0x44000002u  /* sc */
    };
    static const uint32_t cr0_fault[] = {
        0x3C608000u, /* lis r3,0x8000 */
        0x74648000u, /* andis. r4,r3,0x8000 -> CR0 LT */
        0x3CC00004u, /* lis r6,4 -> 0x40000 */
        0x80E60000u, /* lwz r7,0(r6) -> fault */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(add_eq, sizeof(add_eq) / sizeof(add_eq[0]));
    expect_status("cr0_eq", run(), QRX_PPC_TRAP);
    expect_u32("cr0_eq value", ppc.cpu.gpr[5], 0);
    expect_u32("cr0_eq cr0", (ppc.cpu.cr >> 28) & 0xFu, 2);

    prepare(add_lt, sizeof(add_lt) / sizeof(add_lt[0]));
    expect_status("cr0_lt", run(), QRX_PPC_TRAP);
    expect_u32("cr0_lt value", ppc.cpu.gpr[5], 0xFFFFFFFFu);
    expect_u32("cr0_lt cr0", (ppc.cpu.cr >> 28) & 0xFu, 8);

    prepare(add_gt_so, sizeof(add_gt_so) / sizeof(add_gt_so[0]));
    ppc.cpu.xer = 0x80000000u;
    expect_status("cr0_gt_so", run(), QRX_PPC_TRAP);
    expect_u32("cr0_gt_so value", ppc.cpu.gpr[5], 3);
    expect_u32("cr0_gt_so cr0", (ppc.cpu.cr >> 28) & 0xFu, 5);

    prepare(andi_eq, sizeof(andi_eq) / sizeof(andi_eq[0]));
    expect_status("cr0_andi", run(), QRX_PPC_TRAP);
    expect_u32("cr0_andi value", ppc.cpu.gpr[4], 0);
    expect_u32("cr0_andi cr0", (ppc.cpu.cr >> 28) & 0xFu, 2);

    prepare(andis_lt, sizeof(andis_lt) / sizeof(andis_lt[0]));
    expect_status("cr0_andis", run(), QRX_PPC_TRAP);
    expect_u32("cr0_andis value", ppc.cpu.gpr[4], 0x80000000u);
    expect_u32("cr0_andis cr0", (ppc.cpu.cr >> 28) & 0xFu, 8);

    prepare(cr0_fault, sizeof(cr0_fault) / sizeof(cr0_fault[0]));
    expect_status("cr0_fault", run(), QRX_PPC_FAULT);
    expect_u32("cr0_fault pc", ppc.cpu.pc, 0x100Cu);
    expect_u32("cr0_fault addr", ppc.fault_addr, 0x40000u);
    expect_u32("cr0_fault insns", ppc.insns, 4);
    expect_u32("cr0_fault cr0", (ppc.cpu.cr >> 28) & 0xFu, 8);
    report("jit_cr0", before);
}

#ifdef __AROS__
#define FUZZ_CASES 64u
#define FUZZ_INSNS 16u
#define FUZZ_FAULT_CASES 96u

static uint32_t fuzz_rand(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t fuzz_reg(uint32_t *state)
{
    return 3u + fuzz_rand(state) % 8u;
}

static uint32_t fuzz_insn(uint32_t *state)
{
    uint32_t rt = fuzz_reg(state);
    uint32_t ra = fuzz_reg(state);
    uint32_t rb = fuzz_reg(state);
    uint32_t imm = fuzz_rand(state) & 0xFFFFu;

    switch (fuzz_rand(state) % 15u) {
    case 0: /* addi */
        if ((fuzz_rand(state) & 3u) == 0)
            ra = 0;
        return 0x38000000u | (rt << 21) | (ra << 16) | imm;
    case 1: /* addis */
        if ((fuzz_rand(state) & 3u) == 0)
            ra = 0;
        return 0x3C000000u | (rt << 21) | (ra << 16) | imm;
    case 2: /* ori */
        return 0x60000000u | (rt << 21) | (ra << 16) | imm;
    case 3: /* oris */
        return 0x64000000u | (rt << 21) | (ra << 16) | imm;
    case 4: /* xori */
        return 0x68000000u | (rt << 21) | (ra << 16) | imm;
    case 5: /* xoris */
        return 0x6C000000u | (rt << 21) | (ra << 16) | imm;
    case 6: /* andi. */
        return 0x70000000u | (rt << 21) | (ra << 16) | imm;
    case 7: /* andis. */
        return 0x74000000u | (rt << 21) | (ra << 16) | imm;
    case 8: /* add. */
        return 0x7C000214u | (rt << 21) | (ra << 16) | (rb << 11) |
               (fuzz_rand(state) & 1u);
    case 9: /* subf. */
        return 0x7C000050u | (rt << 21) | (ra << 16) | (rb << 11) |
               (fuzz_rand(state) & 1u);
    case 10: /* and. */
        return 0x7C000038u | (rt << 21) | (ra << 16) | (rb << 11) |
               (fuzz_rand(state) & 1u);
    case 11: /* or. */
        return 0x7C000378u | (rt << 21) | (ra << 16) | (rb << 11) |
               (fuzz_rand(state) & 1u);
    case 12: /* xor. */
        return 0x7C000278u | (rt << 21) | (ra << 16) | (rb << 11) |
               (fuzz_rand(state) & 1u);
    case 13: { /* lwz, absolute address in the data area */
        uint32_t disp = 0x2000u + ((fuzz_rand(state) & 0x7FFu) << 2);
        return 0x80000000u | (rt << 21) | disp;
    }
    default: { /* stw, absolute address in the data area */
        uint32_t disp = 0x2000u + ((fuzz_rand(state) & 0x7FFu) << 2);
        return 0x90000000u | (rt << 21) | disp;
    }
    }
}

static void test_jit_fuzz(void)
{
    uint32_t state = 0x9E3779B9u;
    unsigned before = failures;
    uint32_t c;

    for (c = 0; c < FUZZ_CASES; c++) {
        uint32_t code[FUZZ_INSNS + 1];
        uint32_t i;
        unsigned case_before = failures;

        for (i = 0; i < FUZZ_INSNS; i++)
            code[i] = fuzz_insn(&state);
        code[FUZZ_INSNS] = 0x44000002u; /* sc */
        prepare(code, FUZZ_INSNS + 1);

        /* Random memory and architectural state, but keep the code range
         * untouched so every generated block ends at the sc above. */
        for (i = 0; i < 0x800u; i++)
            put32(DATA_BASE + i * 4u, fuzz_rand(&state));
        for (i = 3; i <= 10; i++)
            ppc.cpu.gpr[i] = fuzz_rand(&state);
        ppc.cpu.cr = fuzz_rand(&state);
        ppc.cpu.xer = fuzz_rand(&state);
        ppc.cpu.lr = fuzz_rand(&state);
        ppc.cpu.ctr = fuzz_rand(&state);
        ppc.cpu.msr = fuzz_rand(&state);

        expect_status("jit_fuzz", run(), QRX_PPC_TRAP);
        if (failures != case_before) {
            printf("FAIL jit_fuzz case %lu\n", (unsigned long)c);
            break;
        }
    }
    report("jit_fuzz", before);
}

static uint32_t fuzz_reg_not(uint32_t *state, uint32_t avoid)
{
    uint32_t reg;

    do {
        reg = fuzz_reg(state);
    } while (reg == avoid);
    return reg;
}

static void fuzz_emit_addr(uint32_t *code, unsigned *count, uint32_t reg,
                           uint32_t addr)
{
    code[(*count)++] = 0x3C000000u | (reg << 21) | (addr >> 16);
    code[(*count)++] = 0x60000000u | (reg << 21) | (reg << 16) |
                       (addr & 0xFFFFu);
}

/* Every form writes CR0, so the following fault must commit the pending
 * nibble before the checked memory helper aborts the block. */
static uint32_t fuzz_cr0_insn(uint32_t *state, uint32_t addr_reg)
{
    uint32_t rt = fuzz_reg_not(state, addr_reg);
    uint32_t ra = fuzz_reg(state);
    uint32_t rb = fuzz_reg(state);
    uint32_t imm = fuzz_rand(state) & 0xFFFFu;

    switch (fuzz_rand(state) % 6u) {
    case 0: /* andi. */
        return 0x70000000u | (ra << 21) | (rt << 16) | imm;
    case 1: /* andis. */
        return 0x74000000u | (ra << 21) | (rt << 16) | imm;
    case 2: /* add. */
        return 0x7C000214u | (rt << 21) | (ra << 16) | (rb << 11) | 1u;
    case 3: /* subf. */
        return 0x7C000050u | (rt << 21) | (ra << 16) | (rb << 11) | 1u;
    case 4: /* and. */
        return 0x7C000038u | (rt << 21) | (ra << 16) | (rb << 11) | 1u;
    default: /* xor. */
        return 0x7C000278u | (rt << 21) | (ra << 16) | (rb << 11) | 1u;
    }
}

/*
 * The test maps guest RAM at 0x00000000..0x0003FFFF.  The random cases use
 * the full 32-bit space and move an address that happens to be in RAM just
 * past the RAM end, so every case remains a fault case.  The last 32 cases
 * cover word accesses at the 256-KiB RAM boundary and near the 32-bit wrap.
 */
static uint32_t fuzz_fault_addr(uint32_t *state, unsigned c,
                                const char **kind)
{
    static const uint32_t ram_boundary[8] = {
        QRX_PPC_DEFAULT_MEM_SIZE - 3u,
        QRX_PPC_DEFAULT_MEM_SIZE - 2u,
        QRX_PPC_DEFAULT_MEM_SIZE - 1u,
        QRX_PPC_DEFAULT_MEM_SIZE,
        QRX_PPC_DEFAULT_MEM_SIZE + 1u,
        QRX_PPC_DEFAULT_MEM_SIZE + 2u,
        QRX_PPC_DEFAULT_MEM_SIZE + 3u,
        QRX_PPC_DEFAULT_MEM_SIZE + 4u
    };
    static const uint32_t wrap_boundary[8] = {
        0xFFFFFFF8u, 0xFFFFFFF9u, 0xFFFFFFFAu, 0xFFFFFFFBu,
        0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu
    };
    uint32_t ea;

    if (c < 64u) {
        ea = fuzz_rand(state);
        if (c & 1u) {
            ea = (ea & ~3u) | (1u + fuzz_rand(state) % 3u);
            *kind = "unaligned";
        } else {
            *kind = "random";
        }
        if (ea < QRX_PPC_DEFAULT_MEM_SIZE)
            ea += QRX_PPC_DEFAULT_MEM_SIZE;
        return ea;
    }
    if (c < 80u) {
        *kind = "ram_boundary";
        return ram_boundary[(c - 64u) >> 1];
    }
    *kind = "wrap";
    return wrap_boundary[(c - 80u) >> 1];
}

static void test_jit_fault_fuzz(void)
{
    uint32_t state = 0xC0DEF00Du;
    unsigned before = failures;
    uint32_t c;

    for (c = 0; c < FUZZ_FAULT_CASES; c++) {
        uint32_t code[8];
        uint32_t addr_reg = fuzz_reg(&state);
        uint32_t value_reg = fuzz_reg_not(&state, addr_reg);
        uint32_t ea;
        const char *kind;
        uint32_t i;
        unsigned n = 0;
        unsigned fault_index;
        unsigned case_before = failures;
        int store = (c < 64u) ? ((c & 2u) != 0) : ((c & 1u) != 0);

        ea = fuzz_fault_addr(&state, c, &kind);

        fuzz_emit_addr(code, &n, addr_reg, ea);
        code[n++] = fuzz_cr0_insn(&state, addr_reg);
        fault_index = n;
        code[n++] = (store ? 0x90000000u : 0x80000000u) |
                    (value_reg << 21) | (addr_reg << 16);
        code[n++] = 0x44000002u; /* sc, not reached after the fault */

        prepare(code, n);
        for (i = 0; i < 0x800u; i++)
            put32(DATA_BASE + i * 4u, fuzz_rand(&state));
        for (i = 3; i <= 10; i++)
            ppc.cpu.gpr[i] = fuzz_rand(&state);
        ppc.cpu.cr = fuzz_rand(&state);
        ppc.cpu.xer = fuzz_rand(&state);
        ppc.cpu.lr = fuzz_rand(&state);
        ppc.cpu.ctr = fuzz_rand(&state);
        ppc.cpu.msr = fuzz_rand(&state);

        expect_status("jit_fault_fuzz", run(), QRX_PPC_FAULT);
        expect_u32("jit_fault_fuzz pc", ppc.cpu.pc,
                   CODE_BASE + fault_index * 4u);
        expect_u32("jit_fault_fuzz addr", ppc.fault_addr, ea);
        expect_u32("jit_fault_fuzz insns", ppc.insns, fault_index + 1u);
        if (jit.nblocks != 1) {
            printf("FAIL jit_fault_fuzz jit blocks: got %lu want 1\n",
                   (unsigned long)jit.nblocks);
            failures++;
        } else if (jit.blocks[0].insns != n) {
            printf("FAIL jit_fault_fuzz jit block insns: got %lu want %lu\n",
                   (unsigned long)jit.blocks[0].insns, (unsigned long)n);
            failures++;
        }
        if (failures != case_before) {
            printf("FAIL jit_fault_fuzz case %lu type=%s addr=%08lx\n",
                   (unsigned long)c, kind, (unsigned long)ea);
            break;
        }
    }
    report("jit_fault_fuzz", before);
}

static void test_jit_widths(void)
{
    static const uint32_t valid[] = {
        0x38602000u, /* li r3,0x2000 */
        0x3C801234u, /* lis r4,0x1234 */
        0x60845678u, /* ori r4,r4,0x5678 */
        0x90830000u, /* stw r4,0(r3) */
        0x88830000u, /* lbz r4,0(r3) */
        0xA0A30002u, /* lhz r5,2(r3) */
        0xA9230002u, /* lha r9,2(r3) */
        0x3D00FFFFu, /* lis r8,0xFFFF */
        0x6108FFFEu, /* ori r8,r8,0xFFFE */
        0x91030004u, /* stw r8,4(r3) */
        0xA8C30006u, /* lha r6,6(r3) */
        0xA0E30006u, /* lhz r7,6(r3) */
        0x3CA0ABCDu, /* lis r5,0xABCD */
        0x60A51234u, /* ori r5,r5,0x1234 */
        0x98A30004u, /* stb r5,4(r3) */
        0xB0A30006u, /* sth r5,6(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t byte_last[] = {
        0x3C600004u, /* lis r3,4 */
        0x3863FFFFu, /* addi r3,r3,-1 */
        0x88830000u, /* lbz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t half_last[] = {
        0x3C600004u, /* lis r3,4 */
        0x3863FFFEu, /* addi r3,r3,-2 */
        0xA0830000u, /* lhz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t word_last[] = {
        0x3C600004u, /* lis r3,4 */
        0x3863FFFCu, /* addi r3,r3,-4 */
        0x80830000u, /* lwz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t byte_oob[] = {
        0x3C600004u, /* lis r3,4 -> 0x40000 */
        0x88830000u, /* lbz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t half_oob[] = {
        0x3C600004u, /* lis r3,4 -> 0x40000 */
        0xA0830000u, /* lhz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t word_oob[] = {
        0x3C600004u, /* lis r3,4 -> 0x40000 */
        0x80830000u, /* lwz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t stb_oob[] = {
        0x3C600004u, /* lis r3,4 -> 0x40000 */
        0x38800012u, /* li r4,0x12 */
        0x98830000u, /* stb r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t sth_oob[] = {
        0x3C600004u, /* lis r3,4 -> 0x40000 */
        0x38801234u, /* li r4,0x1234 */
        0xB0830000u, /* sth r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t half_unaligned[] = {
        0x3C600004u, /* lis r3,4 */
        0x3863FFFFu, /* addi r3,r3,-1 */
        0xA0830000u, /* lhz r4,0(r3) */
        0x44000002u  /* sc */
    };
    const unsigned valid_insns = sizeof(valid) / sizeof(valid[0]);
    const unsigned byte_last_insns = sizeof(byte_last) / sizeof(byte_last[0]);
    const unsigned half_last_insns = sizeof(half_last) / sizeof(half_last[0]);
    const unsigned word_last_insns = sizeof(word_last) / sizeof(word_last[0]);
    const unsigned byte_oob_insns = sizeof(byte_oob) / sizeof(byte_oob[0]);
    const unsigned half_oob_insns = sizeof(half_oob) / sizeof(half_oob[0]);
    const unsigned word_oob_insns = sizeof(word_oob) / sizeof(word_oob[0]);
    const unsigned stb_oob_insns = sizeof(stb_oob) / sizeof(stb_oob[0]);
    const unsigned sth_oob_insns = sizeof(sth_oob) / sizeof(sth_oob[0]);
    const unsigned unaligned_insns = sizeof(half_unaligned) /
                                     sizeof(half_unaligned[0]);
    unsigned before = failures;

    prepare(valid, valid_insns);
    expect_status("jit_widths", run(), QRX_PPC_TRAP);
    expect_u32("jit_widths r3", ppc.cpu.gpr[3], 0x2000u);
    expect_u32("jit_widths lbz", ppc.cpu.gpr[4], 0x12u);
    expect_u32("jit_widths lhz", ppc.cpu.gpr[5], 0xABCD1234u);
    expect_u32("jit_widths lha_pos", ppc.cpu.gpr[9], 0x5678u);
    expect_u32("jit_widths lha_neg", ppc.cpu.gpr[6], 0xFFFFFFFEu);
    expect_u32("jit_widths lhz_zero", ppc.cpu.gpr[7], 0xFFFEu);
    expect_u32("jit_widths store_src", ppc.cpu.gpr[8], 0xFFFFFFFEu);
    expect_u32("jit_widths mem0", get32(DATA_BASE), 0x12345678u);
    expect_u32("jit_widths mem4", get32(DATA_BASE + 4u), 0x34FF1234u);
    expect_jit_block("jit_widths", valid_insns);

    prepare(byte_last, byte_last_insns);
    put32(0x3FFFCu, 0x123456ABu);
    expect_status("jit_width_byte_last", run(), QRX_PPC_TRAP);
    expect_u32("jit_width_byte_last value", ppc.cpu.gpr[4], 0xABu);
    expect_jit_block("jit_width_byte_last", byte_last_insns);

    prepare(half_last, half_last_insns);
    put32(0x3FFFCu, 0x12345678u);
    expect_status("jit_width_half_last", run(), QRX_PPC_TRAP);
    expect_u32("jit_width_half_last value", ppc.cpu.gpr[4], 0x5678u);
    expect_jit_block("jit_width_half_last", half_last_insns);

    prepare(word_last, word_last_insns);
    put32(0x3FFFCu, 0x12345678u);
    expect_status("jit_width_word_last", run(), QRX_PPC_TRAP);
    expect_u32("jit_width_word_last value", ppc.cpu.gpr[4], 0x12345678u);
    expect_jit_block("jit_width_word_last", word_last_insns);

    prepare(byte_oob, byte_oob_insns);
    expect_status("jit_width_byte_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_byte_oob pc", ppc.cpu.pc, CODE_BASE + 4u);
    expect_u32("jit_width_byte_oob addr", ppc.fault_addr,
               QRX_PPC_DEFAULT_MEM_SIZE);
    expect_u32("jit_width_byte_oob insns", ppc.insns, 2);
    expect_jit_block("jit_width_byte_oob", byte_oob_insns);

    prepare(half_oob, half_oob_insns);
    expect_status("jit_width_half_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_half_oob pc", ppc.cpu.pc, CODE_BASE + 4u);
    expect_u32("jit_width_half_oob addr", ppc.fault_addr,
               QRX_PPC_DEFAULT_MEM_SIZE);
    expect_u32("jit_width_half_oob insns", ppc.insns, 2);
    expect_jit_block("jit_width_half_oob", half_oob_insns);

    prepare(word_oob, word_oob_insns);
    expect_status("jit_width_word_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_word_oob pc", ppc.cpu.pc, CODE_BASE + 4u);
    expect_u32("jit_width_word_oob addr", ppc.fault_addr,
               QRX_PPC_DEFAULT_MEM_SIZE);
    expect_u32("jit_width_word_oob insns", ppc.insns, 2);
    expect_jit_block("jit_width_word_oob", word_oob_insns);

    prepare(stb_oob, stb_oob_insns);
    expect_status("jit_width_stb_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_stb_oob pc", ppc.cpu.pc, CODE_BASE + 8u);
    expect_u32("jit_width_stb_oob addr", ppc.fault_addr,
               QRX_PPC_DEFAULT_MEM_SIZE);
    expect_u32("jit_width_stb_oob insns", ppc.insns, 3);
    expect_jit_block("jit_width_stb_oob", stb_oob_insns);

    prepare(sth_oob, sth_oob_insns);
    expect_status("jit_width_sth_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_sth_oob pc", ppc.cpu.pc, CODE_BASE + 8u);
    expect_u32("jit_width_sth_oob addr", ppc.fault_addr,
               QRX_PPC_DEFAULT_MEM_SIZE);
    expect_u32("jit_width_sth_oob insns", ppc.insns, 3);
    expect_jit_block("jit_width_sth_oob", sth_oob_insns);

    prepare(half_unaligned, unaligned_insns);
    expect_status("jit_width_half_unaligned", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_half_unaligned pc", ppc.cpu.pc, CODE_BASE + 8u);
    expect_u32("jit_width_half_unaligned addr", ppc.fault_addr, 0x3FFFFu);
    expect_u32("jit_width_half_unaligned insns", ppc.insns, 3);
    expect_jit_block("jit_width_half_unaligned", unaligned_insns);
    report("jit_widths", before);
}

/* Use a logical limit below the 256-KiB backing array so a deliberately
 * broken range check can be exercised without touching real out-of-bounds
 * storage.  This keeps the mutation tests safe while still checking the
 * helper's status and fault address. */
static void test_jit_widths_limit(void)
{
    static const uint32_t byte_last[] = {
        0x3C600000u, /* lis r3,0 */
        0x60632FFFu, /* ori r3,r3,0x2FFF */
        0x88830000u, /* lbz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t half_last[] = {
        0x3C600000u, /* lis r3,0 */
        0x60632FFEu, /* ori r3,r3,0x2FFE */
        0xA0830000u, /* lhz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t word_last[] = {
        0x3C600000u, /* lis r3,0 */
        0x60632FFCu, /* ori r3,r3,0x2FFC */
        0x80830000u, /* lwz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t byte_oob[] = {
        0x3C600000u, /* lis r3,0 */
        0x60633000u, /* ori r3,r3,0x3000 */
        0x88830000u, /* lbz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t half_oob[] = {
        0x3C600000u, /* lis r3,0 */
        0x60633000u, /* ori r3,r3,0x3000 */
        0xA0830000u, /* lhz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t word_oob[] = {
        0x3C600000u, /* lis r3,0 */
        0x60633000u, /* ori r3,r3,0x3000 */
        0x80830000u, /* lwz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t stb_oob[] = {
        0x3C600000u, /* lis r3,0 */
        0x60633000u, /* ori r3,r3,0x3000 */
        0x38800012u, /* li r4,0x12 */
        0x98830000u, /* stb r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t sth_oob[] = {
        0x3C600000u, /* lis r3,0 */
        0x60633000u, /* ori r3,r3,0x3000 */
        0x38801234u, /* li r4,0x1234 */
        0xB0830000u, /* sth r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t half_unaligned[] = {
        0x3C600000u, /* lis r3,0 */
        0x60632001u, /* ori r3,r3,0x2001 */
        0xA0830000u, /* lhz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t sth_unaligned[] = {
        0x3C600000u, /* lis r3,0 */
        0x60632001u, /* ori r3,r3,0x2001 */
        0x38801234u, /* li r4,0x1234 */
        0xB0830000u, /* sth r4,0(r3) */
        0x44000002u  /* sc */
    };
    const uint32_t limit = 0x3000u;
    unsigned before = failures;

    prepare(byte_last, sizeof(byte_last) / sizeof(byte_last[0]));
    ppc.mem.size = limit;
    put32(limit - 4u, 0x123456ABu);
    expect_status("jit_width_limit_byte_last", run(), QRX_PPC_TRAP);
    expect_u32("jit_width_limit_byte_last value", ppc.cpu.gpr[4], 0xABu);
    expect_jit_block("jit_width_limit_byte_last",
                     sizeof(byte_last) / sizeof(byte_last[0]));

    prepare(half_last, sizeof(half_last) / sizeof(half_last[0]));
    ppc.mem.size = limit;
    put32(limit - 4u, 0x12345678u);
    expect_status("jit_width_limit_half_last", run(), QRX_PPC_TRAP);
    expect_u32("jit_width_limit_half_last value", ppc.cpu.gpr[4], 0x5678u);
    expect_jit_block("jit_width_limit_half_last",
                     sizeof(half_last) / sizeof(half_last[0]));

    prepare(word_last, sizeof(word_last) / sizeof(word_last[0]));
    ppc.mem.size = limit;
    put32(limit - 4u, 0x12345678u);
    expect_status("jit_width_limit_word_last", run(), QRX_PPC_TRAP);
    expect_u32("jit_width_limit_word_last value", ppc.cpu.gpr[4],
               0x12345678u);
    expect_jit_block("jit_width_limit_word_last",
                     sizeof(word_last) / sizeof(word_last[0]));

    prepare(byte_oob, sizeof(byte_oob) / sizeof(byte_oob[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_byte_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_byte_oob pc", ppc.cpu.pc, CODE_BASE + 8u);
    expect_u32("jit_width_limit_byte_oob addr", ppc.fault_addr, limit);
    expect_u32("jit_width_limit_byte_oob insns", ppc.insns, 3);
    expect_jit_block("jit_width_limit_byte_oob",
                     sizeof(byte_oob) / sizeof(byte_oob[0]));

    prepare(half_oob, sizeof(half_oob) / sizeof(half_oob[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_half_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_half_oob pc", ppc.cpu.pc, CODE_BASE + 8u);
    expect_u32("jit_width_limit_half_oob addr", ppc.fault_addr, limit);
    expect_u32("jit_width_limit_half_oob insns", ppc.insns, 3);
    expect_jit_block("jit_width_limit_half_oob",
                     sizeof(half_oob) / sizeof(half_oob[0]));

    prepare(word_oob, sizeof(word_oob) / sizeof(word_oob[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_word_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_word_oob pc", ppc.cpu.pc, CODE_BASE + 8u);
    expect_u32("jit_width_limit_word_oob addr", ppc.fault_addr, limit);
    expect_u32("jit_width_limit_word_oob insns", ppc.insns, 3);
    expect_jit_block("jit_width_limit_word_oob",
                     sizeof(word_oob) / sizeof(word_oob[0]));

    prepare(stb_oob, sizeof(stb_oob) / sizeof(stb_oob[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_stb_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_stb_oob pc", ppc.cpu.pc, CODE_BASE + 12u);
    expect_u32("jit_width_limit_stb_oob addr", ppc.fault_addr, limit);
    expect_u32("jit_width_limit_stb_oob insns", ppc.insns, 4);
    expect_jit_block("jit_width_limit_stb_oob",
                     sizeof(stb_oob) / sizeof(stb_oob[0]));

    prepare(sth_oob, sizeof(sth_oob) / sizeof(sth_oob[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_sth_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_sth_oob pc", ppc.cpu.pc, CODE_BASE + 12u);
    expect_u32("jit_width_limit_sth_oob addr", ppc.fault_addr, limit);
    expect_u32("jit_width_limit_sth_oob insns", ppc.insns, 4);
    expect_jit_block("jit_width_limit_sth_oob",
                     sizeof(sth_oob) / sizeof(sth_oob[0]));

    prepare(half_unaligned, sizeof(half_unaligned) /
                            sizeof(half_unaligned[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_half_unaligned", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_half_unaligned pc", ppc.cpu.pc,
               CODE_BASE + 8u);
    expect_u32("jit_width_limit_half_unaligned addr", ppc.fault_addr,
               0x2001u);
    expect_u32("jit_width_limit_half_unaligned insns", ppc.insns, 3);
    expect_jit_block("jit_width_limit_half_unaligned",
                     sizeof(half_unaligned) / sizeof(half_unaligned[0]));

    prepare(sth_unaligned, sizeof(sth_unaligned) /
                            sizeof(sth_unaligned[0]));
    ppc.mem.size = limit;
    expect_status("jit_width_limit_sth_unaligned", run(), QRX_PPC_FAULT);
    expect_u32("jit_width_limit_sth_unaligned pc", ppc.cpu.pc,
               CODE_BASE + 12u);
    expect_u32("jit_width_limit_sth_unaligned addr", ppc.fault_addr,
               0x2001u);
    expect_u32("jit_width_limit_sth_unaligned insns", ppc.insns, 4);
    expect_jit_block("jit_width_limit_sth_unaligned",
                     sizeof(sth_unaligned) / sizeof(sth_unaligned[0]));
    report("jit_widths_limit", before);
}

static void test_jit_branch(void)
{
    static const uint32_t taken[] = {
        0x38600001u, /* li r3,1 */
        0x38800001u, /* li r4,1 */
        0x7C232000u, /* cmpw r3,r4 */
        0x4182000Cu, /* beq +12 -> taken path */
        0x38A00007u, /* li r5,7 (fallthrough) */
        0x44000002u, /* sc */
        0x38A0002Au, /* li r5,42 */
        0x38C02000u, /* li r6,0x2000 */
        0x90A60000u, /* stw r5,0(r6) */
        0x44000002u  /* sc */
    };
    static const uint32_t not_taken[] = {
        0x38600001u, /* li r3,1 */
        0x38800002u, /* li r4,2 */
        0x7C232000u, /* cmpw r3,r4 */
        0x4182000Cu, /* beq +12 -> taken path */
        0x38A00007u, /* li r5,7 (fallthrough) */
        0x44000002u  /* sc */
    };
    static const uint32_t cr_field[] = {
        0x38600005u, /* li r3,5 */
        0x38800005u, /* li r4,5 */
        0x2C830005u, /* cmpwi cr1,r3,5 */
        0x4186000Cu, /* beq cr1,+12 -> taken path */
        0x38A00007u, /* li r5,7 (fallthrough) */
        0x44000002u, /* sc */
        0x38A0002Au, /* li r5,42 */
        0x38C02000u, /* li r6,0x2000 */
        0x90A60000u, /* stw r5,0(r6) */
        0x44000002u  /* sc */
    };
    static const uint32_t link[] = {
        0x42800009u, /* bcl 20,0,+8 */
        0x38A00063u, /* li r5,99 (skipped) */
        0x38A0002Au, /* li r5,42 */
        0x7CC802A6u, /* mfspr r6,LR */
        0x38E02000u, /* li r7,0x2000 */
        0x90C70000u, /* stw r6,0(r7) */
        0x44000002u  /* sc */
    };
    static const uint32_t ctr_loop[] = {
        0x38600000u, /* li r3,0 */
        0x38800003u, /* li r4,3 */
        0x7C8903A6u, /* mtctr r4 */
        0x38630001u, /* addi r3,r3,1 */
        0x42003FFCu, /* bdnz loop */
        0x38C02000u, /* li r6,0x2000 */
        0x90660000u, /* stw r3,0(r6) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(taken, sizeof(taken) / sizeof(taken[0]));
    expect_status("jit_branch_taken", run(), QRX_PPC_TRAP);
    expect_u32("jit_branch_taken r5", ppc.cpu.gpr[5], 42);
    expect_u32("jit_branch_taken cr0", (ppc.cpu.cr >> 28) & 0xFu, 2u);
    expect_u32("jit_branch_taken mem", get32(DATA_BASE), 42);
    expect_u32("jit_branch_taken pc", ppc.cpu.pc, CODE_BASE + 40u);
    expect_jit_source("jit_branch_taken", 0x4182000Cu);

    prepare(not_taken, sizeof(not_taken) / sizeof(not_taken[0]));
    expect_status("jit_branch_not_taken", run(), QRX_PPC_TRAP);
    expect_u32("jit_branch_not_taken r5", ppc.cpu.gpr[5], 7);
    expect_u32("jit_branch_not_taken mem", get32(DATA_BASE), 0u);
    expect_u32("jit_branch_not_taken pc", ppc.cpu.pc, CODE_BASE + 24u);
    expect_jit_source("jit_branch_not_taken", 0x4182000Cu);

    prepare(cr_field, sizeof(cr_field) / sizeof(cr_field[0]));
    expect_status("jit_branch_cr1", run(), QRX_PPC_TRAP);
    expect_u32("jit_branch_cr1 r5", ppc.cpu.gpr[5], 42);
    expect_u32("jit_branch_cr1 cr1", (ppc.cpu.cr >> 24) & 0xFu, 2u);
    expect_u32("jit_branch_cr1 pc", ppc.cpu.pc, CODE_BASE + 40u);
    expect_jit_source("jit_branch_cr1", 0x4186000Cu);

    prepare(link, sizeof(link) / sizeof(link[0]));
    expect_status("jit_branch_link", run(), QRX_PPC_TRAP);
    expect_u32("jit_branch_link r5", ppc.cpu.gpr[5], 42);
    expect_u32("jit_branch_link lr", ppc.cpu.lr, 0x1004u);
    expect_u32("jit_branch_link r6", ppc.cpu.gpr[6], 0x1004u);
    expect_u32("jit_branch_link mem", get32(DATA_BASE), 0x1004u);
    expect_jit_source("jit_branch_link", 0x42800009u);

    prepare(ctr_loop, sizeof(ctr_loop) / sizeof(ctr_loop[0]));
    expect_status("jit_branch_ctr", run(), QRX_PPC_TRAP);
    expect_u32("jit_branch_ctr r3", ppc.cpu.gpr[3], 3);
    expect_u32("jit_branch_ctr ctr", ppc.cpu.ctr, 0);
    expect_u32("jit_branch_ctr mem", get32(DATA_BASE), 3);
    expect_jit_source("jit_branch_ctr", 0x42003FFCu);
    report("jit_branch", before);
}

static uint32_t get32_native(uint32_t addr)
{
    return ((uint32_t)memory_jit[addr + 0] << 24) |
           ((uint32_t)memory_jit[addr + 1] << 16) |
           ((uint32_t)memory_jit[addr + 2] << 8) |
           (uint32_t)memory_jit[addr + 3];
}

/* After the first run has translated all three blocks, a direct and a
 * conditional branch must both tail-jump into their target: the second run
 * has to enter only the first block natively, translate nothing and still
 * execute all eight instructions. */
static void test_jit_chain(void)
{
    static const uint32_t code[] = {
        0x38600001u, /* 0x1000 li r3,1 */
        0x48000008u, /* 0x1004 b +8 -> 0x100c */
        0x38A00063u, /* 0x1008 li r5,99 (skipped) */
        0x2C030001u, /* 0x100c cmpwi r3,1 */
        0x41820008u, /* 0x1010 beq +8 -> 0x1018 */
        0x38A00063u, /* 0x1014 li r5,99 (not taken) */
        0x38A00007u, /* 0x1018 li r5,7 (taken target) */
        0x38C02000u, /* 0x101c li r6,0x2000 */
        0x90660000u, /* 0x1020 stw r3,0(r6) */
        0x44000002u  /* 0x1024 sc */
    };
    qrx_ppc_t expected;
    qrx_ppc_t native;
    uint32_t hits_before;
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    expect_status("jit_chain_interp", qrx_ppc_run(&ppc, 10000), QRX_PPC_TRAP);
    expected = ppc;
    expect_u32("jit_chain_interp r5", ppc.cpu.gpr[5], 7u);
    expect_u32("jit_chain_interp mem", get32(DATA_BASE), 1u);

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_chain_first", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_first blocks", jit.nblocks, 3u);
    expect_u32("jit_chain_first misses", jit.misses, 3u);
    expect_u32("jit_chain_first hits", jit.hits, 0u);
    expect_u32("jit_chain_first insns", native.insns, 8u);
    expect_u32("jit_chain_first pc", native.cpu.pc, expected.cpu.pc);
    expect_u32("jit_chain_first r5", native.cpu.gpr[5], 7u);
    expect_u32("jit_chain_first mem", get32_native(DATA_BASE), 1u);
    hits_before = jit.hits;

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    expect_status("jit_chain_linked", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_linked entries", jit.hits - hits_before, 1u);
    expect_u32("jit_chain_linked misses", jit.misses, 3u);
    expect_u32("jit_chain_linked insns", native.insns, 8u);
    expect_u32("jit_chain_linked pc", native.cpu.pc, expected.cpu.pc);
    expect_u32("jit_chain_linked gpr3", native.cpu.gpr[3],
               expected.cpu.gpr[3]);
    expect_u32("jit_chain_linked gpr5", native.cpu.gpr[5],
               expected.cpu.gpr[5]);
    expect_u32("jit_chain_linked cr", native.cpu.cr, expected.cpu.cr);
    expect_u32("jit_chain_linked mem", get32_native(DATA_BASE), 1u);
    report("jit_chain", before);
}

/* bclr: a subroutine returns through LR, so its target is only known at run
 * time and has to go through the indirect target cache. */
static void test_jit_chain_blr(void)
{
    static const uint32_t code[] = {
        0x38800002u, /* 0x1000 li r4,2 */
        0x38600000u, /* 0x1004 li r3,0 */
        0x48000021u, /* 0x1008 bl 0x1028 */
        0x3884FFFFu, /* 0x100c addi r4,r4,-1 */
        0x2C040000u, /* 0x1010 cmpwi r4,0 */
        0x4082FFF4u, /* 0x1014 bne 0x1008 */
        0x38C02000u, /* 0x1018 li r6,0x2000 */
        0x90660000u, /* 0x101c stw r3,0(r6) */
        0x44000002u, /* 0x1020 sc */
        0x00000000u, /* 0x1024 padding */
        0x38630001u, /* 0x1028 addi r3,r3,1 */
        0x4E800020u  /* 0x102c blr */
    };
    qrx_ppc_t native;
    uint32_t hits_before;
    uint32_t misses_before;
    uint32_t cache_before;
    unsigned before = failures;

    /* Both native runs use memory_jit, so `memory` stays the pristine image
     * for the interpreter reference run at the end. */
    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_chain_blr_first", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_blr_blocks", jit.nblocks, 5u);
    expect_u32("jit_chain_blr_first cache", jit.cache_hits, 1u);
    expect_u32("jit_chain_blr_first insns", native.insns, 17u);
    expect_u32("jit_chain_blr_first mem", get32_native(DATA_BASE), 2u);

    hits_before = jit.hits;
    misses_before = jit.misses;
    cache_before = jit.cache_hits;
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    expect_status("jit_chain_blr_linked", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_blr_entries", jit.hits - hits_before, 1u);
    expect_u32("jit_chain_blr_no_misses", jit.misses - misses_before, 0u);
    expect_u32("jit_chain_blr_cache", jit.cache_hits - cache_before, 2u);
    expect_u32("jit_chain_blr_insns", native.insns, 17u);

    expect_status("jit_chain_blr_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_blr_interp r3", ppc.cpu.gpr[3], 2u);
    expect_u32("jit_chain_blr_interp lr", ppc.cpu.lr, 0x100cu);
    expect_u32("jit_chain_blr_interp mem", get32(DATA_BASE), 2u);
    compare_jit_state(&ppc, &native);
    report("jit_chain_blr", before);
}

/* bcctr: the target comes from CTR, which the interpreter sets from mtctr in
 * front of the native block.  The block after it must chain into that target. */
static void test_jit_chain_bcctr(void)
{
    static const uint32_t code[] = {
        0x38801024u, /* 0x1000 li r4,0x1024 */
        0x7C8903A6u, /* 0x1004 mtctr r4 (interpreter) */
        0x38600005u, /* 0x1008 li r3,5 */
        0x4E800420u, /* 0x100c bcctr 20,0 -> CTR */
        0x38A00063u, /* 0x1010 li r5,99 (skipped) */
        0x00000000u, /* 0x1014 padding */
        0x00000000u, /* 0x1018 padding */
        0x00000000u, /* 0x101c padding */
        0x00000000u, /* 0x1020 padding */
        0x38C02000u, /* 0x1024 li r6,0x2000 */
        0x90660000u, /* 0x1028 stw r3,0(r6) */
        0x44000002u  /* 0x102c sc */
    };
    qrx_ppc_t native;
    uint32_t hits_before;
    uint32_t misses_before;
    uint32_t cache_before;
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_chain_bcctr_first", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_bcctr_blocks", jit.nblocks, 3u);
    expect_u32("jit_chain_bcctr_first insns", native.insns, 7u);
    expect_u32("jit_chain_bcctr_first mem", get32_native(DATA_BASE), 5u);

    hits_before = jit.hits;
    misses_before = jit.misses;
    cache_before = jit.cache_hits;
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    expect_status("jit_chain_bcctr_linked", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    /* 0x1000 and 0x1008 are separate entries because mtctr stays interpreted;
     * a missing indirect chain would add a third entry for 0x1024. */
    expect_u32("jit_chain_bcctr_entries", jit.hits - hits_before, 2u);
    expect_u32("jit_chain_bcctr_no_misses", jit.misses - misses_before, 0u);
    expect_u32("jit_chain_bcctr_cache", jit.cache_hits - cache_before, 1u);
    expect_u32("jit_chain_bcctr_insns", native.insns, 7u);

    expect_status("jit_chain_bcctr_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_bcctr_interp r3", ppc.cpu.gpr[3], 5u);
    expect_u32("jit_chain_bcctr_interp r5", ppc.cpu.gpr[5], 0u);
    expect_u32("jit_chain_bcctr_interp mem", get32(DATA_BASE), 5u);
    compare_jit_state(&ppc, &native);
    report("jit_chain_bcctr", before);
}

/* A not-taken bclr has to continue at the fallthrough and still chain there. */
static void test_jit_chain_indirect_not_taken(void)
{
    static const uint32_t code[] = {
        0x38600001u, /* 0x1000 li r3,1 */
        0x38800002u, /* 0x1004 li r4,2 */
        0x7C232000u, /* 0x1008 cmpw r3,r4 (LT, EQ clear) */
        0x4D820020u, /* 0x100c beqlr, not taken */
        0x38A00007u, /* 0x1010 li r5,7 */
        0x44000002u  /* 0x1014 sc */
    };
    qrx_ppc_t native;
    uint32_t hits_before;
    uint32_t misses_before;
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_chain_beqlr_first", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_beqlr_blocks", jit.nblocks, 2u);
    expect_u32("jit_chain_beqlr_first insns", native.insns, 6u);

    hits_before = jit.hits;
    misses_before = jit.misses;
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    expect_status("jit_chain_beqlr_linked", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_beqlr_entries", jit.hits - hits_before, 1u);
    expect_u32("jit_chain_beqlr_no_misses", jit.misses - misses_before, 0u);
    expect_u32("jit_chain_beqlr_insns", native.insns, 6u);

    expect_status("jit_chain_beqlr_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_beqlr_interp r5", ppc.cpu.gpr[5], 7u);
    expect_u32("jit_chain_beqlr_interp pc", ppc.cpu.pc, 0x1018u);
    compare_jit_state(&ppc, &native);
    report("jit_chain_beqlr", before);
}
/* Two return sites (0x1010 after the first bl, 0x1014 after the second, i.e.
 * sets 4 and 5): both cache entries stay resident, so the second run is fully
 * native and every indirect return is a hit.  The same-set thrash test below
 * covers three sites competing for one set. */
static void test_jit_chain_cache_2way(void)
{
    static const uint32_t code[] = {
        0x38600000u, /* 0x1000 li r3,0 */
        0x38800000u, /* 0x1004 li r4,0 */
        0x38A00004u, /* 0x1008 li r5,4 */
        0x4800002Du, /* 0x100c bl 0x1038 */
        0x48000069u, /* 0x1010 bl 0x1078 */
        0x38A5FFFFu, /* 0x1014 addi r5,r5,-1 */
        0x2C050000u, /* 0x1018 cmpwi r5,0 */
        0x4082FFF0u, /* 0x101c bne 0x100c */
        0x38C02000u, /* 0x1020 li r6,0x2000 */
        0x90660000u, /* 0x1024 stw r3,0(r6) */
        0x90860004u, /* 0x1028 stw r4,4(r6) */
        0x44000002u, /* 0x102c sc */
        0x00000000u, /* 0x1030 padding */
        0x00000000u, /* 0x1034 padding */
        0x38630001u, /* 0x1038 addi r3,r3,1 */
        0x4E800020u, /* 0x103c blr -> 0x1010 */
        0x00000000u, /* 0x1040 padding */
        0x00000000u, /* 0x1044 padding */
        0x00000000u, /* 0x1048 padding */
        0x00000000u, /* 0x104c padding */
        0x00000000u, /* 0x1050 padding */
        0x00000000u, /* 0x1054 padding */
        0x00000000u, /* 0x1058 padding */
        0x00000000u, /* 0x105c padding */
        0x00000000u, /* 0x1060 padding */
        0x00000000u, /* 0x1064 padding */
        0x00000000u, /* 0x1068 padding */
        0x00000000u, /* 0x106c padding */
        0x00000000u, /* 0x1070 padding */
        0x00000000u, /* 0x1074 padding */
        0x38840001u, /* 0x1078 addi r4,r4,1 */
        0x4E800020u  /* 0x107c blr -> 0x1014 */
    };
    qrx_ppc_t native;
    uint32_t hits_before;
    uint32_t misses_before;
    uint32_t cache_before;
    uint32_t cache_miss_before;
    unsigned i;
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_chain_2way_first", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_2way_blocks", jit.nblocks, 7u);
    expect_u32("jit_chain_2way_first hits", jit.cache_hits, 6u);
    expect_u32("jit_chain_2way_first misses", jit.cache_misses, 2u);
    expect_u32("jit_chain_2way_first insns", native.insns, 43u);
    expect_u32("jit_chain_2way_first mem", get32_native(DATA_BASE), 4u);
    expect_u32("jit_chain_2way_first mem4", get32_native(DATA_BASE + 4u), 4u);

    hits_before = jit.hits;
    misses_before = jit.misses;
    cache_before = jit.cache_hits;
    cache_miss_before = jit.cache_misses;
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    expect_status("jit_chain_2way_linked", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_2way_entries", jit.hits - hits_before, 1u);
    expect_u32("jit_chain_2way_no_misses", jit.misses - misses_before, 0u);
    /* Eight indirect returns per run and all of them hits: both targets share
     * a set and still stay resident, where a 1-way cache would miss each. */
    expect_u32("jit_chain_2way_hits", jit.cache_hits - cache_before, 8u);
    expect_u32("jit_chain_2way_no_cache_misses",
               jit.cache_misses - cache_miss_before, 0u);
    expect_u32("jit_chain_2way_insns", native.insns, 43u);

    /* Interleaved lookups keep both ways usable: neither target evicts the
     * other, which a directly mapped cache cannot do. */
    cache_before = jit.cache_hits;
    cache_miss_before = jit.cache_misses;
    for (i = 0; i < 4; i++) {
        expect_u32("jit_chain_2way_keep_a",
                   qrx_ppc_jit_chain_target(&jit, 0x1038u) != 0, 1u);
        expect_u32("jit_chain_2way_keep_b",
                   qrx_ppc_jit_chain_target(&jit, 0x1078u) != 0, 1u);
    }
    expect_u32("jit_chain_2way_keep_hits", jit.cache_hits - cache_before, 8u);
    expect_u32("jit_chain_2way_keep_misses",
               jit.cache_misses - cache_miss_before, 0u);

    expect_status("jit_chain_2way_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_2way_interp r3", ppc.cpu.gpr[3], 4u);
    expect_u32("jit_chain_2way_interp r4", ppc.cpu.gpr[4], 4u);
    expect_u32("jit_chain_2way_interp lr", ppc.cpu.lr, 0x1014u);
    expect_u32("jit_chain_2way_interp pc", ppc.cpu.pc, 0x1030u);
    compare_jit_state(&ppc, &native);
    report("jit_chain_cache_2way", before);
}
/* Three return targets in one cache set (0x10B8, 0x10F8 and 0x1138 all map to
 * set 6).  Two ways cannot keep all three, so every cycle evicts the least
 * recently used target; the evicted target has to come back through the
 * runner and must still produce exactly the interpreter's result. */
/* Three return sites in one cache set: the bclr targets are the addresses
 * after each bl, and 0x1038, 0x1078 and 0x10B8 all map to set 6.  Two ways
 * cannot hold all three, so the alternating calls keep evicting the least
 * recently used return site; every evicted site has to come back through the
 * runner and must still produce exactly the interpreter's result. */
/* Three return sites in one cache set: a bclr target is the address after its
 * bl, and 0x1038, 0x1078 and 0x10B8 all map to set 6.  Two ways cannot hold
 * all three, so A is called twice in a row (always a hit), while B and C keep
 * evicting the least recently used site.  Every evicted site has to come back
 * through the runner and must still produce the interpreter's result. */
static void test_jit_chain_cache_thrash(void)
{
    static const uint32_t code[] = {
        0x38600000u, /* 0x1000 li r3,0 */
        0x38800000u, /* 0x1004 li r4,0 */
        0x38A00000u, /* 0x1008 li r5,0 */
        0x38C00004u, /* 0x100c li r6,4 */
        0x38E00002u, /* 0x1010 li r7,2 */
        0x48000020u, /* 0x1014 b 0x1034 */
        0x00000000u, /* 0x1018 padding */
        0x00000000u, /* 0x101c padding */
        0x00000000u, /* 0x1020 padding */
        0x00000000u, /* 0x1024 padding */
        0x00000000u, /* 0x1028 padding */
        0x00000000u, /* 0x102c padding */
        0x00000000u, /* 0x1030 padding */
        0x480000CDu, /* 0x1034 bl 0x1100 (A), ret 0x1038 */
        0x38E7FFFFu, /* 0x1038 addi r7,r7,-1 */
        0x2C070000u, /* 0x103c cmpwi r7,0 */
        0x4082FFF4u, /* 0x1040 bne 0x1034 */
        0x48000030u, /* 0x1044 b 0x1074 */
        0x00000000u, /* 0x1048 padding */
        0x00000000u, /* 0x104c padding */
        0x00000000u, /* 0x1050 padding */
        0x00000000u, /* 0x1054 padding */
        0x00000000u, /* 0x1058 padding */
        0x00000000u, /* 0x105c padding */
        0x00000000u, /* 0x1060 padding */
        0x00000000u, /* 0x1064 padding */
        0x00000000u, /* 0x1068 padding */
        0x00000000u, /* 0x106c padding */
        0x00000000u, /* 0x1070 padding */
        0x48000095u, /* 0x1074 bl 0x1108 (B), ret 0x1078 */
        0x4800003Cu, /* 0x1078 b 0x10B4 */
        0x00000000u, /* 0x107c padding */
        0x00000000u, /* 0x1080 padding */
        0x00000000u, /* 0x1084 padding */
        0x00000000u, /* 0x1088 padding */
        0x00000000u, /* 0x108c padding */
        0x00000000u, /* 0x1090 padding */
        0x00000000u, /* 0x1094 padding */
        0x00000000u, /* 0x1098 padding */
        0x00000000u, /* 0x109c padding */
        0x00000000u, /* 0x10a0 padding */
        0x00000000u, /* 0x10a4 padding */
        0x00000000u, /* 0x10a8 padding */
        0x00000000u, /* 0x10ac padding */
        0x00000000u, /* 0x10b0 padding */
        0x4800005Du, /* 0x10b4 bl 0x1110 (C), ret 0x10B8 */
        0x38E00002u, /* 0x10b8 li r7,2 */
        0x38C6FFFFu, /* 0x10bc addi r6,r6,-1 */
        0x2C060000u, /* 0x10c0 cmpwi r6,0 */
        0x4082FF70u, /* 0x10c4 bne 0x1034 */
        0x39002000u, /* 0x10c8 li r8,0x2000 */
        0x90680000u, /* 0x10cc stw r3,0(r8) */
        0x90880004u, /* 0x10d0 stw r4,4(r8) */
        0x90A80008u, /* 0x10d4 stw r5,8(r8) */
        0x44000002u, /* 0x10d8 sc */
        0x00000000u, /* 0x10dc padding */
        0x00000000u, /* 0x10e0 padding */
        0x00000000u, /* 0x10e4 padding */
        0x00000000u, /* 0x10e8 padding */
        0x00000000u, /* 0x10ec padding */
        0x00000000u, /* 0x10f0 padding */
        0x00000000u, /* 0x10f4 padding */
        0x00000000u, /* 0x10f8 padding */
        0x00000000u, /* 0x10fc padding */
        0x38630001u, /* 0x1100 addi r3,r3,1 */
        0x4E800020u, /* 0x1104 blr */
        0x38840001u, /* 0x1108 addi r4,r4,1 */
        0x4E800020u, /* 0x110c blr */
        0x38A50001u, /* 0x1110 addi r5,r5,1 */
        0x4E800020u, /* 0x1114 blr */
    };
    qrx_ppc_t native;
    uint32_t hits_before;
    uint32_t cache_before;
    uint32_t cache_miss_before;
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_chain_thrash_first", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_thrash_blocks", jit.nblocks, 12u);
    expect_u32("jit_chain_thrash_first insns", native.insns, 107u);
    expect_u32("jit_chain_thrash_first mem", get32_native(DATA_BASE), 8u);
    expect_u32("jit_chain_thrash_first mem4", get32_native(DATA_BASE + 4u), 4u);
    expect_u32("jit_chain_thrash_first mem8", get32_native(DATA_BASE + 8u), 4u);

    hits_before = jit.hits;
    cache_before = jit.cache_hits;
    cache_miss_before = jit.cache_misses;
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    expect_status("jit_chain_thrash_linked", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_thrash_insns", native.insns, 107u);
    /* Four returns per iteration (A A B C): the repeated A always hits, while
     * B and C compete with A for the two ways of set 6. */
    expect_u32("jit_chain_thrash_lookups",
               (jit.cache_hits + jit.cache_misses) -
               (cache_before + cache_miss_before), 16u);
    expect_u32("jit_chain_thrash_hits", jit.cache_hits - cache_before, 4u);
    expect_u32("jit_chain_thrash_misses",
               (jit.cache_misses - cache_miss_before) > 0, 1u);
    /* Each eviction leaves through the runner and re-enters exactly once, so
     * the extra native entries match the misses: an evicted entry never runs
     * stale code. */
    expect_u32("jit_chain_thrash_entries", jit.hits - hits_before,
               1u + (jit.cache_misses - cache_miss_before));

    expect_status("jit_chain_thrash_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_chain_thrash_interp r3", ppc.cpu.gpr[3], 8u);
    expect_u32("jit_chain_thrash_interp r4", ppc.cpu.gpr[4], 4u);
    expect_u32("jit_chain_thrash_interp r5", ppc.cpu.gpr[5], 4u);
    expect_u32("jit_chain_thrash_interp pc", ppc.cpu.pc, 0x10DCu);
    compare_jit_state(&ppc, &native);
    report("jit_chain_cache_thrash", before);
}
/* Mid-block SMC: a store inside a block rewrites an instruction that the same
 * block would execute next.  The store must end the block, because the block
 * still holds the old translation of that instruction; the runner then
 * re-checks the bytes and translates the successor anew. */
static void test_jit_smc_midblock(void)
{
    static const uint32_t overlap[] = {
        0x38600000u, /* 0x1000 li r3,0 */
        0x3CC03863u, /* 0x1004 lis r6,0x3863 */
        0x60C6000Au, /* 0x1008 ori r6,r6,0x000a */
        0x38E01014u, /* 0x100c li r7,0x1014 */
        0x90C70000u, /* 0x1010 stw r6,0(r7) patches 0x1014 */
        0x38630001u, /* 0x1014 addi r3,r3,1 (becomes +10) */
        0x39002000u, /* 0x1018 li r8,0x2000 */
        0x90680000u, /* 0x101c stw r3,0(r8) */
        0x44000002u  /* 0x1020 sc */
    };
    static const uint32_t disjoint[] = {
        0x38600000u, /* 0x1000 li r3,0 */
        0x3CC03863u, /* 0x1004 lis r6,0x3863 */
        0x60C6000Au, /* 0x1008 ori r6,r6,0x000a */
        0x38E02000u, /* 0x100c li r7,0x2000 */
        0x90C70000u, /* 0x1010 stw r6,0(r7) outside the block */
        0x39002004u, /* 0x1014 li r8,0x2004 */
        0x90680000u, /* 0x1018 stw r3,0(r8) */
        0x44000002u  /* 0x101c sc */
    };
    qrx_ppc_t native;
    unsigned before = failures;

    prepare(overlap, sizeof(overlap) / sizeof(overlap[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_smc_overlap", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    /* Five instructions, then the patched block at 0x1014: without the store
     * check the stale +1 would run and the block count would stay at one. */
    expect_u32("jit_smc_overlap blocks", jit.nblocks, 2u);
    expect_u32("jit_smc_overlap insns", native.insns, 9u);
    expect_u32("jit_smc_overlap mem", get32_native(DATA_BASE), 10u);

    expect_status("jit_smc_overlap_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_smc_overlap_interp r3", ppc.cpu.gpr[3], 10u);
    expect_u32("jit_smc_overlap_interp mem", get32(DATA_BASE), 10u);
    compare_jit_state(&ppc, &native);

    /* A store outside the block must not end it: one block, one native
     * entry. */
    prepare(disjoint, sizeof(disjoint) / sizeof(disjoint[0]));
    memcpy(memory_jit, memory, sizeof(memory));
    native = ppc;
    native.mem.data = memory_jit;
    qrx_ppc_jit_reset(&jit);
    expect_status("jit_smc_disjoint", qrx_ppc_run_jit(&native, &jit, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_smc_disjoint blocks", jit.nblocks, 1u);
    expect_u32("jit_smc_disjoint insns", native.insns, 8u);
    expect_u32("jit_smc_disjoint mem", get32_native(DATA_BASE), 0x3863000Au);
    expect_u32("jit_smc_disjoint mem4", get32_native(DATA_BASE + 4u), 0u);

    expect_status("jit_smc_disjoint_interp", qrx_ppc_run(&ppc, 10000),
                  QRX_PPC_TRAP);
    expect_u32("jit_smc_disjoint_interp mem", get32(DATA_BASE), 0x3863000Au);
    compare_jit_state(&ppc, &native);
    report("jit_smc_midblock", before);
}
#endif

static void test_jit_faults(void)
{
    static const uint32_t oob[] = {
        0x3C600004u, /* lis r3,4 -> 0x40000 */
        0x80830000u, /* lwz r4,0(r3) */
        0x44000002u  /* sc */
    };
    static const uint32_t unaligned[] = {
        0x38602002u, /* li r3,0x2002 */
        0x38800001u, /* li r4,1 */
        0x90830000u, /* stw r4,0(r3) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(oob, sizeof(oob) / sizeof(oob[0]));
    expect_status("jit_fault_oob", run(), QRX_PPC_FAULT);
    expect_u32("jit_fault_oob pc", ppc.cpu.pc, 0x1004u);
    expect_u32("jit_fault_oob addr", ppc.fault_addr, 0x40000u);
    expect_u32("jit_fault_oob insns", ppc.insns, 2);
    report("jit_fault_oob", before);

    before = failures;
    prepare(unaligned, sizeof(unaligned) / sizeof(unaligned[0]));
    expect_status("jit_fault_unaligned", run(), QRX_PPC_FAULT);
    expect_u32("jit_fault_unaligned pc", ppc.cpu.pc, 0x1008u);
    expect_u32("jit_fault_unaligned addr", ppc.fault_addr, 0x2002u);
    expect_u32("jit_fault_unaligned insns", ppc.insns, 3);
    report("jit_fault_unaligned", before);
}

static void test_fpu_double(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xC8402108u, /* lfd f2,0x2108(0) */
        0xEC61102Au, /* fadd f3,f1,f2 */
        0xD8602110u, /* stfd f3,0x2110(0) */
        0xFC830800u, /* fcmpu cr1,f3,f1 */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    put64(0x2100, double_bits(1.5));
    put64(0x2108, double_bits(2.25));
    expect_status("fpu_double", run(), QRX_PPC_TRAP);
    expect_u64("fpu_double f1", ppc.cpu.fpr[1], double_bits(1.5));
    expect_u64("fpu_double f2", ppc.cpu.fpr[2], double_bits(2.25));
    expect_u64("fpu_double f3", ppc.cpu.fpr[3], double_bits(3.75));
    expect_u64("fpu_double mem", get64(0x2110), double_bits(3.75));
    expect_u32("fpu_double cr1", (ppc.cpu.cr >> 24) & 0xFu, 4);
    report("fpu_double", before);
}

static void test_fpu_single(void)
{
    static const uint32_t code[] = {
        0xC0202100u, /* lfs f1,0x2100(0) */
        0xC0402104u, /* lfs f2,0x2104(0) */
        0xEC611028u, /* fsub f3,f1,f2 */
        0xD0602110u, /* stfs f3,0x2110(0) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    put32(0x2100, 0x40B00000u); /* 5.5f */
    put32(0x2104, 0x40000000u); /* 2.0f */
    expect_status("fpu_single", run(), QRX_PPC_TRAP);
    expect_u64("fpu_single f3", ppc.cpu.fpr[3], double_bits(3.5));
    expect_u32("fpu_single mem", get32(0x2110), 0x40600000u); /* 3.5f */
    report("fpu_single", before);
}

static void test_fpu_compare(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xC8402108u, /* lfd f2,0x2108(0) */
        0xFD011000u, /* fcmpu cr2,f1,f2 */
        0xFD820840u, /* fcmpo cr3,f2,f1 */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    put64(0x2100, double_bits(2.0));
    put64(0x2108, double_bits(3.0));
    expect_status("fpu_compare", run(), QRX_PPC_TRAP);
    expect_u32("fpu_compare cr2", (ppc.cpu.cr >> 20) & 0xFu, 8);
    expect_u32("fpu_compare cr3", (ppc.cpu.cr >> 16) & 0xFu, 4);
    report("fpu_compare", before);
}

static void test_fpu_mul_div(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xC8402108u, /* lfd f2,0x2108(0) */
        0xEC611032u, /* fmul f3,f1,f2 */
        0xEC831024u, /* fdiv f4,f3,f2 */
        0xD8802110u, /* stfd f4,0x2110(0) */
        0xFC840800u, /* fcmpu cr1,f4,f1 */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    put64(0x2100, double_bits(6.0));
    put64(0x2108, double_bits(2.0));
    expect_status("fpu_muldiv", run(), QRX_PPC_TRAP);
    expect_u64("fpu_muldiv f3", ppc.cpu.fpr[3], double_bits(12.0));
    expect_u64("fpu_muldiv f4", ppc.cpu.fpr[4], double_bits(6.0));
    expect_u64("fpu_muldiv mem", get64(0x2110), double_bits(6.0));
    expect_u32("fpu_muldiv cr1", (ppc.cpu.cr >> 24) & 0xFu, 2);
    report("fpu_mul_div", before);
}

static void test_fpu_round_modes(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xC8402108u, /* lfd f2,0x2108(0) */
        0xEC61102Au, /* fadd f3,f1,f2 */
        0xD8602110u, /* stfd f3,0x2110(0) */
        0x44000002u  /* sc */
    };
    static const uint64_t pos_expected[4] = {
        0x3FF0000000000000ull, 0x3FF0000000000000ull,
        0x3FF0000000000001ull, 0x3FF0000000000000ull
    };
    static const uint64_t neg_expected[4] = {
        0xBFF0000000000000ull, 0xBFF0000000000000ull,
        0xBFF0000000000000ull, 0xBFF0000000000001ull
    };
    unsigned before = failures;
    unsigned mode;

    for (mode = 0; mode < 4; mode++) {
        prepare(code, sizeof(code) / sizeof(code[0]));
        put64(0x2100, double_bits(1.0));
        put64(0x2108, 0x3CA0000000000000ull); /* 2^-53 */
        expect_status("round_pos", run_fpscr(mode), QRX_PPC_TRAP);
        expect_u64("round_pos f3", ppc.cpu.fpr[3], pos_expected[mode]);
        expect_u64("round_pos mem", get64(0x2110), pos_expected[mode]);
        expect_u32("round_pos flags", ppc.cpu.fpscr & 0xC2000000u, 0xC2000000u);

        prepare(code, sizeof(code) / sizeof(code[0]));
        put64(0x2100, double_bits(-1.0));
        put64(0x2108, 0xBCA0000000000000ull); /* -2^-53 */
        expect_status("round_neg", run_fpscr(mode), QRX_PPC_TRAP);
        expect_u64("round_neg f3", ppc.cpu.fpr[3], neg_expected[mode]);
        expect_u64("round_neg mem", get64(0x2110), neg_expected[mode]);
        expect_u32("round_neg flags", ppc.cpu.fpscr & 0xC2000000u, 0xC2000000u);
    }
    report("fpu_round_modes", before);
}

static void test_fpu_frsp(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xFC400818u, /* frsp f2,f1 */
        0xD8402110u, /* stfd f2,0x2110(0) */
        0x44000002u  /* sc */
    };
    static const uint64_t expected[4] = {
        0x3FF0000000000000ull, 0x3FF0000000000000ull,
        0x3FF0000020000000ull, 0x3FF0000000000000ull
    };
    unsigned before = failures;
    unsigned mode;

    for (mode = 0; mode < 4; mode++) {
        prepare(code, sizeof(code) / sizeof(code[0]));
        put64(0x2100, 0x3FF0000010000000ull); /* 1 + 2^-24 */
        expect_status("frsp", run_fpscr(mode), QRX_PPC_TRAP);
        expect_u64("frsp f2", ppc.cpu.fpr[2], expected[mode]);
        expect_u64("frsp mem", get64(0x2110), expected[mode]);
        expect_u32("frsp flags", ppc.cpu.fpscr & 0xC2000000u, 0xC2000000u);
    }
    report("fpu_frsp", before);
}

static void test_fpu_fctiw(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xFC40081Cu, /* fctiw f2,f1 */
        0xFC60081Eu, /* fctiwz f3,f1 */
        0xD8402110u, /* stfd f2,0x2110(0) */
        0xD8602118u, /* stfd f3,0x2118(0) */
        0x44000002u  /* sc */
    };
    static const uint32_t pos_expected[4] = { 2, 2, 3, 2 };
    static const uint32_t neg_expected[4] = {
        0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFDu
    };
    unsigned before = failures;
    unsigned mode;

    for (mode = 0; mode < 4; mode++) {
        prepare(code, sizeof(code) / sizeof(code[0]));
        put64(0x2100, double_bits(2.5));
        expect_status("fctiw_pos", run_fpscr(mode), QRX_PPC_TRAP);
        expect_u32("fctiw_pos", (uint32_t)ppc.cpu.fpr[2], pos_expected[mode]);
        expect_u32("fctiwz_pos", (uint32_t)ppc.cpu.fpr[3], 2);
        expect_u32("fctiw_pos flags", ppc.cpu.fpscr & 0xC2000000u, 0xC2000000u);

        prepare(code, sizeof(code) / sizeof(code[0]));
        put64(0x2100, double_bits(-2.5));
        expect_status("fctiw_neg", run_fpscr(mode), QRX_PPC_TRAP);
        expect_u32("fctiw_neg", (uint32_t)ppc.cpu.fpr[2], neg_expected[mode]);
        expect_u32("fctiwz_neg", (uint32_t)ppc.cpu.fpr[3], 0xFFFFFFFEu);
        expect_u32("fctiw_neg flags", ppc.cpu.fpscr & 0xC2000000u, 0xC2000000u);
    }
    report("fpu_fctiw", before);
}

static void check_special(const char *what, const uint32_t *code, size_t count,
                          uint64_t a, uint64_t b, uint64_t expected,
                          uint32_t expected_flags)
{
    const uint32_t mask = 0xFFF80000u; /* all FPSCR exception fields */

    prepare(code, count);
    put64(0x2100, a);
    put64(0x2108, b);
    expect_status(what, run(), QRX_PPC_TRAP);
    expect_u64(what, ppc.cpu.fpr[3], expected);
    expect_u32(what, ppc.cpu.fpscr & mask, expected_flags);
}

static void test_fpu_special(void)
{
    static const uint32_t add_code[] = {
        0xC8202100u, 0xC8402108u, 0xEC61102Au, 0xD8602110u, 0x44000002u
    };
    static const uint32_t sub_code[] = {
        0xC8202100u, 0xC8402108u, 0xEC611028u, 0xD8602110u, 0x44000002u
    };
    static const uint32_t mul_code[] = {
        0xC8202100u, 0xC8402108u, 0xEC611032u, 0xD8602110u, 0x44000002u
    };
    static const uint32_t div_code[] = {
        0xC8202100u, 0xC8402108u, 0xEC611024u, 0xD8602110u, 0x44000002u
    };
    const size_t n = sizeof(add_code) / sizeof(add_code[0]);
    unsigned before = failures;

    check_special("special_div00", div_code, n, 0x0000000000000000ull,
                  0x0000000000000000ull, 0x7FF8000000000000ull,
                  0xE0200000u); /* FX|FEX|VX|VXZDZ */
    check_special("special_infdiv", div_code, n, 0x7FF0000000000000ull,
                  0x7FF0000000000000ull, 0x7FF8000000000000ull,
                  0xE0400000u); /* FX|FEX|VX|VXIDI */
    check_special("special_infsub", sub_code, n, 0x7FF0000000000000ull,
                  0x7FF0000000000000ull, 0x7FF8000000000000ull,
                  0xE0800000u); /* FX|FEX|VX|VXISI */
    check_special("special_infmul", mul_code, n, 0x0000000000000000ull,
                  0x7FF0000000000000ull, 0x7FF8000000000000ull,
                  0xE0100000u); /* FX|FEX|VX|VXIMZ */
    check_special("special_snan", add_code, n, 0x7FF0000000000001ull,
                  0x3FF0000000000000ull, 0x7FF8000000000000ull,
                  0xE1000000u); /* FX|FEX|VX|VXSNAN */
    check_special("special_divzero", div_code, n, 0x3FF0000000000000ull,
                  0x0000000000000000ull, 0x7FF0000000000000ull,
                  0xC4000000u); /* FX|FEX|ZX */
    check_special("special_divzero_neg", div_code, n, 0xBFF0000000000000ull,
                  0x0000000000000000ull, 0xFFF0000000000000ull,
                  0xC4000000u);
    check_special("special_infadd", add_code, n, 0x7FF0000000000000ull,
                  0x3FF0000000000000ull, 0x7FF0000000000000ull, 0);
    check_special("special_qnan", add_code, n, 0x7FF8000000000000ull,
                  0x3FF0000000000000ull, 0x7FF8000000000000ull, 0);
    report("fpu_special", before);
}

static void test_fpu_inexact(void)
{
    static const uint32_t mul_code[] = {
        0xC8202100u, 0xC8402108u, 0xEC611032u, 0xD8602110u, 0x44000002u
    };
    static const uint32_t div_code[] = {
        0xC8202100u, 0xC8402108u, 0xEC611024u, 0xD8602110u, 0x44000002u
    };
    const size_t n = sizeof(mul_code) / sizeof(mul_code[0]);
    unsigned before = failures;

    prepare(mul_code, n);
    put64(0x2100, 0x7FEFFFFFFFFFFFFFull); /* DBL_MAX */
    put64(0x2108, double_bits(2.0));
    expect_status("overflow", run(), QRX_PPC_TRAP);
    expect_u64("overflow result", ppc.cpu.fpr[3], 0x7FF0000000000000ull);
    expect_u32("overflow flags", ppc.cpu.fpscr & 0xD2000000u, 0xD2000000u);

    prepare(div_code, n);
    put64(0x2100, 0x0010000000000000ull); /* min normal */
    put64(0x2108, double_bits(3.0));
    expect_status("underflow", run(), QRX_PPC_TRAP);
    expect_u32("underflow class", (uint32_t)(ppc.cpu.fpr[3] >> 52) & 0x7FFu, 0);
    expect_u32("underflow flags", ppc.cpu.fpscr & 0xCA000000u, 0xCA000000u);

    prepare(div_code, n);
    put64(0x2100, double_bits(1.0));
    put64(0x2108, double_bits(3.0));
    expect_status("inexact", run(), QRX_PPC_TRAP);
    expect_u64("inexact result", ppc.cpu.fpr[3], 0x3FD5555555555555ull);
    expect_u32("inexact flags", ppc.cpu.fpscr & 0xC2000000u, 0xC2000000u);
    report("fpu_inexact", before);
}

static void test_fpu_fpscr_transfer(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xFDFE0D8Eu, /* mtfsf 0xFF,f1 */
        0xFC40048Eu, /* mffs f2 */
        0xD8402110u, /* stfd f2,0x2110(0) */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    put64(0x2100, 0x0000000000000003ull);
    expect_status("fpscr_transfer", run(), QRX_PPC_TRAP);
    expect_u32("fpscr transfer", ppc.cpu.fpscr & 3u, 3);
    expect_u32("mffs transfer", (uint32_t)ppc.cpu.fpr[2], 3);
    expect_u64("mffs mem", get64(0x2110), 3);
    report("fpu_fpscr_transfer", before);
}

static void test_fpu_compare_nan(void)
{
    static const uint32_t code[] = {
        0xC8202100u, /* lfd f1,0x2100(0) */
        0xC8402108u, /* lfd f2,0x2108(0) */
        0xFC811000u, /* fcmpu cr1,f1,f2 */
        0xFD011040u, /* fcmpo cr2,f1,f2 */
        0x44000002u  /* sc */
    };
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    put64(0x2100, 0x7FF8000000000000ull); /* quiet NaN */
    put64(0x2108, double_bits(1.0));
    expect_status("compare_qnan", run(), QRX_PPC_TRAP);
    expect_u32("compare_qnan cr1", (ppc.cpu.cr >> 24) & 0xFu, 1);
    expect_u32("compare_qnan cr2", (ppc.cpu.cr >> 20) & 0xFu, 1);
    expect_u32("compare_qnan vxvc", ppc.cpu.fpscr & 0x00080000u, 0x00080000u);

    prepare(code, sizeof(code) / sizeof(code[0]));
    put64(0x2100, 0x7FF0000000000001ull); /* signalling NaN */
    put64(0x2108, double_bits(1.0));
    expect_status("compare_snan", run(), QRX_PPC_TRAP);
    expect_u32("compare_snan vxsnan", ppc.cpu.fpscr & 0x01000000u, 0x01000000u);
    report("fpu_compare_nan", before);
}

static void test_link(void)
{
    static const uint32_t code[] = {
        0x38C02000u, /* li r6,0x2000 */
        0x48000011u, /* bl +16 -> subroutine at 0x1014 */
        0x38A00063u, /* li r5,99 (return target) */
        0x90A60000u, /* stw r5,0(r6) */
        0x44000002u, /* sc */
        0x38A0002Au, /* li r5,42 (subroutine) */
        0x4E800020u  /* blr */
    };
    unsigned before = failures;

    expect_status("link", start(code, sizeof(code) / sizeof(code[0])),
                  QRX_PPC_TRAP);
    expect_u32("link r5", ppc.cpu.gpr[5], 99);
    expect_u32("link mem0", get32(DATA_BASE), 99);
    report("link", before);
}

#ifdef __AROS__
static void test_jit_cache(void)
{
    static const uint32_t code[] = {
        0x38600001u, /* li r3,1 */
        0x38C02000u, /* li r6,0x2000 */
        0x90660000u, /* stw r3,0(r6) */
        0x44000002u  /* sc */
    };
    qrx_ppc_t initial, interp, native;
    unsigned before = failures;

    prepare(code, sizeof(code) / sizeof(code[0]));
    qrx_ppc_jit_reset(&jit);
    initial = ppc;
    memcpy(memory_jit, memory, sizeof(memory));

    native = initial;
    native.mem.data = memory_jit;
    qrx_ppc_run_jit(&native, &jit, 10000);
    interp = initial;
    qrx_ppc_run(&interp, 10000);
    compare_jit_state(&interp, &native);
    expect_u32("jit_cache r3", native.cpu.gpr[3], 1);
    expect_u32("jit_cache mem", get32(DATA_BASE), 1);

    prepare(code, sizeof(code) / sizeof(code[0]));
    initial = ppc;
    memcpy(memory_jit, memory, sizeof(memory));
    native = initial;
    native.mem.data = memory_jit;
    qrx_ppc_run_jit(&native, &jit, 10000);
    interp = initial;
    qrx_ppc_run(&interp, 10000);
    compare_jit_state(&interp, &native);
    expect_u32("jit_cache hits", jit.hits, 1);

    prepare(code, sizeof(code) / sizeof(code[0]));
    put32(CODE_BASE, 0x38600002u);
    initial = ppc;
    memcpy(memory_jit, memory, sizeof(memory));
    native = initial;
    native.mem.data = memory_jit;
    qrx_ppc_run_jit(&native, &jit, 10000);
    interp = initial;
    qrx_ppc_run(&interp, 10000);
    compare_jit_state(&interp, &native);
    expect_u32("jit_smc r3", native.cpu.gpr[3], 2);
    expect_u32("jit_smc mem", get32(DATA_BASE), 2);
    expect_u32("jit_smc recompiles", jit.recompiles, 1);
    report("jit_cache_smc", before);
}
#endif

/* P2.1: PEF/CFM container reader.  A synthetic fragment is described by real
 * container bytes, loaded through qrx_pef_load() and entered through its main
 * transition vector. */
#define PEF_IMAGE_SIZE 208u
#define PEF_CODE_OFF 96u
#define PEF_LOADER_OFF 128u
#define PEF_LOAD_BASE 0x8000u

static void pef_put16(uint8_t *buf, uint32_t off, uint16_t value)
{
    buf[off] = (uint8_t)(value >> 8);
    buf[off + 1u] = (uint8_t)value;
}

static void pef_put32(uint8_t *buf, uint32_t off, uint32_t value)
{
    pef_put16(buf, off, (uint16_t)(value >> 16));
    pef_put16(buf, off + 2u, (uint16_t)value);
}

/* One code section and one loader section.  The code starts with a transition
 * vector { entry, TOC }; its words already hold the runtime addresses for
 * PEF_LOAD_BASE because relocation is not applied yet. */
static void pef_build_fragment(uint8_t *buf)
{
    memset(buf, 0, PEF_IMAGE_SIZE);
    pef_put32(buf, 0, 0x4A6F7921u);        /* 'Joy!' */
    pef_put32(buf, 4, 0x70656666u);        /* 'peff' */
    pef_put32(buf, 8, 0x70777063u);        /* 'pwpc' */
    pef_put32(buf, 12, 1u);                /* formatVersion */
    pef_put16(buf, 32, 2u);                /* sectionCount */
    pef_put16(buf, 34, 1u);                /* instSectionCount */

    /* Section 0: code at container offset 96. */
    pef_put32(buf, 40, 0xFFFFFFFFu);       /* nameOffset -1 */
    pef_put32(buf, 44, 0x8000u);           /* defaultAddress */
    pef_put32(buf, 48, 32u);               /* totalSize */
    pef_put32(buf, 52, 32u);               /* unpackedSize */
    pef_put32(buf, 56, 0u);                /* packedSize */
    pef_put32(buf, 60, PEF_CODE_OFF);      /* containerOffset */
    buf[64] = 0;                           /* sectionKind: code */
    buf[66] = 2;                           /* alignment */

    /* Section 1: loader at container offset 128. */
    pef_put32(buf, 68, 0xFFFFFFFFu);
    pef_put32(buf, 72, 0x9000u);
    pef_put32(buf, 76, 80u);
    pef_put32(buf, 80, 80u);
    pef_put32(buf, 84, 0u);
    pef_put32(buf, 88, PEF_LOADER_OFF);
    buf[92] = 4;                           /* sectionKind: loader */
    buf[94] = 2;

    /* Code: TVector at offset 0, fragment at offset 8 (entry 0x8008). */
    pef_put32(buf, PEF_CODE_OFF + 0u, 0x8008u);
    pef_put32(buf, PEF_CODE_OFF + 4u, 0u);
    pef_put32(buf, PEF_CODE_OFF + 8u, 0x38600007u);  /* li r3,7 */
    pef_put32(buf, PEF_CODE_OFF + 12u, 0x38630023u); /* addi r3,r3,35 */
    pef_put32(buf, PEF_CODE_OFF + 16u, 0x39002000u); /* li r8,0x2000 */
    pef_put32(buf, PEF_CODE_OFF + 20u, 0x90680000u); /* stw r3,0(r8) */
    pef_put32(buf, PEF_CODE_OFF + 24u, 0x44000002u); /* sc */

    /* Loader header: main in section 0 at offset 0, no init/term/imports. */
    pef_put32(buf, PEF_LOADER_OFF + 0u, 0u);           /* mainSection */
    pef_put32(buf, PEF_LOADER_OFF + 8u, 0xFFFFFFFFu);  /* initSection */
    pef_put32(buf, PEF_LOADER_OFF + 16u, 0xFFFFFFFFu); /* termSection */
    pef_put32(buf, PEF_LOADER_OFF + 40u, 74u);         /* loaderStringsOffset */
    pef_put32(buf, PEF_LOADER_OFF + 44u, 56u);         /* exportHashOffset */
    pef_put32(buf, PEF_LOADER_OFF + 52u, 1u);          /* exportedSymbolCount */

    /* Export tables: one hash slot, one key, one entry, strings at 74. */
    pef_put32(buf, PEF_LOADER_OFF + 56u, 1u << 18);    /* count 1, first 0 */
    pef_put32(buf, PEF_LOADER_OFF + 64u, 0u);          /* class 0, name off 0 */
    pef_put32(buf, PEF_LOADER_OFF + 68u, 8u);          /* symbolValue */
    pef_put16(buf, PEF_LOADER_OFF + 72u, 0u);          /* sectionIndex */
    memcpy(buf + PEF_LOADER_OFF + 74u, "_main", 6u);
}

static void test_pef_loader(void)
{
    static qrx_pef_t image;
    static qrx_ppc_t tiny;
    static uint8_t tiny_mem[0x1000];
    uint8_t built[PEF_IMAGE_SIZE];
    uint8_t damaged[PEF_IMAGE_SIZE];
#ifdef __AROS__
    qrx_ppc_t start;
#endif
    qrx_pef_status_t st;
    uint32_t tvector;
    unsigned before = failures;

    pef_build_fragment(built);

    /* Every malformed container ends in its own status instead of a crash. */
    expect_u32("pef_short", (uint32_t)qrx_pef_parse(&image, built, 39u),
               (uint32_t)QRX_PEF_ERR_SHORT);
    memcpy(damaged, built, sizeof(damaged));
    damaged[0] = 'X';
    expect_u32("pef_magic", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_MAGIC);
    memcpy(damaged, built, sizeof(damaged));
    pef_put32(damaged, 8u, 0x6D36386Bu);   /* 'm68k' */
    expect_u32("pef_arch", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_ARCH);
    memcpy(damaged, built, sizeof(damaged));
    pef_put32(damaged, 12u, 2u);
    expect_u32("pef_version", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_VERSION);
    memcpy(damaged, built, sizeof(damaged));
    pef_put16(damaged, 32u, 0u);
    expect_u32("pef_sections_zero", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_SECTIONS);
    memcpy(damaged, built, sizeof(damaged));
    pef_put16(damaged, 32u, 17u);
    expect_u32("pef_sections_max", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_SECTIONS);
    memcpy(damaged, built, sizeof(damaged));
    pef_put32(damaged, 56u, 4u);
    expect_u32("pef_packed", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_PACKED);
    memcpy(damaged, built, sizeof(damaged));
    pef_put32(damaged, 60u, 300u);
    expect_u32("pef_offset", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_OFFSET);
    memcpy(damaged, built, sizeof(damaged));
    pef_put32(damaged, PEF_LOADER_OFF + 52u, 20u);
    expect_u32("pef_exports", (uint32_t)qrx_pef_parse(&image, damaged, PEF_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_EXPORTS);

    st = qrx_pef_parse(&image, built, PEF_IMAGE_SIZE);
    expect_u32("pef_parse", (uint32_t)st, (uint32_t)QRX_PEF_OK);
    expect_u32("pef_sections_parsed", image.section_count, 2u);
    expect_u32("pef_loader_index", (uint32_t)image.loader_index, 1u);
    expect_u32("pef_main_section", (uint32_t)image.main_section, 0u);
    expect_u32("pef_export_count", image.export_count, 1u);
    expect_u32("pef_import_count", image.import_count, 0u);
    expect_u32("pef_library_count", image.library_count, 0u);
    expect_u32("pef_export_value", image.exports[0].value, 8u);
    expect_u32("pef_export_section", (uint32_t)image.exports[0].section_index, 0u);
    expect_u32("pef_export_class", image.exports[0].symbol_class, 0u);
    if (qrx_pef_string(&image, image.exports[0].name_offset) == 0 ||
        strcmp(qrx_pef_string(&image, image.exports[0].name_offset), "_main") != 0) {
        printf("FAIL pef_export_name\n");
        failures++;
    }
    expect_u32("pef_find_code",
               (uint32_t)(qrx_pef_find_export(&image, 0, "_main") + 1), 1u);
    expect_u32("pef_find_any",
               (uint32_t)(qrx_pef_find_export(&image, -1, "_main") + 1), 1u);
    expect_u32("pef_find_wrong_class",
               (uint32_t)(qrx_pef_find_export(&image, 1, "_main") + 1), 0u);
    expect_u32("pef_find_missing",
               (uint32_t)(qrx_pef_find_export(&image, 0, "_nope") + 1), 0u);
    expect_u32("pef_string_oob", qrx_pef_string(&image, 80u) == 0 ? 1u : 0u, 1u);

    memset(memory, 0, sizeof(memory));
    qrx_ppc_init(&ppc, memory, (uint32_t)sizeof(memory));
    qrx_ppc_init(&tiny, tiny_mem, (uint32_t)sizeof(tiny_mem));
    expect_u32("pef_load_small",
               (uint32_t)qrx_pef_load(&image, &tiny, PEF_LOAD_BASE),
               (uint32_t)QRX_PEF_ERR_MEMORY);
    st = qrx_pef_load(&image, &ppc, PEF_LOAD_BASE);
    expect_u32("pef_load", (uint32_t)st, (uint32_t)QRX_PEF_OK);
    expect_u32("pef_address0", qrx_pef_section_address(&image, 0), PEF_LOAD_BASE);
    expect_u32("pef_address1", qrx_pef_section_address(&image, 1),
               PEF_LOAD_BASE + 0x1000u);
    expect_u32("pef_loader_copy", get32(PEF_LOAD_BASE + 0x1000u), 0u);
    tvector = qrx_pef_main_tvector(&image);
    expect_u32("pef_tvector", tvector, PEF_LOAD_BASE);
    expect_u32("pef_entry", get32(tvector), PEF_LOAD_BASE + 8u);

    ppc.cpu.pc = get32(tvector);
    ppc.cpu.gpr[1] = 0x3000;
    ppc.cpu.gpr[2] = 0x3000;
#ifdef __AROS__
    memcpy(memory_jit, memory, sizeof(memory));
    start = ppc;
#endif
    expect_status("pef_run", qrx_ppc_run(&ppc, 1000), QRX_PPC_TRAP);
    expect_u32("pef_run_r3", ppc.cpu.gpr[3], 42u);
    expect_u32("pef_run_mem", get32(DATA_BASE), 42u);

#ifdef __AROS__
    if (jit_code) {
        qrx_ppc_t native = start;

        native.mem.data = memory_jit;
        qrx_ppc_jit_reset(&jit);
        expect_status("pef_jit", qrx_ppc_run_jit(&native, &jit, 1000),
                      QRX_PPC_TRAP);
        expect_u32("pef_jit_r3", native.cpu.gpr[3], 42u);
        expect_u32("pef_jit_mem", get32_native(DATA_BASE), 42u);
        compare_jit_state(&ppc, &native);
    }
#endif
    report("pef_loader", before);
}


/* P2.1: PEF relocations.  The container has code, data and loader sections;
 * the relocation stream fixes up a TVector12, an imported glue pointer and
 * two section-relative words, so the fragment is entered through real
 * addresses. */
#define PEF3_IMAGE_SIZE 342u
#define PEF3_CODE_OFF 128u
#define PEF3_DATA_OFF 176u
#define PEF3_LOADER_OFF 192u
#define PEF3_BASE 0x8000u
#define PEF3_IMPORT_ADDR 0x9300u

static uint32_t pef_test_resolver(void *ctx, const char *library,
                                  uint8_t symbol_class, const char *name)
{
    (void)ctx;
    if (library && strcmp(library, "TestLib") == 0 &&
        symbol_class == 4u && strcmp(name, "_glue") == 0)
        return PEF3_IMPORT_ADDR;
    return 0;
}

static void pef_build_reloc_fragment(uint8_t *buf)
{
    memset(buf, 0, PEF3_IMAGE_SIZE);
    pef_put32(buf, 0, 0x4A6F7921u);        /* 'Joy!' */
    pef_put32(buf, 4, 0x70656666u);        /* 'peff' */
    pef_put32(buf, 8, 0x70777063u);        /* 'pwpc' */
    pef_put32(buf, 12, 1u);                /* formatVersion */
    pef_put16(buf, 32, 3u);                /* sectionCount */
    pef_put16(buf, 34, 1u);                /* instSectionCount */

    /* Section 0: code, 48 bytes at 128. */
    pef_put32(buf, 40, 0xFFFFFFFFu);
    pef_put32(buf, 44, 0u);
    pef_put32(buf, 48, 48u);
    pef_put32(buf, 52, 48u);
    pef_put32(buf, 56, 0u);
    pef_put32(buf, 60, PEF3_CODE_OFF);
    buf[64] = 0;                           /* code */
    buf[66] = 2;

    /* Section 1: data, 16 bytes at 176. */
    pef_put32(buf, 68, 0xFFFFFFFFu);
    pef_put32(buf, 72, 0u);
    pef_put32(buf, 76, 16u);
    pef_put32(buf, 80, 16u);
    pef_put32(buf, 84, 0u);
    pef_put32(buf, 88, PEF3_DATA_OFF);
    buf[92] = 1;                           /* unpacked data */
    buf[94] = 2;

    /* Section 2: loader, 150 bytes at 192. */
    pef_put32(buf, 96, 0xFFFFFFFFu);
    pef_put32(buf, 100, 0u);
    pef_put32(buf, 104, 150u);
    pef_put32(buf, 108, 150u);
    pef_put32(buf, 112, 0u);
    pef_put32(buf, 116, PEF3_LOADER_OFF);
    buf[120] = 4;                          /* loader */
    buf[122] = 2;

    /* TVector12: entry offset 16, TOC offset 0, glue word left alone. */
    pef_put32(buf, PEF3_CODE_OFF + 0u, 16u);
    pef_put32(buf, PEF3_CODE_OFF + 4u, 0u);
    pef_put32(buf, PEF3_CODE_OFF + 8u, 0u);
    /* Imported glue pointer placeholder. */
    pef_put32(buf, PEF3_CODE_OFF + 12u, 0u);
    /* Fragment at offset 16: li r3,42; ori r7,r0,0x8000; lwz r9,12(r7);
     * li r8,0x2000; stw r9,0(r8); sc.  ori (not lis) builds the real
     * load address 0x8000, so the read hits base+12, the resolved
     * import pointer in the TVector12. */
    pef_put32(buf, PEF3_CODE_OFF + 16u, 0x3860002Au);
    pef_put32(buf, PEF3_CODE_OFF + 20u, 0x60078000u);
    pef_put32(buf, PEF3_CODE_OFF + 24u, 0x8127000Cu);
    pef_put32(buf, PEF3_CODE_OFF + 28u, 0x39002000u);
    pef_put32(buf, PEF3_CODE_OFF + 32u, 0x91280000u);
    pef_put32(buf, PEF3_CODE_OFF + 36u, 0x44000002u);

    /* Loader header: one import library, two relocation sections. */
    pef_put32(buf, PEF3_LOADER_OFF + 0u, 0u);           /* mainSection */
    pef_put32(buf, PEF3_LOADER_OFF + 4u, 0u);           /* mainOffset */
    pef_put32(buf, PEF3_LOADER_OFF + 8u, 0xFFFFFFFFu);  /* initSection */
    pef_put32(buf, PEF3_LOADER_OFF + 16u, 0xFFFFFFFFu); /* termSection */
    pef_put32(buf, PEF3_LOADER_OFF + 24u, 1u);          /* importedLibraryCount */
    pef_put32(buf, PEF3_LOADER_OFF + 28u, 1u);          /* totalImportedSymbols */
    pef_put32(buf, PEF3_LOADER_OFF + 32u, 2u);          /* relocSectionCount */
    pef_put32(buf, PEF3_LOADER_OFF + 36u, 108u);        /* relocInstrOffset */
    pef_put32(buf, PEF3_LOADER_OFF + 40u, 132u);        /* loaderStringsOffset */
    pef_put32(buf, PEF3_LOADER_OFF + 44u, 146u);        /* exportHashOffset */
    pef_put32(buf, PEF3_LOADER_OFF + 48u, 0u);          /* exportHashTablePower */
    pef_put32(buf, PEF3_LOADER_OFF + 52u, 0u);          /* exportedSymbolCount */

    /* Imported library "TestLib" (strings at 132) and symbol "_glue". */
    pef_put32(buf, PEF3_LOADER_OFF + 56u, 0u);          /* library nameOffset */
    pef_put32(buf, PEF3_LOADER_OFF + 68u, 1u);          /* importedSymbolCount */
    pef_put32(buf, PEF3_LOADER_OFF + 72u, 0u);          /* firstImportedSymbol */
    pef_put32(buf, PEF3_LOADER_OFF + 80u, (4u << 24) | 8u); /* class 4, "_glue" */

    /* Relocation headers: section 0 at 0, section 1 at 12. */
    pef_put16(buf, PEF3_LOADER_OFF + 84u, 0u);
    pef_put32(buf, PEF3_LOADER_OFF + 88u, 6u);
    pef_put32(buf, PEF3_LOADER_OFF + 92u, 0u);
    pef_put16(buf, PEF3_LOADER_OFF + 96u, 1u);
    pef_put32(buf, PEF3_LOADER_OFF + 100u, 6u);
    pef_put32(buf, PEF3_LOADER_OFF + 104u, 12u);

    /* Section 0: SetPosition 0, TVector12 x1, SetPosition 12, SmByImport 0. */
    pef_put16(buf, PEF3_LOADER_OFF + 108u, 0xA000u);
    pef_put16(buf, PEF3_LOADER_OFF + 110u, 0u);
    pef_put16(buf, PEF3_LOADER_OFF + 112u, 0x4400u);
    pef_put16(buf, PEF3_LOADER_OFF + 114u, 0xA000u);
    pef_put16(buf, PEF3_LOADER_OFF + 116u, 12u);
    pef_put16(buf, PEF3_LOADER_OFF + 118u, 0x6000u);
    /* Section 1: SetPosition 0, BySectD x1, SetPosition 4, SmBySection 0. */
    pef_put16(buf, PEF3_LOADER_OFF + 120u, 0xA000u);
    pef_put16(buf, PEF3_LOADER_OFF + 122u, 0u);
    pef_put16(buf, PEF3_LOADER_OFF + 124u, 0x4200u);
    pef_put16(buf, PEF3_LOADER_OFF + 126u, 0xA000u);
    pef_put16(buf, PEF3_LOADER_OFF + 128u, 4u);
    pef_put16(buf, PEF3_LOADER_OFF + 130u, 0x6600u);

    /* String table and the mandatory (empty) export hash slot. */
    memcpy(buf + PEF3_LOADER_OFF + 132u, "TestLib", 8u);
    memcpy(buf + PEF3_LOADER_OFF + 140u, "_glue", 6u);
    pef_put32(buf, PEF3_LOADER_OFF + 146u, 0u);
}

static void test_pef_reloc(void)
{
    static qrx_pef_t image;
    uint8_t built[PEF3_IMAGE_SIZE];
    uint8_t damaged[PEF3_IMAGE_SIZE];
#ifdef __AROS__
    qrx_ppc_t start;
#endif
    qrx_pef_status_t st;
    unsigned before = failures;

    pef_build_reloc_fragment(built);

    memset(memory, 0, sizeof(memory));
    qrx_ppc_init(&ppc, memory, (uint32_t)sizeof(memory));
    expect_u32("pef_reloc_parse",
               (uint32_t)qrx_pef_parse(&image, built, PEF3_IMAGE_SIZE),
               (uint32_t)QRX_PEF_OK);
    expect_u32("pef_reloc_sections", image.section_count, 3u);
    expect_u32("pef_reloc_loader_index", (uint32_t)image.loader_index, 2u);
    expect_u32("pef_reloc_reloc_sections", image.reloc_section_count, 2u);
    expect_u32("pef_reloc_header0", image.reloc_headers[0].reloc_count, 6u);
    expect_u32("pef_reloc_header1",
               image.reloc_headers[1].first_reloc_offset, 12u);
    expect_u32("pef_reloc_load",
               (uint32_t)qrx_pef_load(&image, &ppc, PEF3_BASE),
               (uint32_t)QRX_PEF_OK);
    /* Unresolved imports fail before the first write. */
    expect_u32("pef_reloc_unresolved",
               (uint32_t)qrx_pef_relocate(&image, &ppc, 0, 0),
               (uint32_t)QRX_PEF_ERR_UNRESOLVED);
    expect_u32("pef_reloc_no_partial", get32(PEF3_BASE), 16u);

    st = qrx_pef_relocate(&image, &ppc, pef_test_resolver, 0);
    expect_u32("pef_reloc_status", (uint32_t)st, (uint32_t)QRX_PEF_OK);
    expect_u32("pef_reloc_tvec_entry", get32(PEF3_BASE), 0x8010u);
    expect_u32("pef_reloc_tvec_toc", get32(PEF3_BASE + 4u), 0x9000u);
    expect_u32("pef_reloc_tvec_glue_word", get32(PEF3_BASE + 8u), 0u);
    expect_u32("pef_reloc_import", get32(PEF3_BASE + 12u), PEF3_IMPORT_ADDR);
    expect_u32("pef_reloc_sectd", get32(PEF3_BASE + 0x1000u), 0x9000u);
    expect_u32("pef_reloc_section0", get32(PEF3_BASE + 0x1004u), 0x8000u);

    ppc.cpu.pc = get32(PEF3_BASE);
    ppc.cpu.gpr[0] = 0;
    ppc.cpu.gpr[1] = 0x3000;
    ppc.cpu.gpr[2] = 0x3000;
    expect_u32("pef_reloc_entry", ppc.cpu.pc, 0x8010u);
#ifdef __AROS__
    start = ppc;
    memcpy(memory_jit, memory, sizeof(memory));
#endif
    expect_status("pef_reloc_run", qrx_ppc_run(&ppc, 1000), QRX_PPC_TRAP);
    expect_u32("pef_reloc_run_r3", ppc.cpu.gpr[3], 42u);
    expect_u32("pef_reloc_run_r9", ppc.cpu.gpr[9], PEF3_IMPORT_ADDR);
    expect_u32("pef_reloc_run_mem", get32(DATA_BASE), PEF3_IMPORT_ADDR);

#ifdef __AROS__
    if (jit_code) {
        qrx_ppc_t native = start;

        native.mem.data = memory_jit;
        qrx_ppc_jit_reset(&jit);
        expect_status("pef_reloc_jit", qrx_ppc_run_jit(&native, &jit, 1000),
                      QRX_PPC_TRAP);
        expect_u32("pef_reloc_jit_r9", native.cpu.gpr[9], PEF3_IMPORT_ADDR);
        expect_u32("pef_reloc_jit_mem", get32_native(DATA_BASE),
                   PEF3_IMPORT_ADDR);
        compare_jit_state(&ppc, &native);
    }
#endif

    /* Invalid opcode in the relocation stream. */
    memcpy(damaged, built, sizeof(damaged));
    pef_put16(damaged, PEF3_LOADER_OFF + 108u, 0x7C00u);
    memset(memory, 0, sizeof(memory));
    qrx_ppc_init(&ppc, memory, (uint32_t)sizeof(memory));
    expect_u32("pef_reloc_bad_parse",
               (uint32_t)qrx_pef_parse(&image, damaged, PEF3_IMAGE_SIZE),
               (uint32_t)QRX_PEF_OK);
    expect_u32("pef_reloc_bad_load",
               (uint32_t)qrx_pef_load(&image, &ppc, PEF3_BASE),
               (uint32_t)QRX_PEF_OK);
    expect_u32("pef_reloc_bad_opcode",
               (uint32_t)qrx_pef_relocate(&image, &ppc, pef_test_resolver, 0),
               (uint32_t)QRX_PEF_ERR_RELOC);

    /* A relocation header must name an existing section. */
    memcpy(damaged, built, sizeof(damaged));
    pef_put16(damaged, PEF3_LOADER_OFF + 84u, 7u);
    expect_u32("pef_reloc_bad_section",
               (uint32_t)qrx_pef_parse(&image, damaged, PEF3_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_RELOC);

    /* The relocation area must stay inside the loader section. */
    memcpy(damaged, built, sizeof(damaged));
    pef_put32(damaged, PEF3_LOADER_OFF + 92u, 140u);
    expect_u32("pef_reloc_bad_stream",
               (uint32_t)qrx_pef_parse(&image, damaged, PEF3_IMAGE_SIZE),
               (uint32_t)QRX_PEF_ERR_RELOC);

    report("pef_reloc", before);
}

/* --- P1.5 mixed mode ------------------------------------------------ */

#define MIX_CODE 0x1200u
#define MIX_GLUE 0x1800u
#define MIX_SP_TOP 0x3000u
#define MIX_SP_LIMIT 0x2800u
#define MIX_SCRATCH 0x2200u
#define MIX_GLUE_SP ((MIX_SP_TOP - 24u) & ~0xFu)

#define MIX_F_ADD 0x1200u
#define MIX_F_STACK 0x1240u
#define MIX_F_FP 0x1260u
#define MIX_F_IMPORT 0x1280u
#define MIX_F_NESTED 0x12C0u
#define MIX_F_BADSP 0x1300u
#define MIX_F_UNKNOWN 0x1310u
#define MIX_F_ILLEGAL 0x1320u
#define MIX_F_SPIN 0x1330u
#define MIX_F_SIG 0x1340u
#define MIX_F_LFD 0x1360u
#define MIX_F_SIG_IMPORT 0x1380u
#define MIX_F_SIG_STACK 0x1400u
#define MIX_F_SIG_BADSP 0x1480u
#define MIX_F_PROC 0x14C0u
#define MIX_F_PROC_BAD 0x1500u
#define MIX_F_RDNEST 0x1540u
#define MIX_F_RDCALL 0x1580u
#define MIX_F_RDERR 0x15C0u
#define MIX_F_PI 0x1600u        /* r3+r4+r5: the ProcInfo call target */
#define MIX_F_PI0 0x1620u       /* no parameters, constant result */
#define MIX_RD_CALL 0x20C0u     /* fat: 68k record first, PPC record second */
#define MIX_RD_68K 0x2140u      /* 68k record only */
#define MIX_RD_SPIN 0x2160u     /* PPC record, endless loop */
#define MIX_RD_BADSP 0x2180u    /* PPC record that keeps its own stack */
#define MIX_RD_SIG 0x21A0u      /* PPC record with nine integer parameters */
#define MIX_RD_PI 0x21C0u       /* ProcInfo: three 4-byte parameters */
#define MIX_RD_PI0 0x21E0u      /* ProcInfo: no parameters, void */
#define MIX_RD_PINARROW 0x2240u /* ProcInfo: 1-byte and 2-byte parameters */
#define MIX_RD_PIBAD 0x2260u    /* rewritten for rejected ProcInfos */
#define MIX_F_PROC_PI 0x1640u   /* descriptor import, three arguments */
#define MIX_F_PROC_PI68 0x1680u /* the same for a 68k-only descriptor */
#define MIX_F_IDX_PI 0x16C0u    /* index import with a bound descriptor */
#define MIX_PROC_PI 0x22C0u     /* fat: opaque 68k ProcInfo, PPC ProcInfo */
#define MIX_PROC_PI68 0x2320u   /* 68k record only */
#define MIX_GLUE_RD 0x2360u     /* glue case: fat, PPC ProcInfo names two */
#define MIX_GLUE_RDBAD 0x23A0u  /* glue case: ProcInfo describes nothing */
#define MIX_IDX_RD 0x23E0u      /* bound to index 5: fat, PPC ProcInfo */

typedef struct {
    qrx_mix_t *mix;
    unsigned calls;
    unsigned nested_ok;
    uint32_t seen[2];
    unsigned gcount;
    unsigned fcount;
    unsigned stack_count;
    uint32_t stack0;
    uint32_t stack1;
    uint32_t ptr;
    uint32_t sp;
    double f0;
    uint32_t sum;
    uint32_t proc;
    /* 68k -> PPC re-entry through a routine descriptor */
    unsigned rd_ok;
    unsigned rd_ok2;
    unsigned rd_rd;
    unsigned rd_bad;
    unsigned rd_limit;
    unsigned rd_sp;
    unsigned rd_restore;
    uint32_t rd_sp_seen;
    uint32_t rd_back;
    uint32_t rd_stack;
    uint32_t rd_marker[2];
} mix_ctx_t;

/* 68k callback: D0 becomes r3, and it writes through guest memory. */
static uint32_t mix_import_add(void *ctx, const qrx_mix_args_t *args,
                               qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;

    (void)extra;
    c->calls++;
    c->seen[0] = args->gpr[0];
    c->seen[1] = args->gpr[1];
    put32(MIX_SCRATCH, args->gpr[0] + args->gpr[1]);
    return 42u;
}

/* 68k callback that re-enters PPC code (68k -> PPC -> 68k -> PPC). */
static uint32_t mix_import_nested(void *ctx, const qrx_mix_args_t *args,
                                  qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;
    qrx_mix_args_t in;
    qrx_mix_result_t out;

    (void)args;
    (void)extra;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.gpr[0] = 20u;
    in.gpr[1] = 22u;
    in.gpr_count = 2u;
    c->nested_ok =
        qrx_mix_call(c->mix, MIX_F_ADD, QRX_MIX_RAW_SIG, &in, &out, 4096u) == QRX_MIX_OK &&
        out.gpr[0] == 42u;
    return out.gpr[0] + 5u;
}

/* 68k callback with a fixed signature "iipd": two integers, a pointer and a
 * double.  It checks that the bridge placed every class in the right register
 * and writes through the pointer argument. */
static uint32_t mix_import_sig(void *ctx, const qrx_mix_args_t *args,
                               qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;

    (void)extra;
    c->calls++;
    c->gcount = args->gpr_count;
    c->fcount = args->fpr_count;
    c->seen[0] = args->gpr[0];
    c->seen[1] = args->gpr[1];
    c->ptr = args->gpr[2];
    c->f0 = args->fpr[0];
    put32(args->gpr[2], 0x55u);
    return 7u;
}

/* 68k callback with ten integer parameters: the last two arrive in the
 * parameter area and must show up as raw stack words. */
static uint32_t mix_import_stack(void *ctx, const qrx_mix_args_t *args,
                                 qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;
    unsigned i;

    (void)extra;
    c->calls++;
    c->gcount = args->gpr_count;
    c->stack_count = args->stack_count;
    c->stack0 = args->stack[0];
    c->stack1 = args->stack[1];
    c->sp = args->sp;
    c->sum = 0u;
    for (i = 0; i < args->gpr_count; i++)
        c->sum += args->gpr[i];
    for (i = 0; i < args->stack_count; i++)
        c->sum += args->stack[i];
    return c->sum;
}

/* 68k callback reached through a routine descriptor: the bridge resolves the
 * 68k record and reports its entry point in args->proc. */
static uint32_t mix_import_proc(void *ctx, const qrx_mix_args_t *args,
                                qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;
    unsigned i;

    (void)extra;
    c->calls++;
    c->proc = args->proc;
    c->gcount = args->gpr_count;
    c->stack_count = args->stack_count;
    c->stack0 = args->stack[0];
    c->sp = args->sp;
    c->sum = 0u;
    for (i = 0; i < args->gpr_count; i++)
        c->sum += args->gpr[i];
    for (i = 0; i < args->stack_count; i++)
        c->sum += args->stack[i];
    return c->sum;
}

/* 68k callback reached from PPC that calls back into PPC through a routine
 * descriptor.  It checks the returned value, the guest writes of the nested
 * routine, the back chain and parameter word of the nested frame, and that the
 * nested call left the outer CPU state untouched. */
static uint32_t mix_import_rd_call(void *ctx, const qrx_mix_args_t *args,
                                   qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;
    qrx_mix_sig_t sig2;
    qrx_mix_sig_t sig9;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_ppc_state_t saved;
    uint32_t r1;
    unsigned i;

    (void)extra;
    c->calls++;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    if (qrx_mix_sig_parse(&sig2, "ii", 'i') != QRX_MIX_OK ||
        qrx_mix_sig_parse(&sig9, "iiiiiiiii", 'i') != QRX_MIX_OK)
        return 0u;
    r1 = c->mix->ppc->cpu.gpr[1];
    c->rd_sp_seen = (r1 - QRX_MIX_LINKAGE) & ~0xFu;
    saved = c->mix->ppc->cpu;

    /* The fat descriptor lists its 68k record first; the PPC record is used. */
    in.gpr[0] = args->gpr[0];
    in.gpr[1] = args->gpr[1];
    in.gpr_count = 2u;
    c->rd_ok = qrx_mix_rd_invoke(c->mix, MIX_RD_CALL, &sig2, &in, &out, 200u) ==
                   QRX_MIX_OK &&
               out.gpr[0] == in.gpr[0] + in.gpr[1];
    c->rd_restore = memcmp(&saved, &c->mix->ppc->cpu, sizeof(saved)) == 0;
    c->rd_back = get32(c->rd_sp_seen);
    c->rd_marker[0] = get32(MIX_SCRATCH + 0x10u);
    c->rd_marker[1] = get32(MIX_SCRATCH + 0x14u);

    /* Nine parameters put the last one into the nested parameter area. */
    for (i = 0; i < 8u; i++)
        in.gpr[i] = i + 1u;
    in.gpr_count = 8u;
    in.stack[0] = 9u;
    in.stack_count = 1u;
    c->rd_ok2 = qrx_mix_rd_invoke(c->mix, MIX_RD_SIG, &sig9, &in, &out, 200u) ==
                    QRX_MIX_OK && out.gpr[0] == 12u;
    c->rd_stack = get32(c->rd_sp_seen + QRX_MIX_LINKAGE);
    return out.gpr[0] + 13u;
}

/* The 68k side of a real glue case: it holds nothing but a UniversalProcPtr
 * (the descriptor address) and re-enters PPC without a signature, so the
 * record's ProcInfo has to supply the placement.  The second descriptor has a
 * ProcInfo that describes none; that attempt must fail and leave the CPU state
 * exactly as the outer call left it. */
static uint32_t mix_import_glue_pi(void *ctx, const qrx_mix_args_t *args,
                                   qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_ppc_state_t saved;
    uint32_t r1;

    (void)extra;
    c->calls++;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    r1 = c->mix->ppc->cpu.gpr[1];
    c->rd_sp_seen = (r1 - QRX_MIX_LINKAGE) & ~0xFu;
    saved = c->mix->ppc->cpu;

    in.gpr[0] = args->gpr[0];
    in.gpr[1] = args->gpr[1];
    in.gpr_count = 2u;
    c->rd_ok = qrx_mix_rd_invoke(c->mix, MIX_GLUE_RD, 0, &in, &out, 200u) ==
                   QRX_MIX_OK &&
               out.gpr[0] == in.gpr[0] + in.gpr[1];
    c->rd_marker[0] = get32(0x2210u);
    c->rd_marker[1] = get32(0x2214u);
    c->rd_rd = qrx_mix_rd_invoke(c->mix, MIX_GLUE_RDBAD, 0, &in, &out, 200u) ==
                   QRX_MIX_ERR_RD;
    c->rd_restore = memcmp(&saved, &c->mix->ppc->cpu, sizeof(saved)) == 0;
    c->rd_back = get32(c->rd_sp_seen);
    return in.gpr[0] + in.gpr[1] + 1u;
}

/* The failing nested attempts.  Each must report its error and leave the outer
 * CPU state exactly as it was, otherwise the callback could not continue. */
static uint32_t mix_import_rd_errors(void *ctx, const qrx_mix_args_t *args,
                                     qrx_mix_result_t *extra)
{
    mix_ctx_t *c = (mix_ctx_t *)ctx;
    qrx_mix_sig_t sig;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_ppc_state_t saved;

    (void)args;
    (void)extra;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.gpr_count = 2u;
    c->calls++;
    saved = c->mix->ppc->cpu;
    if (qrx_mix_sig_parse(&sig, "ii", 'i') != QRX_MIX_OK)
        return 0u;

    c->rd_rd = qrx_mix_rd_invoke(c->mix, MIX_RD_68K, &sig, &in, &out, 200u) ==
                   QRX_MIX_ERR_RD &&
               memcmp(&saved, &c->mix->ppc->cpu, sizeof(saved)) == 0;
    c->rd_bad = qrx_mix_rd_invoke(c->mix, MIX_SCRATCH, &sig, &in, &out, 200u) ==
                    QRX_MIX_ERR_RD &&
                memcmp(&saved, &c->mix->ppc->cpu, sizeof(saved)) == 0;
    c->rd_limit = qrx_mix_rd_invoke(c->mix, MIX_RD_SPIN, &sig, &in, &out, 4u) ==
                      QRX_MIX_ERR_LIMIT &&
                  memcmp(&saved, &c->mix->ppc->cpu, sizeof(saved)) == 0;
    c->rd_sp = qrx_mix_rd_invoke(c->mix, MIX_RD_BADSP, &sig, &in, &out, 100u) ==
                   QRX_MIX_ERR_SP &&
               memcmp(&saved, &c->mix->ppc->cpu, sizeof(saved)) == 0;
    return 9u;
}

static void mix_load(void)
{
    memset(memory, 0, sizeof(memory));
    qrx_ppc_init(&ppc, memory, (uint32_t)sizeof(memory));

    /* 0x1200: r3 = r3 + r4, with a PowerOpen frame and saved LR. */
    put32(MIX_F_ADD + 0u, 0x9421FFE0u);  /* stwu r1,-32(r1) */
    put32(MIX_F_ADD + 4u, 0x7C0802A6u);  /* mflr r0 */
    put32(MIX_F_ADD + 8u, 0x90010008u);  /* stw r0,8(r1) */
    put32(MIX_F_ADD + 12u, 0x7C632214u); /* add r3,r3,r4 */
    put32(MIX_F_ADD + 16u, 0x80010008u); /* lwz r0,8(r1) */
    put32(MIX_F_ADD + 20u, 0x7C0803A6u); /* mtlr r0 */
    put32(MIX_F_ADD + 24u, 0x38210020u); /* addi r1,r1,32 */
    put32(MIX_F_ADD + 28u, 0x4E800020u); /* blr */

    /* 0x1240: r3+r4 plus both stack arguments; r10 is kept as proof that the
     * eighth register argument arrived. */
    put32(MIX_F_STACK + 0u, 0x80A10018u);  /* lwz r5,24(r1) */
    put32(MIX_F_STACK + 4u, 0x80C1001Cu);  /* lwz r6,28(r1) */
    put32(MIX_F_STACK + 8u, 0x7C632214u);  /* add r3,r3,r4 */
    put32(MIX_F_STACK + 12u, 0x7C632A14u); /* add r3,r3,r5 */
    put32(MIX_F_STACK + 16u, 0x7C633214u); /* add r3,r3,r6 */
    put32(MIX_F_STACK + 20u, 0x91402204u); /* stw r10,0x2204(0) */
    put32(MIX_F_STACK + 24u, 0x4E800020u); /* blr */

    /* 0x1260: f1 = f1 + f2. */
    put32(MIX_F_FP + 0u, 0xEC21102Au); /* fadd f1,f1,f2 */
    put32(MIX_F_FP + 4u, 0x4E800020u); /* blr */

    /* 0x1280: import 1 with (r3,r4) = (7,35), result + 1. */
    put32(MIX_F_IMPORT + 0u, 0x9421FFE0u);  /* stwu r1,-32(r1) */
    put32(MIX_F_IMPORT + 4u, 0x7C0802A6u);  /* mflr r0 */
    put32(MIX_F_IMPORT + 8u, 0x90010008u);  /* stw r0,8(r1) */
    put32(MIX_F_IMPORT + 12u, 0x38000001u); /* li r0,1 */
    put32(MIX_F_IMPORT + 16u, 0x38600007u); /* li r3,7 */
    put32(MIX_F_IMPORT + 20u, 0x38800023u); /* li r4,35 */
    put32(MIX_F_IMPORT + 24u, 0x44000002u); /* sc */
    put32(MIX_F_IMPORT + 28u, 0x38630001u); /* addi r3,r3,1 */
    put32(MIX_F_IMPORT + 32u, 0x80010008u); /* lwz r0,8(r1) */
    put32(MIX_F_IMPORT + 36u, 0x7C0803A6u); /* mtlr r0 */
    put32(MIX_F_IMPORT + 40u, 0x38210020u); /* addi r1,r1,32 */
    put32(MIX_F_IMPORT + 44u, 0x4E800020u); /* blr */

    /* 0x12C0: import 2, which itself calls back into PPC. */
    put32(MIX_F_NESTED + 0u, 0x9421FFE0u);  /* stwu r1,-32(r1) */
    put32(MIX_F_NESTED + 4u, 0x7C0802A6u);  /* mflr r0 */
    put32(MIX_F_NESTED + 8u, 0x90010008u);  /* stw r0,8(r1) */
    put32(MIX_F_NESTED + 12u, 0x38000002u); /* li r0,2 */
    put32(MIX_F_NESTED + 16u, 0x44000002u); /* sc */
    put32(MIX_F_NESTED + 20u, 0x80010008u); /* lwz r0,8(r1) */
    put32(MIX_F_NESTED + 24u, 0x7C0803A6u); /* mtlr r0 */
    put32(MIX_F_NESTED + 28u, 0x38210020u); /* addi r1,r1,32 */
    put32(MIX_F_NESTED + 32u, 0x4E800020u); /* blr */

    /* 0x1300: leaves the stack pointer moved. */
    put32(MIX_F_BADSP + 0u, 0x9421FFF0u); /* stwu r1,-16(r1) */
    put32(MIX_F_BADSP + 4u, 0x4E800020u); /* blr */

    /* 0x1310: import index 15 is never registered. */
    put32(MIX_F_UNKNOWN + 0u, 0x3800000Fu); /* li r0,15 */
    put32(MIX_F_UNKNOWN + 4u, 0x44000002u); /* sc */

    /* 0x1320: illegal instruction, 0x1330: endless branch. */
    put32(MIX_F_ILLEGAL, 0x00000000u);
    put32(MIX_F_SPIN, 0x48000000u); /* b . */

    /* 0x1340: r3+r4 plus the first parameter-area word. */
    put32(MIX_F_SIG + 0u, 0x80A10018u);  /* lwz r5,24(r1) */
    put32(MIX_F_SIG + 4u, 0x7C632214u);  /* add r3,r3,r4 */
    put32(MIX_F_SIG + 8u, 0x7C632A14u);  /* add r3,r3,r5 */
    put32(MIX_F_SIG + 12u, 0x4E800020u); /* blr */

    /* 0x1360: f1 = f1 + the double at 24(r1). */
    put32(MIX_F_LFD + 0u, 0xC8410018u);  /* lfd f2,24(r1) */
    put32(MIX_F_LFD + 4u, 0xEC21102Au);  /* fadd f1,f1,f2 */
    put32(MIX_F_LFD + 8u, 0x4E800020u);  /* blr */

    /* 0x1380: import 3 with (r3,r4,r5,f1) = (0x11,0x22,MIX_SCRATCH,1.5). */
    put32(MIX_F_SIG_IMPORT + 0u, 0x9421FFE0u);  /* stwu r1,-32(r1) */
    put32(MIX_F_SIG_IMPORT + 4u, 0x38000003u);  /* li r0,3 */
    put32(MIX_F_SIG_IMPORT + 8u, 0x38600011u);  /* li r3,0x11 */
    put32(MIX_F_SIG_IMPORT + 12u, 0x38800022u); /* li r4,0x22 */
    put32(MIX_F_SIG_IMPORT + 16u, 0x3CA00000u); /* lis r5,0 */
    put32(MIX_F_SIG_IMPORT + 20u, 0x60A52200u); /* ori r5,r5,0x2200 */
    put32(MIX_F_SIG_IMPORT + 24u, 0xC8202100u); /* lfd f1,0x2100(0) */
    put32(MIX_F_SIG_IMPORT + 28u, 0x44000002u); /* sc */
    put32(MIX_F_SIG_IMPORT + 32u, 0x38630001u); /* addi r3,r3,1 */
    put32(MIX_F_SIG_IMPORT + 36u, 0x38210020u); /* addi r1,r1,32 */
    put32(MIX_F_SIG_IMPORT + 40u, 0x4E800020u); /* blr */

    /* 0x1400: import 4 with r3..r10 = 1..8 and 0x21/0x22 at 24(r1)/28(r1). */
    put32(MIX_F_SIG_STACK + 0u, 0x9421FFD0u);  /* stwu r1,-48(r1) */
    put32(MIX_F_SIG_STACK + 4u, 0x38600001u);  /* li r3,1 */
    put32(MIX_F_SIG_STACK + 8u, 0x38800002u);  /* li r4,2 */
    put32(MIX_F_SIG_STACK + 12u, 0x38A00003u); /* li r5,3 */
    put32(MIX_F_SIG_STACK + 16u, 0x38C00004u); /* li r6,4 */
    put32(MIX_F_SIG_STACK + 20u, 0x38E00005u); /* li r7,5 */
    put32(MIX_F_SIG_STACK + 24u, 0x39000006u); /* li r8,6 */
    put32(MIX_F_SIG_STACK + 28u, 0x39200007u); /* li r9,7 */
    put32(MIX_F_SIG_STACK + 32u, 0x39400008u); /* li r10,8 */
    put32(MIX_F_SIG_STACK + 36u, 0x38000021u); /* li r0,0x21 */
    put32(MIX_F_SIG_STACK + 40u, 0x90010018u); /* stw r0,24(r1) */
    put32(MIX_F_SIG_STACK + 44u, 0x38000022u); /* li r0,0x22 */
    put32(MIX_F_SIG_STACK + 48u, 0x9001001Cu); /* stw r0,28(r1) */
    put32(MIX_F_SIG_STACK + 52u, 0x38000004u); /* li r0,4 */
    put32(MIX_F_SIG_STACK + 56u, 0x44000002u); /* sc */
    put32(MIX_F_SIG_STACK + 60u, 0x38630001u); /* addi r3,r3,1 */
    put32(MIX_F_SIG_STACK + 64u, 0x38210030u); /* addi r1,r1,48 */
    put32(MIX_F_SIG_STACK + 68u, 0x4E800020u); /* blr */

    /* 0x1480: import 5 with a misaligned stack pointer. */
    put32(MIX_F_SIG_BADSP + 0u, 0x3C200000u);  /* lis r1,0 */
    put32(MIX_F_SIG_BADSP + 4u, 0x6021FFF8u);  /* ori r1,r1,0xFFF8 */
    put32(MIX_F_SIG_BADSP + 8u, 0x38000005u);  /* li r0,5 */
    put32(MIX_F_SIG_BADSP + 12u, 0x44000002u); /* sc */
    put64(0x2100u, double_bits(1.5));

    /* 0x14C0: import through the routine descriptor at 0x2040 with eight
     * register arguments and one stack word, then increment the result. */
    put32(MIX_F_PROC + 0u, 0x9421FFE0u);   /* stwu r1,-32(r1) */
    put32(MIX_F_PROC + 4u, 0x38600001u);   /* li r3,1 */
    put32(MIX_F_PROC + 8u, 0x38800002u);   /* li r4,2 */
    put32(MIX_F_PROC + 12u, 0x38A00003u);  /* li r5,3 */
    put32(MIX_F_PROC + 16u, 0x38C00004u);  /* li r6,4 */
    put32(MIX_F_PROC + 20u, 0x38E00005u);  /* li r7,5 */
    put32(MIX_F_PROC + 24u, 0x39000006u);  /* li r8,6 */
    put32(MIX_F_PROC + 28u, 0x39200007u);  /* li r9,7 */
    put32(MIX_F_PROC + 32u, 0x39400008u);  /* li r10,8 */
    put32(MIX_F_PROC + 36u, 0x39600055u);  /* li r11,0x55 */
    put32(MIX_F_PROC + 40u, 0x91610018u);  /* stw r11,24(r1) */
    put32(MIX_F_PROC + 44u, 0x38002040u);  /* li r0,0x2040 */
    put32(MIX_F_PROC + 48u, 0x44000002u);  /* sc */
    put32(MIX_F_PROC + 52u, 0x38630001u);  /* addi r3,r3,1 */
    put32(MIX_F_PROC + 56u, 0x38210020u);  /* addi r1,r1,32 */
    put32(MIX_F_PROC + 60u, 0x4E800020u);  /* blr */

    /* 0x1500: r0 addresses a descriptor that is never registered. */
    put32(MIX_F_PROC_BAD + 0u, 0x38002080u); /* li r0,0x2080 */
    put32(MIX_F_PROC_BAD + 4u, 0x44000002u); /* sc */

    /* 0x1540: nested PPC target: records its arguments, returns their sum. */
    put32(MIX_F_RDNEST + 0u, 0x90602210u);  /* stw r3,0x2210(0) */
    put32(MIX_F_RDNEST + 4u, 0x90802214u);  /* stw r4,0x2214(0) */
    put32(MIX_F_RDNEST + 8u, 0x7C632214u);  /* add r3,r3,r4 */
    put32(MIX_F_RDNEST + 12u, 0x4E800020u); /* blr */
    /* 0x1580: PPC caller whose import re-enters PPC through a descriptor. */
    put32(MIX_F_RDCALL + 0u, 0x9421FFE0u);  /* stwu r1,-32(r1) */
    put32(MIX_F_RDCALL + 4u, 0x7C0802A6u);  /* mflr r0 */
    put32(MIX_F_RDCALL + 8u, 0x90010008u);  /* stw r0,8(r1) */
    put32(MIX_F_RDCALL + 12u, 0x38000004u); /* li r0,4 */
    put32(MIX_F_RDCALL + 16u, 0x3860000Bu); /* li r3,11 */
    put32(MIX_F_RDCALL + 20u, 0x3880000Du); /* li r4,13 */
    put32(MIX_F_RDCALL + 24u, 0x44000002u); /* sc */
    put32(MIX_F_RDCALL + 28u, 0x38630001u); /* addi r3,r3,1 */
    put32(MIX_F_RDCALL + 32u, 0x80010008u); /* lwz r0,8(r1) */
    put32(MIX_F_RDCALL + 36u, 0x7C0803A6u); /* mtlr r0 */
    put32(MIX_F_RDCALL + 40u, 0x38210020u); /* addi r1,r1,32 */
    put32(MIX_F_RDCALL + 44u, 0x4E800020u); /* blr */
    /* 0x15C0: the same caller for the failing nested attempts. */
    put32(MIX_F_RDERR + 0u, 0x9421FFE0u);   /* stwu r1,-32(r1) */
    put32(MIX_F_RDERR + 4u, 0x7C0802A6u);   /* mflr r0 */
    put32(MIX_F_RDERR + 8u, 0x90010008u);   /* stw r0,8(r1) */
    put32(MIX_F_RDERR + 12u, 0x38000005u);  /* li r0,5 */
    put32(MIX_F_RDERR + 16u, 0x44000002u);  /* sc */
    put32(MIX_F_RDERR + 20u, 0x80010008u);  /* lwz r0,8(r1) */
    put32(MIX_F_RDERR + 24u, 0x7C0803A6u);  /* mtlr r0 */
    put32(MIX_F_RDERR + 28u, 0x38210020u);  /* addi r1,r1,32 */
    put32(MIX_F_RDERR + 32u, 0x4E800020u);  /* blr */
    /* 0x1600: the ProcInfo target, r3 + r4 + r5. */
    put32(MIX_F_PI + 0u, 0x7C632214u);  /* add r3,r3,r4 */
    put32(MIX_F_PI + 4u, 0x7C632A14u);  /* add r3,r3,r5 */
    put32(MIX_F_PI + 8u, 0x4E800020u);  /* blr */
    /* 0x1620: the parameterless ProcInfo target. */
    put32(MIX_F_PI0 + 0u, 0x3860004Du); /* li r3,77 */
    put32(MIX_F_PI0 + 4u, 0x4E800020u); /* blr */
    /* 0x1640: descriptor import whose placement comes from ProcInfo. */
    put32(MIX_F_PROC_PI + 0u, 0x38600001u);  /* li r3,1 */
    put32(MIX_F_PROC_PI + 4u, 0x38800002u);  /* li r4,2 */
    put32(MIX_F_PROC_PI + 8u, 0x38A00003u);  /* li r5,3 */
    put32(MIX_F_PROC_PI + 12u, 0x380022C0u); /* li r0,0x22C0 */
    put32(MIX_F_PROC_PI + 16u, 0x44000002u); /* sc */
    put32(MIX_F_PROC_PI + 20u, 0x38630001u); /* addi r3,r3,1 */
    put32(MIX_F_PROC_PI + 24u, 0x4E800020u); /* blr */
    /* 0x1680: the same through the 68k-only descriptor at 0x2320. */
    put32(MIX_F_PROC_PI68 + 0u, 0x38600029u); /* li r3,41 */
    put32(MIX_F_PROC_PI68 + 4u, 0x38002320u); /* li r0,0x2320 */
    put32(MIX_F_PROC_PI68 + 8u, 0x44000002u); /* sc */
    put32(MIX_F_PROC_PI68 + 12u, 0x4E800020u); /* blr */
    /* 0x16C0: an index import whose placement comes from a bound descriptor. */
    put32(MIX_F_IDX_PI + 0u, 0x38600001u);  /* li r3,1 */
    put32(MIX_F_IDX_PI + 4u, 0x38800002u);  /* li r4,2 */
    put32(MIX_F_IDX_PI + 8u, 0x38A00003u);  /* li r5,3 */
    put32(MIX_F_IDX_PI + 12u, 0x38000005u); /* li r0,5 */
    put32(MIX_F_IDX_PI + 16u, 0x44000002u); /* sc */
    put32(MIX_F_IDX_PI + 20u, 0x38630001u); /* addi r3,r3,1 */
    put32(MIX_F_IDX_PI + 24u, 0x4E800020u); /* blr */
}

static qrx_mix_status_t mix_setup(qrx_mix_t *mix, mix_ctx_t *ctx)
{
    mix_load();
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
        ctx->mix = mix;
    }
    return qrx_mix_init(mix, &ppc, MIX_GLUE, MIX_SP_TOP, MIX_SP_LIMIT);
}

static void test_mixed_call(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_mix_status_t st;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_call_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_call_glue_word", get32(MIX_GLUE), 0x44000002u);
    in.gpr[0] = 3u;
    in.gpr[1] = 4u;
    in.gpr_count = 2u;
    st = qrx_mix_call(&mix, MIX_F_ADD, QRX_MIX_RAW_SIG, &in, &out, 1000u);
    expect_u32("mixed_call_status", (uint32_t)st, (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_call_result", out.gpr[0], 7u);
    /* The callee built its frame below the glue SP and saved LR at 8(SP). */
    expect_u32("mixed_call_frame", get32(MIX_GLUE_SP - 32u), MIX_GLUE_SP);
    expect_u32("mixed_call_saved_lr", get32(MIX_GLUE_SP - 24u), MIX_GLUE);
    expect_u32("mixed_call_back_chain", get32(MIX_GLUE_SP), 0u);
    report("mixed_call", before);
}

static void test_mixed_stack(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned i;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_stack_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    for (i = 0; i < QRX_MIX_ARGS; i++)
        in.gpr[i] = (uint32_t)i + 1u;
    in.gpr_count = QRX_MIX_ARGS;
    in.stack[0] = 9u;
    in.stack[1] = 10u;
    in.stack_count = 2u;
    expect_u32("mixed_stack_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_STACK, QRX_MIX_RAW_SIG, &in, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_stack_result", out.gpr[0], 22u);
    expect_u32("mixed_stack_param0", get32(MIX_GLUE_SP + 24u), 9u);
    expect_u32("mixed_stack_param1", get32(MIX_GLUE_SP + 28u), 10u);
    expect_u32("mixed_stack_last_reg", get32(MIX_SCRATCH + 4u), 8u);
    report("mixed_stack", before);
}

static void test_mixed_fpu(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_fpu_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    in.fpr[0] = 1.5;
    in.fpr[1] = 2.25;
    in.fpr_count = 2u;
    expect_u32("mixed_fpu_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_FP, QRX_MIX_RAW_SIG, &in, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u64("mixed_fpu_result", double_bits(out.fpr), double_bits(3.75));
    report("mixed_fpu", before);
}

static void test_mixed_import(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_import_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_import_register",
               (uint32_t)qrx_mix_register(&mix, 1u, QRX_MIX_RAW, mix_import_add, &ctx, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_import_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IMPORT, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_import_result", out.gpr[0], 43u);
    expect_u32("mixed_import_calls", ctx.calls, 1u);
    expect_u32("mixed_import_arg0", ctx.seen[0], 7u);
    expect_u32("mixed_import_arg1", ctx.seen[1], 35u);
    expect_u32("mixed_import_68k_store", get32(MIX_SCRATCH), 42u);
    report("mixed_import", before);
}

static void test_mixed_nested(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_nested_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_nested_register",
               (uint32_t)qrx_mix_register(&mix, 2u, QRX_MIX_RAW, mix_import_nested, &ctx, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_nested_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_NESTED, QRX_MIX_RAW_SIG, 0, &out, 10000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_nested_result", out.gpr[0], 47u);
    expect_u32("mixed_nested_inner", ctx.nested_ok, 1u);
    report("mixed_nested", before);
}

static void test_mixed_errors(void)
{
    qrx_mix_t mix;
    qrx_mix_result_t out;
    uint32_t pc_before;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    mix_load();
    memset(&mix, 0, sizeof(mix));
    expect_u32("mixed_errors_bad_stack",
               (uint32_t)qrx_mix_init(&mix, &ppc, MIX_GLUE, MIX_SP_TOP + 4u,
                                      MIX_SP_LIMIT),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_errors_small_stack",
               (uint32_t)qrx_mix_init(&mix, &ppc, MIX_GLUE, MIX_SP_TOP,
                                      MIX_SP_TOP),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_errors_init",
               (uint32_t)qrx_mix_init(&mix, &ppc, MIX_GLUE, MIX_SP_TOP,
                                      MIX_SP_LIMIT),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_errors_unknown_import",
               (uint32_t)qrx_mix_call(&mix, MIX_F_UNKNOWN, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_IMPORT);
    expect_u32("mixed_errors_illegal",
               (uint32_t)qrx_mix_call(&mix, MIX_F_ILLEGAL, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RUN);
    expect_u32("mixed_errors_limit",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SPIN, QRX_MIX_RAW_SIG, 0, &out, 100u),
               (uint32_t)QRX_MIX_ERR_LIMIT);
    expect_u32("mixed_errors_stack_restore",
               (uint32_t)qrx_mix_call(&mix, MIX_F_BADSP, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_SP);
    /* A missing signature is no longer a licence for the raw registers, and
     * the rejected call does not start the routine. */
    pc_before = ppc.cpu.pc;
    expect_u32("mixed_errors_raw_sig",
               (uint32_t)qrx_mix_call(&mix, MIX_F_ADD, 0, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_errors_raw_sig_pc", (uint32_t)ppc.cpu.pc,
               (uint32_t)pc_before);
    report("mixed_errors", before);
}

static void test_mixed_sig_call(void)
{
    qrx_mix_t mix;
    qrx_mix_sig_t sig;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned i;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_sig_call_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "iiiiiiiiii", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_call_params", sig.nparams, 10u);
    expect_u32("mixed_sig_call_ret", (uint32_t)sig.ret, (uint32_t)'i');
    expect_u32("mixed_sig_call_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    for (i = 0; i < QRX_MIX_ARGS; i++)
        in.gpr[i] = i + 1u;
    in.gpr_count = QRX_MIX_ARGS;
    in.stack[0] = 9u;
    in.stack[1] = 10u;
    in.stack_count = 2u;
    expect_u32("mixed_sig_call_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SIG, &sig, &in, &out,
                                      1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_call_result", out.gpr[0], 12u);
    expect_u32("mixed_sig_call_stack0", get32(MIX_GLUE_SP + 24u), 9u);
    expect_u32("mixed_sig_call_stack1", get32(MIX_GLUE_SP + 28u), 10u);
    expect_u32("mixed_sig_call_r4_cleared", out.gpr[1], 0u);
    report("mixed_sig_call", before);
}

static void test_mixed_sig_fp(void)
{
    qrx_mix_t mix;
    qrx_mix_sig_t sig;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned i;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_sig_fp_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "ddddddddd", 'f'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_fp_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    for (i = 0; i < QRX_MIX_ARGS; i++)
        in.fpr[i] = (double)i + 1.0;
    in.fpr_count = QRX_MIX_ARGS;
    in.stack[0] = (uint32_t)(double_bits(9.0) >> 32);
    in.stack[1] = (uint32_t)double_bits(9.0);
    in.stack_count = 2u;
    expect_u32("mixed_sig_fp_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_LFD, &sig, &in, &out,
                                      1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u64("mixed_sig_fp_result", double_bits(out.fpr), double_bits(10.0));
    expect_u32("mixed_sig_fp_stack0", get32(MIX_GLUE_SP + 24u), 0x40220000u);
    expect_u32("mixed_sig_fp_stack1", get32(MIX_GLUE_SP + 28u), 0u);
    expect_u32("mixed_sig_fp_gpr_masked", out.gpr[0], 0u);
    report("mixed_sig_fp", before);
}

static void test_mixed_sig_import(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_sig_t sig;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_sig_import_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "iipd", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_import_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_import_register",
               (uint32_t)qrx_mix_register(&mix, 3u, 0u, mix_import_sig, &ctx,
                                          &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_import_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SIG_IMPORT, QRX_MIX_RAW_SIG, 0, &out,
                                      1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_import_result", out.gpr[0], 8u);
    expect_u32("mixed_sig_import_gcount", ctx.gcount, 3u);
    expect_u32("mixed_sig_import_fcount", ctx.fcount, 1u);
    expect_u32("mixed_sig_import_int0", ctx.seen[0], 0x11u);
    expect_u32("mixed_sig_import_int1", ctx.seen[1], 0x22u);
    expect_u32("mixed_sig_import_ptr", ctx.ptr, MIX_SCRATCH);
    expect_u64("mixed_sig_import_f0", double_bits(ctx.f0), double_bits(1.5));
    expect_u32("mixed_sig_import_store", get32(MIX_SCRATCH), 0x55u);
    report("mixed_sig_import", before);
}

static void test_mixed_sig_stack(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_sig_t sig;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_sig_stack_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "iiiiiiiiii", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_stack_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_stack_register",
               (uint32_t)qrx_mix_register(&mix, 4u, 0u, mix_import_stack, &ctx,
                                          &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_stack_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SIG_STACK, QRX_MIX_RAW_SIG, 0, &out,
                                      1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_stack_result", out.gpr[0], 104u);
    expect_u32("mixed_sig_stack_gcount", ctx.gcount, QRX_MIX_ARGS);
    expect_u32("mixed_sig_stack_count", ctx.stack_count, 2u);
    expect_u32("mixed_sig_stack_word0", ctx.stack0, 0x21u);
    expect_u32("mixed_sig_stack_word1", ctx.stack1, 0x22u);
    expect_u32("mixed_sig_stack_sp", ctx.sp, MIX_GLUE_SP - 24u);
    report("mixed_sig_stack", before);
}

static void test_mixed_sig_errors(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_sig_t sig;
    qrx_mix_sig_t bad;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&bad, 0, sizeof(bad));
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_sig_errors_type",
               (uint32_t)qrx_mix_sig_parse(&bad, "iz", 'i'),
               (uint32_t)QRX_MIX_ERR_SIG);
    expect_u32("mixed_sig_errors_ret",
               (uint32_t)qrx_mix_sig_parse(&bad, "i", 'x'),
               (uint32_t)QRX_MIX_ERR_SIG);
    expect_u32("mixed_sig_errors_null",
               (uint32_t)qrx_mix_sig_parse(&bad, 0, 'i'),
               (uint32_t)QRX_MIX_ERR_SIG);
    expect_u32("mixed_sig_errors_too_long",
               (uint32_t)qrx_mix_sig_parse(&bad, "iiiiiiiiiiiiiiiii", 'i'),
               (uint32_t)QRX_MIX_ERR_SIG);
    expect_u32("mixed_sig_errors_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_errors_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "ii", 'i'),
               (uint32_t)QRX_MIX_OK);
    in.gpr[0] = 1u;
    in.gpr_count = 1u;
    expect_u32("mixed_sig_errors_arg_count",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SIG, &sig, &in, &out,
                                      1000u),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_sig_errors_parse10",
               (uint32_t)qrx_mix_sig_parse(&sig, "iiiiiiiiii", 'i'),
               (uint32_t)QRX_MIX_OK);
    in.gpr_count = QRX_MIX_ARGS;
    in.stack_count = 1u;
    expect_u32("mixed_sig_errors_stack_count",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SIG, &sig, &in, &out,
                                      1000u),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_sig_errors_register",
               (uint32_t)qrx_mix_register(&mix, 5u, 0u, mix_import_sig, &ctx,
                                          &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_sig_errors_bad_sp",
               (uint32_t)qrx_mix_call(&mix, MIX_F_SIG_BADSP, QRX_MIX_RAW_SIG, 0, &out,
                                      1000u),
               (uint32_t)QRX_MIX_ERR_ARG);
    report("mixed_sig_errors", before);
}

#define MIX_RD 0x2000u
#define MIX_RD_REL 0x1000u
#define MIX_RD_END 0x3FFF0u
#define MIX_PROC_RD 0x2040u
#define MIX_PROC_BAD 0x2080u

static void mix_put8(uint32_t addr, uint8_t value)
{
    memory[addr] = value;
}

static void mix_put16(uint32_t addr, uint16_t value)
{
    memory[addr] = (uint8_t)(value >> 8);
    memory[addr + 1u] = (uint8_t)value;
}

/* RoutineDescriptor header; the records follow at rd + QRX_MIX_RD_HEADER. */
static void mix_rd_header(uint32_t rd, int version, uint16_t count)
{
    mix_put16(rd + 0u, QRX_MIX_RD_TRAP);
    mix_put8(rd + 2u, (uint8_t)version);
    mix_put8(rd + 3u, 0u);
    put32(rd + 4u, 0u);
    mix_put8(rd + 8u, 0u);
    mix_put8(rd + 9u, 0u);
    mix_put16(rd + 10u, count);
}

static void mix_rd_record_pi(uint32_t rr, int isa, uint32_t proc_info,
                             uint32_t entry, uint16_t flags)
{
    put32(rr + 0u, proc_info);
    mix_put8(rr + 4u, 0u);
    mix_put8(rr + 5u, (uint8_t)isa);
    mix_put16(rr + 6u, flags);
    put32(rr + 8u, entry);
    put32(rr + 12u, 0u);
    put32(rr + 16u, 0u);
}

static void mix_rd_record(uint32_t rr, int isa, uint32_t entry,
                          uint16_t flags)
{
    /* 0xA1B2 describes no placement; these tests pass a signature. */
    mix_rd_record_pi(rr, isa, 0x0000A1B2u, entry, flags);
}

static void test_mixed_rd(void)
{
    qrx_mix_t mix;
    qrx_mix_sig_t sig;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_mix_rd_t rd;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    memset(&rd, 0, sizeof(rd));
    expect_u32("mixed_rd_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "iiiiiiiii", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  0u);
    expect_u32("mixed_rd_resolve",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_descriptor", rd.descriptor, MIX_RD);
    expect_u32("mixed_rd_record", rd.record, MIX_RD + QRX_MIX_RD_HEADER);
    expect_u32("mixed_rd_entry", rd.entry, MIX_F_SIG);
    expect_u32("mixed_rd_count", rd.record_count, 1u);
    expect_u32("mixed_rd_isa", (uint32_t)rd.isa, (uint32_t)QRX_MIX_RD_ISA_PPC);
    expect_u32("mixed_rd_proc_info", rd.proc_info, 0x0000A1B2u);
    expect_u32("mixed_rd_flags", rd.routine_flags, 0u);
    /* Reserved fields are ignored, as the Mixed Mode Manager does. */
    put32(MIX_RD + 4u, 0xFFFFFFFFu);
    mix_put8(MIX_RD + 8u, 0x7Fu);
    mix_put8(MIX_RD + 9u, 0xFFu);
    put32(MIX_RD + QRX_MIX_RD_HEADER + 16u, 0x12345678u);
    expect_u32("mixed_rd_reserved",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_reserved_entry", rd.entry, MIX_F_SIG);
    /* Call through the descriptor: r3 + r4 + the first stack word. */
    in.gpr[0] = 7u;
    in.gpr[1] = 35u;
    in.gpr_count = QRX_MIX_ARGS;
    in.stack[0] = 5u;
    in.stack_count = 1u;
    expect_u32("mixed_rd_call",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                         &sig, &in, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_result", out.gpr[0], 47u);
    expect_u32("mixed_rd_stack_word", get32(MIX_GLUE_SP + 24u), 5u);
    /* Fat descriptor: 68k record first, PPC record second. */
    mix_rd_header(MIX_RD, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record(MIX_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K, 0x1110u, 0u);
    mix_rd_record(MIX_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                  QRX_MIX_RD_ISA_PPC, MIX_F_SIG, 0u);
    expect_u32("mixed_rd_fat_ppc",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_fat_ppc_record", rd.record,
               MIX_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD);
    expect_u32("mixed_rd_fat_ppc_entry", rd.entry, MIX_F_SIG);
    expect_u32("mixed_rd_fat_count", rd.record_count, 2u);
    expect_u32("mixed_rd_fat_68k",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_68K,
                                            &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_fat_68k_entry", rd.entry, 0x1110u);
    expect_u32("mixed_rd_fat_68k_isa", (uint32_t)rd.isa,
               (uint32_t)QRX_MIX_RD_ISA_68K);
    /* A relative procDescriptor is resolved against the descriptor. */
    mix_rd_header(MIX_RD_REL, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_RD_REL + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                  MIX_F_SIG - MIX_RD_REL, QRX_MIX_RD_FLAG_RELATIVE);
    expect_u32("mixed_rd_relative",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD_REL,
                                            QRX_MIX_RD_ISA_PPC, &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_relative_entry", rd.entry, MIX_F_SIG);
    expect_u32("mixed_rd_relative_flags", rd.routine_flags,
               QRX_MIX_RD_FLAG_RELATIVE);
    in.gpr[0] = 1u;
    in.gpr[1] = 2u;
    in.stack[0] = 3u;
    expect_u32("mixed_rd_relative_call",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_REL,
                                         QRX_MIX_RD_ISA_PPC, &sig, &in, &out,
                                         1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_relative_result", out.gpr[0], 6u);
    report("mixed_rd", before);
}

static void test_mixed_rd_errors(void)
{
    qrx_mix_t mix;
    qrx_mix_rd_t rd;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    uint32_t saved_pc;
    uint32_t rr = MIX_RD + QRX_MIX_RD_HEADER;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    memset(&rd, 0, sizeof(rd));
    expect_u32("mixed_rd_errors_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG, 0u);
    /* A word other than the Mixed Mode A-trap is not a descriptor. */
    mix_put16(MIX_RD + 0u, 0xAB1Du);
    expect_u32("mixed_rd_errors_trap",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_put16(MIX_RD + 0u, QRX_MIX_RD_TRAP);
    mix_put8(MIX_RD + 2u, 6u);
    expect_u32("mixed_rd_errors_version",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_put8(MIX_RD + 2u, QRX_MIX_RD_VERSION);
    /* Dispatched descriptors are not supported. */
    mix_put8(MIX_RD + 3u, 0x01u);
    expect_u32("mixed_rd_errors_dispatched",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_put8(MIX_RD + 3u, 0u);
    mix_put16(MIX_RD + 10u, QRX_MIX_RD_MAX_RECORDS);
    expect_u32("mixed_rd_errors_count",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    /* The header fits, the first record does not. */
    mix_rd_header(MIX_RD_END, QRX_MIX_RD_VERSION, 0u);
    expect_u32("mixed_rd_errors_truncated",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD_END,
                                            QRX_MIX_RD_ISA_PPC, &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_errors_outside",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD_END + 16u,
                                            QRX_MIX_RD_ISA_PPC, &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    /* No PowerPC record at all. */
    mix_rd_header(MIX_RD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(rr, QRX_MIX_RD_ISA_68K, 0x1110u, 0u);
    expect_u32("mixed_rd_errors_no_ppc",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    /* Unsupported routine flags. */
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  QRX_MIX_RD_FLAG_NEEDS_PREP);
    expect_u32("mixed_rd_errors_prepare",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  QRX_MIX_RD_FLAG_SELECTOR);
    expect_u32("mixed_rd_errors_selector",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  QRX_MIX_RD_FLAG_INDEXABLE);
    expect_u32("mixed_rd_errors_indexable",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG, 0x0040u);
    expect_u32("mixed_rd_errors_flag",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    /* Unusable entry points. */
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, 0u, 0u);
    expect_u32("mixed_rd_errors_entry_zero",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG + 1u, 0u);
    expect_u32("mixed_rd_errors_entry_align",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, 0x40000u, 0u);
    expect_u32("mixed_rd_errors_entry_limit",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A relative offset that wraps below the descriptor address. */
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, 0xFFFFF000u,
                  QRX_MIX_RD_FLAG_RELATIVE);
    expect_u32("mixed_rd_errors_underflow",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    /* Records that are not selected are not validated. */
    mix_rd_header(MIX_RD, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record(rr, QRX_MIX_RD_ISA_68K, 0x1111u, 0u);
    mix_rd_record(rr + QRX_MIX_RD_RECORD, QRX_MIX_RD_ISA_PPC, MIX_F_SIG, 0u);
    expect_u32("mixed_rd_errors_other_record",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(rr, QRX_MIX_RD_ISA_PPC, MIX_F_SIG, 0u);
    expect_u32("mixed_rd_errors_isa",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, 7, &rd),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_errors_null",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            0),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_rd_errors_68k_call",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD, QRX_MIX_RD_ISA_68K, 0,
                                         &in, &out, 100u),
               (uint32_t)QRX_MIX_ERR_RD);
    /* Resolving never runs guest code. */
    saved_pc = ppc.cpu.pc;
    ppc.cpu.pc = MIX_F_ILLEGAL;
    expect_u32("mixed_rd_errors_pure",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD, QRX_MIX_RD_ISA_PPC,
                                            &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_errors_pure_pc", ppc.cpu.pc, MIX_F_ILLEGAL);
    ppc.cpu.pc = saved_pc;
    report("mixed_rd_errors", before);
}

static void test_mixed_proc_import(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_sig_t sig;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_proc_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "iiiiiiiii", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    /* Fat descriptor: an import must select the 68k record. */
    mix_rd_header(MIX_PROC_RD, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record(MIX_PROC_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K, 0x1110u,
                  0u);
    mix_rd_record(MIX_PROC_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                  QRX_MIX_RD_ISA_PPC, MIX_F_SIG, 0u);
    expect_u32("mixed_proc_register",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_RD,
                                               mix_import_proc, &ctx, &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_calls", ctx.calls, 1u);
    expect_u32("mixed_proc_entry", ctx.proc, 0x1110u);
    expect_u32("mixed_proc_gcount", ctx.gcount, QRX_MIX_ARGS);
    expect_u32("mixed_proc_stack_count", ctx.stack_count, 1u);
    expect_u32("mixed_proc_stack_word", ctx.stack0, 0x55u);
    expect_u32("mixed_proc_sp", ctx.sp, MIX_GLUE_SP - 8u);
    expect_u32("mixed_proc_sum", ctx.sum, 36u + 0x55u);
    expect_u32("mixed_proc_result", out.gpr[0], 37u + 0x55u);
    report("mixed_proc_import", before);
}

static void test_mixed_proc_errors(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_sig_t sig;
    qrx_mix_sig_t bad;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_proc_errors_parse",
               (uint32_t)qrx_mix_sig_parse(&sig, "ii", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_errors_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    /* Values below QRX_MIX_IMPORTS stay import indices. */
    expect_u32("mixed_proc_errors_low",
               (uint32_t)qrx_mix_register_proc(&mix, 4u, mix_import_proc, &ctx,
                                               &sig),
               (uint32_t)QRX_MIX_ERR_ARG);
    /* Not a routine descriptor. */
    mix_rd_header(MIX_PROC_BAD, QRX_MIX_RD_VERSION, 0u);
    mix_put16(MIX_PROC_BAD, 0xAB1Du);
    expect_u32("mixed_proc_errors_bad_rd",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_BAD,
                                               mix_import_proc, &ctx, &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    /* No 68k record to call. */
    mix_rd_header(MIX_PROC_RD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_PROC_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  0u);
    expect_u32("mixed_proc_errors_no_68k",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_RD,
                                               mix_import_proc, &ctx, &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    mix_rd_record(MIX_PROC_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K, 0x1110u,
                  0u);
    bad = sig;
    bad.ret = 'x';
    expect_u32("mixed_proc_errors_sig",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_RD,
                                               mix_import_proc, &ctx, &bad),
               (uint32_t)QRX_MIX_ERR_SIG);
    expect_u32("mixed_proc_errors_register",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_RD,
                                               mix_import_proc, &ctx, &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_errors_duplicate",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_RD,
                                               mix_import_proc, &ctx, &sig),
               (uint32_t)QRX_MIX_ERR_ARG);
    /* r0 holds a descriptor address that was never registered. */
    expect_u32("mixed_proc_errors_unregistered",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_BAD, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_IMPORT);
    /* The descriptor is resolved again on every call. */
    mix_rd_record(MIX_PROC_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  0u);
    expect_u32("mixed_proc_errors_mutated",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    report("mixed_proc_errors", before);
}

static void test_mixed_rd_nested(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_result_t out;
    uint32_t outer_r1 = MIX_GLUE_SP - 32u;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_nested_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD_CALL, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record(MIX_RD_CALL + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K, 0x2220u,
                  0u);
    mix_rd_record(MIX_RD_CALL + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                  QRX_MIX_RD_ISA_PPC, MIX_F_RDNEST, 0u);
    mix_rd_header(MIX_RD_SIG, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_RD_SIG + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC, MIX_F_SIG,
                  0u);
    expect_u32("mixed_rd_nested_register",
               (uint32_t)qrx_mix_register(&mix, 4u, QRX_MIX_RAW, mix_import_rd_call, &ctx, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_nested_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_RDCALL, QRX_MIX_RAW_SIG, 0, &out, 4000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_nested_calls", ctx.calls, 1u);
    expect_u32("mixed_rd_nested_ppc_record", ctx.rd_ok, 1u);
    expect_u32("mixed_rd_nested_marker0", ctx.rd_marker[0], 11u);
    expect_u32("mixed_rd_nested_marker1", ctx.rd_marker[1], 13u);
    expect_u32("mixed_rd_nested_back_chain", ctx.rd_back, outer_r1);
    expect_u32("mixed_rd_nested_sp", ctx.rd_sp_seen,
               (outer_r1 - QRX_MIX_LINKAGE) & ~0xFu);
    expect_u32("mixed_rd_nested_nine", ctx.rd_ok2, 1u);
    expect_u32("mixed_rd_nested_stack_word", ctx.rd_stack, 9u);
    expect_u32("mixed_rd_nested_restore", ctx.rd_restore, 1u);
    expect_u32("mixed_rd_nested_depth", (uint32_t)mix.depth, 0u);
    expect_u32("mixed_rd_nested_result", out.gpr[0], 26u);
    report("mixed_rd_nested", before);
}

static void test_mixed_rd_nested_errors(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_nested_errors_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD_68K, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_RD_68K + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K, 0x2220u,
                  0u);
    mix_rd_header(MIX_RD_SPIN, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_RD_SPIN + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                  MIX_F_SPIN, 0u);
    mix_rd_header(MIX_RD_BADSP, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record(MIX_RD_BADSP + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                  MIX_F_BADSP, 0u);
    expect_u32("mixed_rd_nested_errors_register",
               (uint32_t)qrx_mix_register(&mix, 5u, QRX_MIX_RAW, mix_import_rd_errors, &ctx,
                                          0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_nested_errors_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_RDERR, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_nested_errors_calls", ctx.calls, 1u);
    expect_u32("mixed_rd_nested_errors_68k", ctx.rd_rd, 1u);
    expect_u32("mixed_rd_nested_errors_bad", ctx.rd_bad, 1u);
    expect_u32("mixed_rd_nested_errors_limit", ctx.rd_limit, 1u);
    expect_u32("mixed_rd_nested_errors_sp", ctx.rd_sp, 1u);
    expect_u32("mixed_rd_nested_errors_depth", (uint32_t)mix.depth, 0u);
    expect_u32("mixed_rd_nested_errors_result", out.gpr[0], 9u);
    report("mixed_rd_nested_errors", before);
}

/* ProcInfo field helper: one 2-bit size code per parameter, five bits apart. */
#define MIX_PI_PARAM(n, size) ((uint32_t)(size) << (11u + 5u * ((n) - 1u)))
#define MIX_PI_THREE \
    (2u | (3u << 4) | MIX_PI_PARAM(1, 3) | MIX_PI_PARAM(2, 3) | \
     MIX_PI_PARAM(3, 3))
#define MIX_PI_TWO (2u | (3u << 4) | MIX_PI_PARAM(1, 3) | MIX_PI_PARAM(2, 3))

/* Rewrite the scratch descriptor with `proc_info` and decode it. */
static qrx_mix_status_t mix_pi_decode(qrx_mix_t *mix, uint32_t descriptor,
                                      uint32_t proc_info, uint32_t entry,
                                      qrx_mix_rd_t *rd, qrx_mix_sig_t *sig)
{
    qrx_mix_status_t rc;

    mix_rd_header(descriptor, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(descriptor + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     proc_info, entry, 0u);
    rc = qrx_mix_rd_resolve(mix, descriptor, QRX_MIX_RD_ISA_PPC, rd);
    if (rc != QRX_MIX_OK)
        return rc;
    return qrx_mix_rd_procinfo_sig(rd, sig);
}

static void test_mixed_rd_procinfo(void)
{
    qrx_mix_t mix;
    qrx_mix_rd_t rd;
    qrx_mix_sig_t sig;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    unsigned before = failures;

    memset(&rd, 0, sizeof(rd));
    memset(&sig, 0, sizeof(sig));
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_procinfo_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    /* Three 4-byte parameters, 4-byte result. */
    mix_rd_header(MIX_RD_PI, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     2u | (3u << 4) | MIX_PI_PARAM(1, 3) | MIX_PI_PARAM(2, 3) |
                         MIX_PI_PARAM(3, 3),
                     MIX_F_PI, 0u);
    expect_u32("mixed_rd_procinfo_resolve",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD_PI,
                                            QRX_MIX_RD_ISA_PPC, &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_decode",
               (uint32_t)qrx_mix_rd_procinfo_sig(&rd, &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_count", sig.nparams, 3u);
    expect_u32("mixed_rd_procinfo_p0", (uint32_t)(uint8_t)sig.params[0],
               (uint32_t)'i');
    expect_u32("mixed_rd_procinfo_p1", (uint32_t)(uint8_t)sig.params[1],
               (uint32_t)'i');
    expect_u32("mixed_rd_procinfo_p2", (uint32_t)(uint8_t)sig.params[2],
               (uint32_t)'i');
    expect_u32("mixed_rd_procinfo_ret", (uint32_t)(uint8_t)sig.ret,
               (uint32_t)'i');
    in.gpr[0] = 4u;
    in.gpr[1] = 5u;
    in.gpr[2] = 6u;
    in.gpr_count = 3u;
    expect_u32("mixed_rd_procinfo_call",
               (uint32_t)qrx_mix_rd_invoke_procinfo(&mix, MIX_RD_PI, &in, &out,
                                                    1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_result", out.gpr[0], 15u);
    expect_u32("mixed_rd_procinfo_depth", (uint32_t)mix.depth, 0u);
    /* No parameters, no result: the entry is still called. */
    mix_rd_header(MIX_RD_PI0, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI0 + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC, 2u,
                     MIX_F_PI0, 0u);
    expect_u32("mixed_rd_procinfo_void_resolve",
               (uint32_t)qrx_mix_rd_resolve(&mix, MIX_RD_PI0,
                                            QRX_MIX_RD_ISA_PPC, &rd),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_void_decode",
               (uint32_t)qrx_mix_rd_procinfo_sig(&rd, &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_void_count", sig.nparams, 0u);
    expect_u32("mixed_rd_procinfo_void_ret", (uint32_t)(uint8_t)sig.ret,
               (uint32_t)'v');
    memset(&in, 0, sizeof(in));
    expect_u32("mixed_rd_procinfo_void_call",
               (uint32_t)qrx_mix_rd_invoke_procinfo(&mix, MIX_RD_PI0, &in, &out,
                                                    1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_void_result", out.gpr[0], 0u);
    /* A 1-byte and a 2-byte parameter each still occupy one register. */
    mix_rd_header(MIX_RD_PINARROW, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PINARROW + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     2u | (1u << 4) | MIX_PI_PARAM(1, 1) | MIX_PI_PARAM(2, 2),
                     MIX_F_ADD, 0u);
    expect_u32("mixed_rd_procinfo_narrow_decode",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PINARROW,
                                       2u | (1u << 4) | MIX_PI_PARAM(1, 1) |
                                           MIX_PI_PARAM(2, 2),
                                       MIX_F_ADD, &rd, &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_narrow_count", sig.nparams, 2u);
    expect_u32("mixed_rd_procinfo_narrow_p1", (uint32_t)(uint8_t)sig.params[1],
               (uint32_t)'i');
    expect_u32("mixed_rd_procinfo_narrow_ret", (uint32_t)(uint8_t)sig.ret,
               (uint32_t)'i');
    memset(&in, 0, sizeof(in));
    in.gpr[0] = 20u;
    in.gpr[1] = 22u;
    in.gpr_count = 2u;
    expect_u32("mixed_rd_procinfo_narrow_call",
               (uint32_t)qrx_mix_rd_invoke_procinfo(&mix, MIX_RD_PINARROW, &in,
                                                    &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_narrow_result", out.gpr[0], 42u);
    /* Four parameters are the last the record can describe. */
    expect_u32("mixed_rd_procinfo_four",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PINARROW,
                                       2u | (3u << 4) | MIX_PI_PARAM(1, 3) |
                                           MIX_PI_PARAM(2, 3) |
                                           MIX_PI_PARAM(3, 3) |
                                           MIX_PI_PARAM(4, 3),
                                       MIX_F_PI, &rd, &sig),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_procinfo_four_count", sig.nparams, 4u);
    report("mixed_rd_procinfo", before);
}

static void test_mixed_rd_procinfo_errors(void)
{
    qrx_mix_t mix;
    qrx_mix_rd_t rd;
    qrx_mix_sig_t sig;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    uint32_t pc;
    unsigned before = failures;

    memset(&rd, 0, sizeof(rd));
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_procinfo_errors_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    memset(&sig, 0x5A, sizeof(sig));
    /* Stack-based, dispatched and special-case conventions are not
     * placement the bridge can reproduce in registers. */
    expect_u32("mixed_rd_procinfo_errors_pascal",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD, 0u, MIX_F_PI, &rd,
                                       &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_procinfo_errors_cstack",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD, 1u, MIX_F_PI, &rd,
                                       &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_procinfo_errors_dispatched",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD, 8u, MIX_F_PI, &rd,
                                       &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_procinfo_errors_special",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD, 15u, MIX_F_PI, &rd,
                                       &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    /* The signature is written only when the ProcInfo decodes. */
    expect_u32("mixed_rd_procinfo_errors_untouched",
               (uint32_t)(uint8_t)sig.params[0], 0x5Au);
    /* A parameter field with a register hint but size code 0. */
    expect_u32("mixed_rd_procinfo_errors_nosize",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD, 2u | (0x1Cu << 11),
                                       MIX_F_PI, &rd, &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A fifth parameter would need bits beyond bit 31. */
    expect_u32("mixed_rd_procinfo_errors_five",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD,
                                       2u | (3u << 4) | MIX_PI_PARAM(1, 3) |
                                           MIX_PI_PARAM(2, 3) |
                                           MIX_PI_PARAM(3, 3) |
                                           MIX_PI_PARAM(4, 3) | (1u << 31),
                                       MIX_F_PI, &rd, &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_procinfo_errors_null_rd",
               (uint32_t)qrx_mix_rd_procinfo_sig(0, &sig),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_rd_procinfo_errors_null_sig",
               (uint32_t)qrx_mix_rd_procinfo_sig(&rd, 0),
               (uint32_t)QRX_MIX_ERR_ARG);
    /* Invoking through a descriptor without a PPC record. */
    mix_rd_header(MIX_RD_PIBAD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PIBAD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K, 2u,
                     0x2220u, 0u);
    expect_u32("mixed_rd_procinfo_errors_no_ppc",
               (uint32_t)qrx_mix_rd_invoke_procinfo(&mix, MIX_RD_PIBAD, &in,
                                                    &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A rejected ProcInfo stops before guest code runs. */
    pc = ppc.cpu.pc;
    expect_u32("mixed_rd_procinfo_errors_no_run",
               (uint32_t)mix_pi_decode(&mix, MIX_RD_PIBAD, 0u, MIX_F_PI, &rd,
                                       &sig),
               (uint32_t)QRX_MIX_ERR_RD);
    in.gpr[0] = 1u;
    in.gpr[1] = 1u;
    in.gpr[2] = 1u;
    in.gpr_count = 3u;
    expect_u32("mixed_rd_procinfo_errors_call",
               (uint32_t)qrx_mix_rd_invoke_procinfo(&mix, MIX_RD_PIBAD, &in,
                                                    &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_procinfo_errors_pc", ppc.cpu.pc, pc);
    expect_u32("mixed_rd_procinfo_errors_result", out.gpr[0], 0u);
    report("mixed_rd_procinfo_errors", before);
}

/* Descriptor imports without their own signature: the placement comes from
 * the record's ProcInfo.  The fat descriptor's 68k record carries an opaque
 * ProcInfo, so the PPC record has to win; the 68k-only descriptor falls back
 * to its own record; an explicit signature still overrides ProcInfo; and a
 * ProcInfo rewritten in guest memory changes the next call. */
static void test_mixed_proc_procinfo(void)
{
    qrx_mix_t mix;
    qrx_mix_t mix2;
    mix_ctx_t ctx;
    mix_ctx_t ctx2;
    qrx_mix_sig_t one;
    qrx_mix_result_t out;
    unsigned before = failures;
    uint32_t three = MIX_PI_THREE;
    uint32_t two = MIX_PI_TWO;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_proc_pi_parse",
               (uint32_t)qrx_mix_sig_parse(&one, "i", 'i'),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_PROC_PI, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     0x0000A1B2u, 0x1120u, 0u);
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, three, MIX_F_PI, 0u);
    expect_u32("mixed_proc_pi_register",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_PI,
                                               mix_import_proc, &ctx, QRX_MIX_PROCINFO),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_calls", ctx.calls, 1u);
    expect_u32("mixed_proc_pi_entry", ctx.proc, 0x1120u);
    expect_u32("mixed_proc_pi_gcount", ctx.gcount, 3u);
    expect_u32("mixed_proc_pi_sum", ctx.sum, 6u);
    expect_u32("mixed_proc_pi_stack_count", ctx.stack_count, 0u);
    expect_u32("mixed_proc_pi_result", out.gpr[0], 7u);

    /* 68k record only: its own ProcInfo describes the one argument. */
    mix_rd_header(MIX_PROC_PI68, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_PROC_PI68 + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     2u | (3u << 4) | MIX_PI_PARAM(1, 3), 0x1130u, 0u);
    expect_u32("mixed_proc_pi68_register",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_PI68,
                                               mix_import_proc, &ctx, QRX_MIX_PROCINFO),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi68_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI68, QRX_MIX_RAW_SIG, 0, &out,
                                      2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi68_gcount", ctx.gcount, 1u);
    expect_u32("mixed_proc_pi68_sum", ctx.sum, 41u);
    expect_u32("mixed_proc_pi68_result", out.gpr[0], 41u);

    /* An explicit signature is copied at registration and wins. */
    /* mix_setup() reloads the guest image; the descriptors written above must
     * survive, so only the bridge state of the second instance is reset. */
    memset(&ctx2, 0, sizeof(ctx2));
    ctx2.mix = &mix2;
    expect_u32("mixed_proc_pi_explicit_init",
               (uint32_t)qrx_mix_init(&mix2, &ppc, MIX_GLUE, MIX_SP_TOP,
                                      MIX_SP_LIMIT),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_explicit_register",
               (uint32_t)qrx_mix_register_proc(&mix2, MIX_PROC_PI,
                                               mix_import_proc, &ctx2, &one),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_explicit_call",
               (uint32_t)qrx_mix_call(&mix2, MIX_F_PROC_PI, QRX_MIX_RAW_SIG, 0, &out,
                                      2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_explicit_gcount", ctx2.gcount, 1u);
    expect_u32("mixed_proc_pi_explicit_sum", ctx2.sum, 1u);
    expect_u32("mixed_proc_pi_explicit_result", out.gpr[0], 2u);

    /* Rewriting the PPC record's ProcInfo changes the next call: the derived
     * placement is not cached from the registration. */
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, two, MIX_F_PI, 0u);
    expect_u32("mixed_proc_pi_live_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_live_calls", ctx.calls, 3u);
    expect_u32("mixed_proc_pi_live_gcount", ctx.gcount, 2u);
    expect_u32("mixed_proc_pi_live_sum", ctx.sum, 3u);
    expect_u32("mixed_proc_pi_live_result", out.gpr[0], 4u);
    report("mixed_proc_procinfo", before);
}

static void test_mixed_proc_procinfo_errors(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_result_t out;
    unsigned before = failures;
    uint32_t three = MIX_PI_THREE;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_proc_pi_errors_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    /* A missing signature is no longer a licence for ProcInfo, and the
     * rejected registration takes no slot. */
    expect_u32("mixed_proc_pi_errors_raw_sig",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_PI,
                                               mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_proc_pi_errors_raw_sig_unregistered",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI, QRX_MIX_RAW_SIG, 0,
                                      &out, 1000u),
               (uint32_t)QRX_MIX_ERR_IMPORT);
    /* The PPC record's ProcInfo is stack-based: there is no placement to
     * derive, and the 68k record's opaque value must not be used instead. */
    mix_rd_header(MIX_PROC_PI, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     0x0000A1B2u, 0x1120u, 0u);
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, 0x00000001u, MIX_F_PI, 0u);
    expect_u32("mixed_proc_pi_errors_fat_stack",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_PI,
                                               mix_import_proc, &ctx, QRX_MIX_PROCINFO),
               (uint32_t)QRX_MIX_ERR_RD);
    /* 68k-only descriptor whose ProcInfo says nothing about placement. */
    mix_rd_header(MIX_PROC_PI68, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_PROC_PI68 + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     0x0000A1B2u, 0x1130u, 0u);
    expect_u32("mixed_proc_pi_errors_68k_opaque",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_PI68,
                                               mix_import_proc, &ctx, QRX_MIX_PROCINFO),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A rejected descriptor took no slot: its import address stays unknown. */
    expect_u32("mixed_proc_pi_errors_unregistered",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI68, QRX_MIX_RAW_SIG, 0, &out,
                                      1000u),
               (uint32_t)QRX_MIX_ERR_IMPORT);

    /* Register a decodable descriptor, then break its ProcInfo in guest
     * memory: the next call fails before the callback runs. */
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, three, MIX_F_PI, 0u);
    expect_u32("mixed_proc_pi_errors_register",
               (uint32_t)qrx_mix_register_proc(&mix, MIX_PROC_PI,
                                               mix_import_proc, &ctx, QRX_MIX_PROCINFO),
               (uint32_t)QRX_MIX_OK);
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, 0u, MIX_F_PI, 0u);
    expect_u32("mixed_proc_pi_errors_live",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI, QRX_MIX_RAW_SIG, 0, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_proc_pi_errors_no_callback", ctx.calls, 0u);
    expect_u32("mixed_proc_pi_errors_no_result", out.gpr[0], 0u);
    /* Restoring the record serves the next call again. */
    mix_rd_record_pi(MIX_PROC_PI + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, three, MIX_F_PI, 0u);
    expect_u32("mixed_proc_pi_errors_restored",
               (uint32_t)qrx_mix_call(&mix, MIX_F_PROC_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_proc_pi_errors_restored_calls", ctx.calls, 1u);
    expect_u32("mixed_proc_pi_errors_restored_result", out.gpr[0], 7u);
    report("mixed_proc_procinfo_errors", before);
}

/* qrx_mix_rd_invoke() without a signature takes the ProcInfo path: the same
 * placement as qrx_mix_rd_invoke_procinfo(), an explicit signature still wins,
 * and a void result keeps the callee's r3. */
static void test_mixed_rd_invoke_procinfo(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_mix_sig_t one;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_invoke_pi_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_invoke_pi_parse",
               (uint32_t)qrx_mix_sig_parse(&one, "i", 'i'),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD_PI, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     MIX_PI_THREE, MIX_F_PI, 0u);
    in.gpr[0] = 20u;
    in.gpr[1] = 22u;
    in.gpr[2] = 24u;
    in.gpr_count = 3u;
    expect_u32("mixed_rd_invoke_pi_status",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_PI, 0, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_invoke_pi_result", out.gpr[0], 66u);
    /* The explicit ProcInfo entry point is the same call. */
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_invoke_pi_alias",
               (uint32_t)qrx_mix_rd_invoke_procinfo(&mix, MIX_RD_PI, &in, &out,
                                                    1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_invoke_pi_alias_result", out.gpr[0], 66u);
    /* An explicit signature decides the placement, not the record: one placed
     * argument reaches a callee whose ProcInfo names three, which the derived
     * path would have rejected for its argument count. */
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     MIX_PI_THREE, MIX_F_RDNEST, 0u);
    memset(&out, 0, sizeof(out));
    in.gpr_count = 1u;
    in.gpr[0] = 20u;
    expect_u32("mixed_rd_invoke_pi_explicit",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_PI, &one, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_invoke_pi_explicit_stored", get32(0x2210u), 20u);
    /* A 0-byte result has no value: the mask drops r3, as everywhere else. */
    mix_rd_header(MIX_RD_PI0, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI0 + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC, 2u,
                     MIX_F_PI0, 0u);
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_invoke_pi_void",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_PI0, 0, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_invoke_pi_void_result", out.gpr[0], 0u);
    report("mixed_rd_invoke_procinfo", before);
}

static void test_mixed_rd_invoke_procinfo_errors(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_ppc_state_t saved;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_invoke_pi_errors_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    in.gpr_count = 3u;
    /* A stack-based ProcInfo describes no placement; no guest code may run. */
    mix_rd_header(MIX_RD_PIBAD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PIBAD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     0x00000001u, MIX_F_PI, 0u);
    in.gpr[0] = 20u;
    in.gpr[1] = 22u;
    in.gpr[2] = 24u;
    saved = ppc.cpu;
    expect_u32("mixed_rd_invoke_pi_errors_stack",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_PIBAD, 0, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_invoke_pi_errors_state",
               (uint32_t)(memcmp(&saved, &ppc.cpu, sizeof(saved)) == 0), 1u);
    /* A 68k-only descriptor carries no PPC entry to run. */
    mix_rd_header(MIX_RD_68K, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_68K + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     MIX_PI_THREE, 0x2220u, 0u);
    expect_u32("mixed_rd_invoke_pi_errors_68k",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_68K, 0, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A fifth parameter cannot be placed. */
    mix_rd_header(MIX_RD_PI, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     2u | (3u << 4) | (1u << 31), MIX_F_PI, 0u);
    expect_u32("mixed_rd_invoke_pi_errors_fifth",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_PI, 0, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A repaired record serves the next call again. */
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     MIX_PI_THREE, MIX_F_PI, 0u);
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_invoke_pi_errors_restored",
               (uint32_t)qrx_mix_rd_invoke(&mix, MIX_RD_PI, 0, &in, &out,
                                           1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_invoke_pi_errors_restored_result", out.gpr[0], 66u);
    report("mixed_rd_invoke_procinfo_errors", before);
}

/* qrx_mix_rd_call() derives the placement from ProcInfo when no signature is
 * given, keeps the ISA gate ahead of any guest access, and still lets an
 * explicit signature decide. */
static void test_mixed_rd_call_procinfo(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_mix_sig_t one;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_call_pi_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_call_pi_parse",
               (uint32_t)qrx_mix_sig_parse(&one, "i", 'i'),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD_PI, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     MIX_PI_THREE, MIX_F_PI, 0u);
    in.gpr[0] = 20u;
    in.gpr[1] = 22u;
    in.gpr[2] = 24u;
    in.gpr_count = 3u;
    expect_u32("mixed_rd_call_pi_status",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_PI, QRX_MIX_RD_ISA_PPC,
                                         0, &in, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_call_pi_result", out.gpr[0], 66u);
    /* An explicit signature decides the placement, not the record: one placed
     * argument reaches a callee whose ProcInfo names three. */
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     MIX_PI_THREE, MIX_F_RDNEST, 0u);
    memset(&out, 0, sizeof(out));
    in.gpr_count = 1u;
    expect_u32("mixed_rd_call_pi_explicit",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_PI, QRX_MIX_RD_ISA_PPC,
                                         &one, &in, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_call_pi_explicit_stored", get32(0x2210u), 20u);
    report("mixed_rd_call_procinfo", before);
}

static void test_mixed_rd_call_procinfo_errors(void)
{
    qrx_mix_t mix;
    qrx_mix_args_t in;
    qrx_mix_result_t out;
    qrx_ppc_state_t saved;
    unsigned before = failures;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_call_pi_errors_init", (uint32_t)mix_setup(&mix, 0),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_RD_PI, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     0x00000001u, MIX_F_PI, 0u);
    in.gpr_count = 3u;
    saved = ppc.cpu;
    expect_u32("mixed_rd_call_pi_errors_stack",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_PI, QRX_MIX_RD_ISA_PPC,
                                         0, &in, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_call_pi_errors_state",
               (uint32_t)(memcmp(&saved, &ppc.cpu, sizeof(saved)) == 0), 1u);
    /* The ISA gate comes first: 68k code is never entered or even resolved. */
    expect_u32("mixed_rd_call_pi_errors_isa",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_PI, QRX_MIX_RD_ISA_68K,
                                         0, &in, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_rd_call_pi_errors_isa_state",
               (uint32_t)(memcmp(&saved, &ppc.cpu, sizeof(saved)) == 0), 1u);
    /* A 68k-only descriptor carries no PPC entry to run. */
    mix_rd_header(MIX_RD_68K, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_RD_68K + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     MIX_PI_THREE, 0x2220u, 0u);
    expect_u32("mixed_rd_call_pi_errors_68k",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_68K, QRX_MIX_RD_ISA_PPC,
                                         0, &in, &out, 1000u),
               (uint32_t)QRX_MIX_ERR_RD);
    /* A repaired record serves the next call again. */
    mix_rd_record_pi(MIX_RD_PI + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     MIX_PI_THREE, MIX_F_PI, 0u);
    in.gpr[0] = 20u;
    in.gpr[1] = 22u;
    in.gpr[2] = 24u;
    memset(&out, 0, sizeof(out));
    expect_u32("mixed_rd_call_pi_errors_restored",
               (uint32_t)qrx_mix_rd_call(&mix, MIX_RD_PI, QRX_MIX_RD_ISA_PPC,
                                         0, &in, &out, 1000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_rd_call_pi_errors_restored_result", out.gpr[0], 66u);
    report("mixed_rd_call_procinfo_errors", before);
}

/* qrx_mix_register() takes the descriptor as well: without a signature its
 * ProcInfo decides the placement - the PPC record's when the descriptor has
 * one, the 68k record's otherwise - while an explicit signature decides it
 * instead and no descriptor may accompany it.  A ProcInfo rewritten in guest
 * memory changes the next call, an undecodable one fails the registration, and
 * the raw register path has to be requested with QRX_MIX_RAW; leaving both out
 * is an error that keeps the previous binding. */
static void test_mixed_import_procinfo(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_result_t out;
    qrx_mix_sig_t two_sig;
    unsigned before = failures;
    uint32_t three = MIX_PI_THREE;
    uint32_t two = MIX_PI_TWO;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_index_pi_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_IDX_RD, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record_pi(MIX_IDX_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     0x0000A1B2u, 0x1140u, 0u);
    mix_rd_record_pi(MIX_IDX_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, three, MIX_F_RDNEST, 0u);
    expect_u32("mixed_index_pi_register",
               (uint32_t)qrx_mix_register(&mix, 5u, MIX_IDX_RD,
                                                   mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi_calls", ctx.calls, 1u);
    expect_u32("mixed_index_pi_proc", ctx.proc, 0u);
    expect_u32("mixed_index_pi_gcount", ctx.gcount, 3u);
    expect_u32("mixed_index_pi_sum", ctx.sum, 6u);
    expect_u32("mixed_index_pi_result", out.gpr[0], 7u);

    /* The binding is re-read on every call, not cached. */
    mix_rd_record_pi(MIX_IDX_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, two, MIX_F_RDNEST, 0u);
    expect_u32("mixed_index_pi_live_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi_live_gcount", ctx.gcount, 2u);
    expect_u32("mixed_index_pi_live_sum", ctx.sum, 3u);
    expect_u32("mixed_index_pi_live_result", out.gpr[0], 4u);
    expect_u32("mixed_index_pi_live_calls", ctx.calls, 2u);

    /* A ProcInfo that describes no placement stops the call before the
     * callback and cannot be registered again. */
    mix_rd_record_pi(MIX_IDX_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, 0x00000001u, MIX_F_RDNEST, 0u);
    expect_u32("mixed_index_pi_bad_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_ERR_RD);
    expect_u32("mixed_index_pi_bad_calls", ctx.calls, 2u);
    expect_u32("mixed_index_pi_bad_register",
               (uint32_t)qrx_mix_register(&mix, 5u, MIX_IDX_RD,
                                                   mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_ERR_RD);

    /* A 68k-only descriptor falls back to its own record. */
    mix_rd_header(MIX_IDX_RD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_IDX_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     2u | (3u << 4) | MIX_PI_PARAM(1, 3), 0x1140u, 0u);
    expect_u32("mixed_index_pi68_register",
               (uint32_t)qrx_mix_register(&mix, 5u, MIX_IDX_RD,
                                                   mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi68_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi68_gcount", ctx.gcount, 1u);
    expect_u32("mixed_index_pi68_sum", ctx.sum, 1u);
    expect_u32("mixed_index_pi68_result", out.gpr[0], 2u);

    /* An explicit signature decides the placement on its own, and registering
     * it drops the descriptor binding. */
    {
        expect_u32("mixed_index_pi_explicit_parse",
                   (uint32_t)qrx_mix_sig_parse(&two_sig, "ii", 'i'),
                   (uint32_t)QRX_MIX_OK);
        expect_u32("mixed_index_pi_explicit",
                   (uint32_t)qrx_mix_register(&mix, 5u, 0u,
                                               mix_import_proc, &ctx,
                                               &two_sig),
                   (uint32_t)QRX_MIX_OK);
        expect_u32("mixed_index_pi_explicit_call",
                   (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out,
                                          2000u),
                   (uint32_t)QRX_MIX_OK);
        expect_u32("mixed_index_pi_explicit_gcount", ctx.gcount, 2u);
        expect_u32("mixed_index_pi_explicit_sum", ctx.sum, 3u);
        expect_u32("mixed_index_pi_explicit_result", out.gpr[0], 4u);
    }

    /* An out-of-range index and a descriptor below the index range are
     * arguments, not a descriptor that could be resolved. */
    expect_u32("mixed_index_pi_args",
               (uint32_t)qrx_mix_register(&mix, QRX_MIX_IMPORTS,
                                                   MIX_IDX_RD,
                                                   mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_index_pi_low",
               (uint32_t)qrx_mix_register(&mix, 5u, 8u,
                                                   mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_ERR_ARG);

    /* Without a signature and without a descriptor the registration is an
     * argument error now, and it leaves the binding from the case above in
     * place, including the import slot it already holds. */
    expect_u32("mixed_index_pi_implicit",
               (uint32_t)qrx_mix_register(&mix, 5u, 0u, mix_import_proc, &ctx,
                                          0),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_index_pi_implicit_slot",
               mix.imports[5] == mix_import_proc ? 1u : 0u, 1u);
    expect_u32("mixed_index_pi_implicit_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi_implicit_gcount", ctx.gcount, 2u);
    expect_u32("mixed_index_pi_implicit_result", out.gpr[0], 4u);

    /* A descriptor next to a signature is not ignored, and raw registers plus
     * a signature is contradictory. */
    expect_u32("mixed_index_pi_sig_desc",
               (uint32_t)qrx_mix_register(&mix, 5u, MIX_IDX_RD,
                                           mix_import_proc, &ctx, &two_sig),
               (uint32_t)QRX_MIX_ERR_ARG);
    expect_u32("mixed_index_pi_raw_sig",
               (uint32_t)qrx_mix_register(&mix, 5u, QRX_MIX_RAW,
                                           mix_import_proc, &ctx, &two_sig),
               (uint32_t)QRX_MIX_ERR_ARG);

    /* QRX_MIX_RAW is the only way to the raw register path; it drops the
     * binding and the callback sees every register again. */
    expect_u32("mixed_index_pi_plain",
               (uint32_t)qrx_mix_register(&mix, 5u, QRX_MIX_RAW,
                                           mix_import_proc, &ctx, 0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi_plain_call",
               (uint32_t)qrx_mix_call(&mix, MIX_F_IDX_PI, QRX_MIX_RAW_SIG, 0, &out, 2000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_index_pi_plain_gcount", ctx.gcount, QRX_MIX_ARGS);
    report("mixed_import_procinfo", before);
}

/* A real glue case end to end: PPC calls an import, the 68k callback holds
 * only a UniversalProcPtr and re-enters PPC through ProcInfo without a
 * signature, then the outer PPC code continues.  The callee stores its two
 * register arguments, so their placement is checked in guest memory, and the
 * nested frame must hang below the caller's r1 with the caller as back chain. */
static void test_mixed_glue_procinfo(void)
{
    qrx_mix_t mix;
    mix_ctx_t ctx;
    qrx_mix_result_t out;
    uint32_t outer_r1 = MIX_GLUE_SP - 32u;
    unsigned before = failures;

    memset(&out, 0, sizeof(out));
    expect_u32("mixed_glue_init", (uint32_t)mix_setup(&mix, &ctx),
               (uint32_t)QRX_MIX_OK);
    mix_rd_header(MIX_GLUE_RD, QRX_MIX_RD_VERSION, 1u);
    mix_rd_record_pi(MIX_GLUE_RD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_68K,
                     0x0000A1B2u, 0x2220u, 0u);
    mix_rd_record_pi(MIX_GLUE_RD + QRX_MIX_RD_HEADER + QRX_MIX_RD_RECORD,
                     QRX_MIX_RD_ISA_PPC, MIX_PI_TWO, MIX_F_RDNEST, 0u);
    mix_rd_header(MIX_GLUE_RDBAD, QRX_MIX_RD_VERSION, 0u);
    mix_rd_record_pi(MIX_GLUE_RDBAD + QRX_MIX_RD_HEADER, QRX_MIX_RD_ISA_PPC,
                     0x00000001u, MIX_F_RDNEST, 0u);
    expect_u32("mixed_glue_register",
               (uint32_t)qrx_mix_register(&mix, 4u, QRX_MIX_RAW, mix_import_glue_pi, &ctx,
                                          0),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_glue_status",
               (uint32_t)qrx_mix_call(&mix, MIX_F_RDCALL, QRX_MIX_RAW_SIG, 0, &out, 4000u),
               (uint32_t)QRX_MIX_OK);
    expect_u32("mixed_glue_calls", ctx.calls, 1u);
    expect_u32("mixed_glue_ppc_entry", ctx.rd_ok, 1u);
    expect_u32("mixed_glue_arg0", ctx.rd_marker[0], 11u);
    expect_u32("mixed_glue_arg1", ctx.rd_marker[1], 13u);
    expect_u32("mixed_glue_back_chain", ctx.rd_back, outer_r1);
    expect_u32("mixed_glue_sp", ctx.rd_sp_seen,
               (outer_r1 - QRX_MIX_LINKAGE) & ~0xFu);
    expect_u32("mixed_glue_rejected", ctx.rd_rd, 1u);
    expect_u32("mixed_glue_restore", ctx.rd_restore, 1u);
    expect_u32("mixed_glue_depth", (uint32_t)mix.depth, 0u);
    expect_u32("mixed_glue_result", out.gpr[0], 26u);
    report("mixed_glue_procinfo", before);
}

int main(void)
{
    unsigned emit_failures;

    printf("QRXPPC reference interpreter\n");
    emit_failures = qrx_ppc_jit_emit_selftest();
    printf("JIT emit selftest: %s\n", emit_failures ? "FAIL" : "PASS");
    failures += emit_failures;
#ifdef __AROS__
    jit_code = (uint8_t *)AllocVec(JIT_CODE_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (jit_code) {
        qrx_ppc_jit_init(&jit, jit_code, JIT_CODE_SIZE);
    } else {
        printf("FAIL jit code allocation\n");
        failures++;
    }
#endif
    test_arithmetic();
    test_loop();
    test_compare_branch();
    test_compare_cr();
    test_load_store();
    test_mul_div();
    test_rotate();
    test_logical_cr();
    test_link();
    test_jit_cr0();
#ifdef __AROS__
    if (jit_code)
        test_jit_fuzz();
    if (jit_code)
        test_jit_fault_fuzz();
    if (jit_code)
        test_jit_widths();
    if (jit_code)
        test_jit_widths_limit();
    if (jit_code)
        test_jit_branch();
    if (jit_code)
        test_jit_chain();
    if (jit_code)
        test_jit_chain_blr();
    if (jit_code)
        test_jit_chain_bcctr();
    if (jit_code)
        test_jit_chain_indirect_not_taken();
        test_jit_chain_cache_2way();
        test_jit_chain_cache_thrash();
        test_jit_smc_midblock();
#endif
    test_jit_faults();
    test_pef_loader();
    test_pef_reloc();
    test_mixed_call();
    test_mixed_stack();
    test_mixed_fpu();
    test_mixed_import();
    test_mixed_nested();
    test_mixed_errors();
    test_mixed_sig_call();
    test_mixed_sig_fp();
    test_mixed_sig_import();
    test_mixed_sig_stack();
    test_mixed_sig_errors();
    test_mixed_rd();
    test_mixed_rd_errors();
    test_mixed_proc_import();
    test_mixed_proc_errors();
    test_mixed_proc_procinfo();
    test_mixed_proc_procinfo_errors();
    test_mixed_rd_invoke_procinfo();
    test_mixed_rd_invoke_procinfo_errors();
    test_mixed_rd_call_procinfo();
    test_mixed_rd_call_procinfo_errors();
    test_mixed_import_procinfo();
    test_mixed_glue_procinfo();
    test_mixed_rd_nested();
    test_mixed_rd_nested_errors();
    test_mixed_rd_procinfo();
    test_mixed_rd_procinfo_errors();
    test_fpu_double();
    test_fpu_single();
    test_fpu_compare();
    test_fpu_mul_div();
    test_fpu_round_modes();
    test_fpu_frsp();
    test_fpu_fctiw();
    test_fpu_special();
    test_fpu_inexact();
    test_fpu_fpscr_transfer();
    test_fpu_compare_nan();
#ifdef __AROS__
    if (jit_code)
        test_jit_cache();
    if (jit_code) {
        FreeVec(jit_code);
        jit_code = NULL;
    }

#endif
    printf("QRXPPC: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
