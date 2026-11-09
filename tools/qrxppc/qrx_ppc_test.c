/*
 * QRXPPC reference test program.
 *
 * Each case places hand-encoded PPC32 instructions at 0x1000, runs the
 * interpreter until sc, and checks registers and big-endian memory.  The same
 * binary is built for the AROS 68k guest and for the host differential run.
 */
#include "qrx_ppc.h"
#include "qrx_ppc_jit.h"

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
