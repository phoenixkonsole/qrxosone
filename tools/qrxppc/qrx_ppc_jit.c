/*
 * QRXPPC P1.4 Tier 1 - PPC32 basic blocks to m68k.
 *
 * The generated function has the C signature void f(qrx_ppc_t *).  It keeps a0
 * pointing at the guest context; D0/D1 are scratch and D2 holds a pending CR0
 * nibble.  D2 is saved/restored by the generated prologue/epilogue.  Guest GPRs
 * are read and written at their real offsets in qrx_ppc_t.  Integer loads/stores
 * call checked helpers so faults and alignment errors keep interpreter
 * semantics.
 *
 * Encodings were checked against GNU as 2.47 for m68k; the fixed snippet in
 * qrx_ppc_jit_emit_selftest() is byte-identical to the assembler output.
 */
#include "qrx_ppc_jit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __AROS__
#include <exec/execbase.h>
#include <proto/exec.h>
#endif

#define GPR_OFF(n) ((uint32_t)(offsetof(qrx_ppc_t, cpu.gpr) + (n) * 4u))
#define CPU_OFF(field) ((uint32_t)offsetof(qrx_ppc_t, cpu.field))
#define STATUS_OFF ((uint32_t)offsetof(qrx_ppc_t, status))
#define PC_OFF CPU_OFF(pc)
#define LR_OFF CPU_OFF(lr)
#define MSR_OFF CPU_OFF(msr)
#define SRR0_OFF CPU_OFF(srr0)
#define SRR1_OFF CPU_OFF(srr1)

#define JTX_PATCH_MAX 24
#define XER_SO 0x80000000u

/* m68k opcodes; all multi-byte values are big endian. */
#define M68_MOVE_L_D0_IMM 0x203Cu
#define M68_EOR_L_D1_D0 0xB380u
#define M68_MOVE_L_A0_D0 0x2028u
#define M68_MOVE_L_D0_A0 0x2140u
#define M68_MOVE_L_A0_D1 0x2228u
#define M68_MOVE_L_8SP_A0 0x206Fu
#define M68_MOVE_L_D0_D2 0x2400u
#define M68_MOVEQ_D2_0 0x7400u
#define M68_MOVE_L_IMM_A0 0x217Cu
#define M68_ADD_L_A0_D0 0xD0A8u
#define M68_SUB_L_A0_D0 0x90A8u
#define M68_AND_L_A0_D0 0xC0A8u
#define M68_OR_L_A0_D0 0x80A8u
#define M68_ADDI_L_D0 0x0680u
#define M68_ANDI_L_D0 0x0280u
#define M68_ORI_L_D0 0x0080u
#define M68_EORI_L_D0 0x0A80u
#define M68_TST_L_A0 0x4AA8u
#define M68_BNE_W 0x6600u
#define M68_PUSH_D0 0x2F00u
#define M68_PUSH_D1 0x2F01u
#define M68_PUSH_D2 0x2F02u
#define M68_PUSH_A0 0x2F08u
#define M68_POP_D2 0x241Fu
#define M68_CLR_L_D2 0x4282u
#define M68_ADDQ_L_8_SP 0x508Fu
#define M68_ADDQ_L_4_SP 0x588Fu
#define M68_JSR_ABS 0x4EB9u
#define M68_RTS 0x4E75u

typedef struct {
    uint8_t *p;
    uint32_t cap;
    uint32_t len;
    int overflow;
} jqe;

typedef struct {
    jqe e;
    uint32_t patch[JTX_PATCH_MAX];
    unsigned npatch;
    int bad;
    int cr0_dirty;
} jtx;

static void je_init(jqe *e, void *buf, uint32_t cap)
{
    e->p = (uint8_t *)buf;
    e->cap = cap;
    e->len = 0;
    e->overflow = 0;
}

static void je_w16(jqe *e, uint32_t v)
{
    if (e->overflow || e->len > e->cap || e->cap - e->len < 2u) {
        e->overflow = 1;
        return;
    }
    e->p[e->len++] = (uint8_t)(v >> 8);
    e->p[e->len++] = (uint8_t)v;
}

static void je_w32(jqe *e, uint32_t v)
{
    je_w16(e, v >> 16);
    je_w16(e, v & 0xFFFFu);
}

static void je_movea_ctx(jqe *e)
{
    /* D2 was pushed on entry, so the C argument is at 8(%sp). */
    je_w16(e, M68_MOVE_L_8SP_A0);
    je_w16(e, 8);
}

static void je_load_d0(jqe *e, uint32_t disp)
{
    je_w16(e, M68_MOVE_L_A0_D0);
    je_w16(e, disp);
}

static void je_load_d1(jqe *e, uint32_t disp)
{
    je_w16(e, M68_MOVE_L_A0_D1);
    je_w16(e, disp);
}

static void je_store_d0(jqe *e, uint32_t disp)
{
    je_w16(e, M68_MOVE_L_D0_A0);
    je_w16(e, disp);
}

static void je_store_imm(jqe *e, uint32_t disp, uint32_t value)
{
    je_w16(e, M68_MOVE_L_IMM_A0);
    je_w32(e, value);
    je_w16(e, disp);
}

static void je_load_imm_d0(jqe *e, uint32_t value)
{
    je_w16(e, M68_MOVE_L_D0_IMM);
    je_w32(e, value);
}

static void je_addi_d0(jqe *e, uint32_t value)
{
    je_w16(e, M68_ADDI_L_D0);
    je_w32(e, value);
}

static void je_andi_d0(jqe *e, uint32_t value)
{
    je_w16(e, M68_ANDI_L_D0);
    je_w32(e, value);
}

static void je_ori_d0(jqe *e, uint32_t value)
{
    je_w16(e, M68_ORI_L_D0);
    je_w32(e, value);
}

static void je_eori_d0(jqe *e, uint32_t value)
{
    je_w16(e, M68_EORI_L_D0);
    je_w32(e, value);
}

static void je_alu_a0_d0(jqe *e, uint32_t opcode, uint32_t disp)
{
    je_w16(e, opcode);
    je_w16(e, disp);
}

static void je_tst_at(jqe *e, uint32_t disp)
{
    je_w16(e, M68_TST_L_A0);
    je_w16(e, disp);
}

static void je_tst_status(jqe *e)
{
    je_tst_at(e, STATUS_OFF);
}

static uint32_t je_bne_placeholder(jqe *e)
{
    uint32_t off = e->len;

    je_w16(e, M68_BNE_W);
    je_w16(e, 0);
    return off;
}

static void je_push_d0(jqe *e)
{
    je_w16(e, M68_PUSH_D0);
}

static void je_push_d1(jqe *e)
{
    je_w16(e, M68_PUSH_D1);
}

static void je_push_d2(jqe *e)
{
    je_w16(e, M68_PUSH_D2);
}

static void je_push_a0(jqe *e)
{
    je_w16(e, M68_PUSH_A0);
}

static void je_pop_d2(jqe *e)
{
    je_w16(e, M68_POP_D2);
}

static void je_moveq_d2_0(jqe *e)
{
    je_w16(e, M68_MOVEQ_D2_0);
}

static void je_move_d0_d2(jqe *e)
{
    je_w16(e, M68_MOVE_L_D0_D2);
}

static void je_clr_d2(jqe *e)
{
    je_w16(e, M68_CLR_L_D2);
}

static void je_cleanup2(jqe *e)
{
    je_w16(e, M68_ADDQ_L_8_SP);
}

static void je_cleanup3(jqe *e)
{
    je_w16(e, M68_ADDQ_L_8_SP);
    je_w16(e, M68_ADDQ_L_4_SP);
}

static void je_jsr_abs(jqe *e, uintptr_t addr)
{
    je_w16(e, M68_JSR_ABS);
    je_w32(e, (uint32_t)addr);
}

static void je_rts(jqe *e)
{
    je_w16(e, M68_RTS);
}

static int jit_read32(qrx_ppc_t *ppc, uint32_t addr, uint32_t *out)
{
    uint32_t v = 0;
    unsigned i;

    if (addr > ppc->mem.size || ppc->mem.size - addr < 4u)
        return 0;
    for (i = 0; i < 4; i++)
        v = (v << 8) | ppc->mem.data[addr + i];
    *out = v;
    return 1;
}

static int jit_read8(qrx_ppc_t *ppc, uint32_t addr, uint8_t *out)
{
    if (addr >= ppc->mem.size)
        return 0;
    *out = ppc->mem.data[addr];
    return 1;
}

uint32_t qrx_ppc_jit_load8(qrx_ppc_t *ppc, uint32_t ea)
{
    if (ea >= ppc->mem.size) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return 0;
    }
    return ppc->mem.data[ea];
}

static int jit_load16(qrx_ppc_t *ppc, uint32_t ea, uint32_t *out)
{
    if ((ea & 1u) || ea >= ppc->mem.size ||
        ppc->mem.size - ea < 2u) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return 0;
    }
    *out = ((uint32_t)ppc->mem.data[ea] << 8) |
           ppc->mem.data[ea + 1u];
    return 1;
}

uint32_t qrx_ppc_jit_load16(qrx_ppc_t *ppc, uint32_t ea)
{
    uint32_t v;

    if (!jit_load16(ppc, ea, &v))
        return 0;
    return v;
}

uint32_t qrx_ppc_jit_load16s(qrx_ppc_t *ppc, uint32_t ea)
{
    uint32_t v;

    if (!jit_load16(ppc, ea, &v))
        return 0;
    return (uint32_t)(int32_t)(int16_t)v;
}

uint32_t qrx_ppc_jit_load32(qrx_ppc_t *ppc, uint32_t ea)
{
    uint32_t v = 0;
    unsigned i;

    if ((ea & 3u) || ea > ppc->mem.size ||
        ppc->mem.size - ea < 4u) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return 0;
    }
    for (i = 0; i < 4; i++)
        v = (v << 8) | ppc->mem.data[ea + i];
    return v;
}

void qrx_ppc_jit_store8(qrx_ppc_t *ppc, uint32_t ea, uint32_t value)
{
    if (ea >= ppc->mem.size) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return;
    }
    ppc->mem.data[ea] = (uint8_t)value;
}

void qrx_ppc_jit_store16(qrx_ppc_t *ppc, uint32_t ea, uint32_t value)
{
    if ((ea & 1u) || ea >= ppc->mem.size ||
        ppc->mem.size - ea < 2u) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return;
    }
    ppc->mem.data[ea] = (uint8_t)(value >> 8);
    ppc->mem.data[ea + 1u] = (uint8_t)value;
}

void qrx_ppc_jit_store32(qrx_ppc_t *ppc, uint32_t ea, uint32_t value)
{
    unsigned i;

    if ((ea & 3u) || ea > ppc->mem.size ||
        ppc->mem.size - ea < 4u) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return;
    }
    for (i = 0; i < 4; i++)
        ppc->mem.data[ea + 3u - i] = (uint8_t)(value >> (i * 8));
}

uint32_t qrx_ppc_jit_cr0_value(qrx_ppc_t *ppc, uint32_t value)
{
    int32_t v = (int32_t)value;
    uint32_t cr = 0;

    if (v < 0)
        cr |= 8;
    else if (v > 0)
        cr |= 4;
    else
        cr |= 2;
    if (ppc->cpu.xer & XER_SO)
        cr |= 1;
    return cr;
}

void qrx_ppc_jit_commit_cr0(qrx_ppc_t *ppc, uint32_t cr0)
{
    ppc->cpu.cr = (ppc->cpu.cr & 0x0FFFFFFFu) |
                  ((cr0 & 0x0Fu) << 28);
}

static void jtx_check_status(jtx *x)
{
    if (x->npatch >= JTX_PATCH_MAX) {
        x->bad = 1;
        return;
    }
    je_tst_status(&x->e);
    x->patch[x->npatch++] = je_bne_placeholder(&x->e);
}

/* CR0 is kept in D2 and only written to cpu.cr when it becomes visible to the
 * interpreter (block boundary) or before a helper that may fault. */
static void jtx_compute_cr0(jtx *x)
{
    je_push_d0(&x->e);      /* result value */
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, (uintptr_t)qrx_ppc_jit_cr0_value);
    je_cleanup2(&x->e);
    je_movea_ctx(&x->e);
    je_move_d0_d2(&x->e);
    x->cr0_dirty = 1;
}

static void jtx_commit_cr0(jtx *x)
{
    if (!x->cr0_dirty)
        return;
    je_push_d2(&x->e);
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, (uintptr_t)qrx_ppc_jit_commit_cr0);
    je_cleanup2(&x->e);
    je_movea_ctx(&x->e);
    je_clr_d2(&x->e);
    x->cr0_dirty = 0;
}

static void jtx_store_result(jtx *x, unsigned rt, int rc)
{
    je_store_d0(&x->e, GPR_OFF(rt));
    if (rc)
        jtx_compute_cr0(x);
}

static void jtx_addr(jtx *x, unsigned ra, int32_t simm)
{
    if (ra == 0) {
        je_load_imm_d0(&x->e, (uint32_t)simm);
    } else {
        je_load_d0(&x->e, GPR_OFF(ra));
        if (simm != 0)
            je_addi_d0(&x->e, (uint32_t)simm);
    }
}

static void jtx_load(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                     uint32_t pc, uintptr_t helper)
{
    jtx_commit_cr0(x);
    jtx_addr(x, ra, simm);
    je_store_imm(&x->e, PC_OFF, pc);
    je_push_d0(&x->e);
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, helper);
    je_cleanup2(&x->e);
    je_movea_ctx(&x->e);
    jtx_check_status(x);
    je_store_d0(&x->e, GPR_OFF(rt));
}

static void jtx_load8(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                      uint32_t pc)
{
    jtx_load(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_load8);
}

static void jtx_load16(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                       uint32_t pc)
{
    jtx_load(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_load16);
}

static void jtx_load16s(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                        uint32_t pc)
{
    jtx_load(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_load16s);
}

static void jtx_load32(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                       uint32_t pc)
{
    jtx_load(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_load32);
}

static void jtx_store(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                      uint32_t pc, uintptr_t helper)
{
    jtx_commit_cr0(x);
    jtx_addr(x, ra, simm);
    je_load_d1(&x->e, GPR_OFF(rt));
    je_store_imm(&x->e, PC_OFF, pc);
    je_push_d1(&x->e);
    je_push_d0(&x->e);
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, helper);
    je_cleanup3(&x->e);
    je_movea_ctx(&x->e);
    jtx_check_status(x);
}

static void jtx_store8(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                       uint32_t pc)
{
    jtx_store(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_store8);
}

static void jtx_store16(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                        uint32_t pc)
{
    jtx_store(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_store16);
}

static void jtx_store32(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                        uint32_t pc)
{
    jtx_store(x, rt, ra, simm, pc, (uintptr_t)qrx_ppc_jit_store32);
}

/* Returns 0 = unsupported, 1 = normal instruction, 2 = terminal block end. */
static int jtx_insn(jtx *x, uint32_t pc, uint32_t insn)
{
    unsigned op = (insn >> 26) & 0x3Fu;
    unsigned rt = (insn >> 21) & 0x1Fu;
    unsigned ra = (insn >> 16) & 0x1Fu;
    unsigned rb = (insn >> 11) & 0x1Fu;
    int32_t simm = (int32_t)(int16_t)(insn & 0xFFFFu);

    switch (op) {
    case 14: /* addi */
    case 15: { /* addis */
        uint32_t imm = (op == 14) ? (uint32_t)simm
                                  : ((insn & 0xFFFFu) << 16);
        if (ra == 0) {
            je_store_imm(&x->e, GPR_OFF(rt), imm);
        } else {
            je_load_d0(&x->e, GPR_OFF(ra));
            je_addi_d0(&x->e, imm);
            je_store_d0(&x->e, GPR_OFF(rt));
        }
        return 1;
    }
    case 17: /* sc */
        jtx_commit_cr0(x);
        je_store_imm(&x->e, PC_OFF, pc + 4u);
        je_store_imm(&x->e, SRR0_OFF, pc);
        je_load_d0(&x->e, MSR_OFF);
        je_store_d0(&x->e, SRR1_OFF);
        je_store_imm(&x->e, STATUS_OFF, (uint32_t)QRX_PPC_TRAP);
        je_pop_d2(&x->e);
        je_rts(&x->e);
        return 2;
    case 18: { /* b / bl / ba / bla */
        int32_t disp = (int32_t)(insn & 0x03FFFFFCu);
        uint32_t target;

        if (disp & 0x02000000)
            disp |= (int32_t)0xFC000000u;
        target = (insn & 2u) ? (uint32_t)disp
                             : (uint32_t)((int32_t)pc + disp);
        jtx_commit_cr0(x);
        if (insn & 1u)
            je_store_imm(&x->e, LR_OFF, pc + 4u);
        je_store_imm(&x->e, PC_OFF, target);
        je_store_imm(&x->e, STATUS_OFF, (uint32_t)QRX_PPC_OK);
        je_pop_d2(&x->e);
        je_rts(&x->e);
        return 2;
    }
    case 24: /* ori */
    case 25: /* oris */
    case 26: /* xori */
    case 27: { /* xoris */
        uint32_t imm = (op == 24 || op == 26)
                           ? (insn & 0xFFFFu)
                           : ((insn & 0xFFFFu) << 16);
        je_load_d0(&x->e, GPR_OFF(rt));
        if (op == 24 || op == 25)
            je_ori_d0(&x->e, imm);
        else
            je_eori_d0(&x->e, imm);
        je_store_d0(&x->e, GPR_OFF(ra));
        return 1;
    }
    case 31: { /* X-form integer */
        unsigned xo = (insn >> 1) & 0x3FFu;
        int rc = (int)(insn & 1u);

        if ((insn >> 10) & 1u) /* OE: XER overflow is not in Tier 1 */
            return 0;
        switch (xo) {
        case 266: /* add */
            je_load_d0(&x->e, GPR_OFF(ra));
            je_alu_a0_d0(&x->e, M68_ADD_L_A0_D0, GPR_OFF(rb));
            jtx_store_result(x, rt, rc);
            return 1;
        case 40: /* subf: rD = rB - rA */
            je_load_d0(&x->e, GPR_OFF(rb));
            je_alu_a0_d0(&x->e, M68_SUB_L_A0_D0, GPR_OFF(ra));
            jtx_store_result(x, rt, rc);
            return 1;
        case 28: /* and */
            je_load_d0(&x->e, GPR_OFF(ra));
            je_alu_a0_d0(&x->e, M68_AND_L_A0_D0, GPR_OFF(rb));
            jtx_store_result(x, rt, rc);
            return 1;
        case 444: /* or */
            je_load_d0(&x->e, GPR_OFF(ra));
            je_alu_a0_d0(&x->e, M68_OR_L_A0_D0, GPR_OFF(rb));
            jtx_store_result(x, rt, rc);
            return 1;
        case 316: /* xor */
            je_load_d0(&x->e, GPR_OFF(ra));
            je_load_d1(&x->e, GPR_OFF(rb));
            je_w16(&x->e, M68_EOR_L_D1_D0);
            jtx_store_result(x, rt, rc);
            return 1;
        default:
            return 0;
        }
    }
    case 28: /* andi. */
    case 29: { /* andis. */
        uint32_t imm = (op == 28) ? (insn & 0xFFFFu)
                                  : ((insn & 0xFFFFu) << 16);
        je_load_d0(&x->e, GPR_OFF(rt));
        je_andi_d0(&x->e, imm);
        jtx_store_result(x, ra, 1);
        return 1;
    }
    case 32: /* lwz */
        jtx_load32(x, rt, ra, simm, pc);
        return 1;
    case 34: /* lbz */
        jtx_load8(x, rt, ra, simm, pc);
        return 1;
    case 36: /* stw */
        jtx_store32(x, rt, ra, simm, pc);
        return 1;
    case 38: /* stb */
        jtx_store8(x, rt, ra, simm, pc);
        return 1;
    case 40: /* lhz */
        jtx_load16(x, rt, ra, simm, pc);
        return 1;
    case 42: /* lha */
        jtx_load16s(x, rt, ra, simm, pc);
        return 1;
    case 44: /* sth */
        jtx_store16(x, rt, ra, simm, pc);
        return 1;
    default:
        return 0;
    }
}

static void jit_add_insns(qrx_ppc_t *ppc, const qrx_ppc_jit_block_t *block)
{
    uint32_t executed = block->insns;

    /* A checked memory access may abort in the middle of a block.  The
     * interpreter counts that faulting instruction, but not the suffix. */
    if (ppc->status == QRX_PPC_FAULT) {
        uint32_t delta = ppc->cpu.pc - block->pc;

        if ((delta & 3u) == 0 && delta / 4u < block->insns)
            executed = delta / 4u + 1u;
    }
    ppc->insns += executed;
}

static int source_matches(qrx_ppc_t *ppc, const qrx_ppc_jit_block_t *block)
{
    uint32_t i;

    for (i = 0; i < block->source_len; i++) {
        uint8_t v;

        if (!jit_read8(ppc, block->pc + i, &v))
            return 0;
        if (v != block->source[i])
            return 0;
    }
    return 1;
}

static void source_bytes(uint8_t *dst, uint32_t insn)
{
    dst[0] = (uint8_t)(insn >> 24);
    dst[1] = (uint8_t)(insn >> 16);
    dst[2] = (uint8_t)(insn >> 8);
    dst[3] = (uint8_t)insn;
}

static int jit_translate(qrx_ppc_jit_t *jit, qrx_ppc_t *ppc, uint32_t start_pc,
                         uint32_t budget, qrx_ppc_jit_block_t *out)
{
    jtx x;
    uint8_t source[QRX_PPC_JIT_MAX_SOURCE];
    uint32_t pc = start_pc;
    uint32_t count = 0;
    uint32_t source_len = 0;
    uint32_t abort_off;
    uint32_t off;
    unsigned i;
    int terminal = 0;

    if (jit->code_used > jit->code_size)
        return 0;
    je_init(&x.e, jit->code + jit->code_used, jit->code_size - jit->code_used);
    je_push_d2(&x.e);
    je_moveq_d2_0(&x.e);
    je_movea_ctx(&x.e); /* first C argument: qrx_ppc_t * at 8(%sp) */
    x.npatch = 0;
    x.bad = 0;
    x.cr0_dirty = 0;

    while (count < budget && source_len + 4u <= QRX_PPC_JIT_MAX_SOURCE) {
        uint32_t insn;
        uint8_t raw[4];
        int kind;

        if (!jit_read32(ppc, pc, &insn))
            break;
        kind = jtx_insn(&x, pc, insn);
        if (kind == 0)
            break;
        source_bytes(raw, insn);
        memcpy(source + source_len, raw, sizeof(raw));
        source_len += 4u;
        count++;
        if (kind == 2) {
            terminal = 1;
            break;
        }
        pc += 4u;
    }
    if (count == 0)
        return 0;

    if (!terminal) {
        jtx_commit_cr0(&x);
        je_store_imm(&x.e, PC_OFF, pc);
        je_store_imm(&x.e, STATUS_OFF, (uint32_t)QRX_PPC_OK);
        je_pop_d2(&x.e);
        je_rts(&x.e);
    }
    abort_off = x.e.len;
    je_pop_d2(&x.e);
    je_rts(&x.e);

    if (x.e.overflow || x.bad)
        return 0;

    for (i = 0; i < x.npatch; i++) {
        int32_t disp = (int32_t)abort_off - (int32_t)(x.patch[i] + 2u);

        if (disp < -32768 || disp > 32767) {
            x.bad = 1;
            break;
        }
        x.e.p[x.patch[i] + 2u] = (uint8_t)((uint32_t)disp >> 8);
        x.e.p[x.patch[i] + 3u] = (uint8_t)disp;
    }

    if (x.e.overflow || x.bad)
        return 0;
    if (jit->code_used + x.e.len > jit->code_size)
        return 0;

    off = jit->code_used;
    out->pc = start_pc;
    out->insns = count;
    out->code_offset = off;
    out->code_len = x.e.len;
    out->source_len = source_len;
    memcpy(out->source, source, source_len);
    jit->code_used += x.e.len;
    jit->nblocks++;
    qrx_ppc_jit_flush(jit);
    return 1;
}

void qrx_ppc_jit_init(qrx_ppc_jit_t *jit, void *code, uint32_t code_size)
{
    uintptr_t base = (uintptr_t)code;
    uintptr_t aligned = (base + 3u) & ~(uintptr_t)3u;

    memset(jit, 0, sizeof(*jit));
    jit->code = (uint8_t *)code;
    jit->code_size = code_size;
    jit->code_align = (uint32_t)(aligned - base);
    if (jit->code_align > code_size)
        jit->code_align = code_size;
    qrx_ppc_jit_reset(jit);
}

void qrx_ppc_jit_reset(qrx_ppc_jit_t *jit)
{
    jit->nblocks = 0;
    jit->code_used = jit->code_align;
    jit->hits = 0;
    jit->misses = 0;
    jit->recompiles = 0;
}

int qrx_ppc_jit_run_block(qrx_ppc_jit_t *jit, qrx_ppc_t *ppc,
                          uint32_t budget)
{
    uint32_t i;
    uint32_t stale = 0;
    qrx_ppc_jit_block_t *block;

    if (!jit || !ppc || budget == 0 || ppc->cpu.pc == 0)
        return 0;

    for (i = 0; i < jit->nblocks; i++) {
        block = &jit->blocks[i];
        if (block->pc != ppc->cpu.pc)
            continue;
        if (block->insns > budget)
            return 0;
        if (!source_matches(ppc, block)) {
            stale = 1;
            continue;
        }
        jit->hits++;
        ppc->status = QRX_PPC_OK;
        ppc->fault_addr = 0;
        ((void (*)(qrx_ppc_t *))(void *)(jit->code + block->code_offset))(ppc);
        jit_add_insns(ppc, block);
        return 1;
    }

    if (jit->nblocks >= QRX_PPC_JIT_MAX_BLOCKS ||
        jit->code_size - jit->code_used < 256u)
        qrx_ppc_jit_reset(jit);
    if (jit->nblocks >= QRX_PPC_JIT_MAX_BLOCKS ||
        jit->code_used + 16u >= jit->code_size)
        return 0;

    block = &jit->blocks[jit->nblocks];
    if (!jit_translate(jit, ppc, ppc->cpu.pc, budget, block))
        return 0;
    jit->misses++;
    if (stale)
        jit->recompiles++;

    ppc->status = QRX_PPC_OK;
    ppc->fault_addr = 0;
    ((void (*)(qrx_ppc_t *))(void *)(jit->code + block->code_offset))(ppc);
    jit_add_insns(ppc, block);
    return 1;
}

qrx_ppc_status_t qrx_ppc_run_jit(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit,
                                 uint32_t max_insns)
{
    if (!jit)
        return qrx_ppc_run(ppc, max_insns);

    ppc->insns = 0;
    while (ppc->insns < max_insns) {
        if (ppc->cpu.pc == 0) {
            ppc->status = QRX_PPC_HALT;
            return QRX_PPC_HALT;
        }
        if (!qrx_ppc_jit_run_block(jit, ppc, max_insns - ppc->insns)) {
            qrx_ppc_status_t status = qrx_ppc_step(ppc);

            ppc->insns++;
            if (status != QRX_PPC_OK)
                return status;
        } else if (ppc->status != QRX_PPC_OK) {
            return ppc->status;
        }
    }
    ppc->status = QRX_PPC_OK;
    return QRX_PPC_OK;
}

void qrx_ppc_jit_flush(qrx_ppc_jit_t *jit)
{
#ifdef __AROS__
    if (jit->code_used != 0) {
        CacheClearE(jit->code, jit->code_used,
                    CACRF_ClearI | CACRF_ClearD);
        CacheClearU();
    }
#else
    (void)jit;
#endif
}

/* GNU as 2.47 output for the same sequence. */
static const uint8_t emit_golden[] = {
    0x2F, 0x02, 0x74, 0x00, 0x20, 0x6F, 0x00, 0x08,
    0x20, 0x28, 0x00, 0x0C,
    0x21, 0x7C, 0x12, 0x34, 0x56, 0x78, 0x00, 0x0C,
    0xD0, 0xA8, 0x00, 0x0C,
    0x90, 0xA8, 0x00, 0x0C,
    0xC0, 0xA8, 0x00, 0x0C,
    0x80, 0xA8, 0x00, 0x0C,
    0x22, 0x28, 0x00, 0x0C,
    0xB3, 0x80,
    0x06, 0x80, 0x00, 0x00, 0x12, 0x34,
    0x00, 0x80, 0x00, 0x00, 0x12, 0x34,
    0x0A, 0x80, 0x00, 0x00, 0x12, 0x34,
    0x02, 0x80, 0x00, 0x00, 0x12, 0x34,
    0x24, 0x00,
    0x4A, 0xA8, 0x01, 0xA4,
    0x66, 0x00, 0x00, 0x0C,
    0x2F, 0x00,
    0x2F, 0x08,
    0x50, 0x8F,
    0x21, 0x40, 0x00, 0x0C,
    0x42, 0x82,
    0x24, 0x1F,
    0x4E, 0x75
};

unsigned qrx_ppc_jit_emit_selftest(void)
{
    uint8_t buf[128];
    jqe e;

    je_init(&e, buf, sizeof(buf));
    je_push_d2(&e);
    je_moveq_d2_0(&e);
    je_movea_ctx(&e);
    je_load_d0(&e, 0x0C);
    je_store_imm(&e, 0x0C, 0x12345678u);
    je_alu_a0_d0(&e, M68_ADD_L_A0_D0, 0x0C);
    je_alu_a0_d0(&e, M68_SUB_L_A0_D0, 0x0C);
    je_alu_a0_d0(&e, M68_AND_L_A0_D0, 0x0C);
    je_alu_a0_d0(&e, M68_OR_L_A0_D0, 0x0C);
    je_load_d1(&e, 0x0C);
    je_w16(&e, M68_EOR_L_D1_D0);
    je_addi_d0(&e, 0x1234u);
    je_ori_d0(&e, 0x1234u);
    je_eori_d0(&e, 0x1234u);
    je_andi_d0(&e, 0x1234u);
    je_move_d0_d2(&e);
    je_tst_at(&e, 0x01A4);
    je_w16(&e, M68_BNE_W);
    je_w16(&e, 0x000C);
    je_push_d0(&e);
    je_push_a0(&e);
    je_cleanup2(&e);
    je_store_d0(&e, 0x0C);
    je_clr_d2(&e);
    je_pop_d2(&e);
    je_rts(&e);

    if (e.overflow || e.len != sizeof(emit_golden))
        return 1;
    return memcmp(buf, emit_golden, sizeof(emit_golden)) ? 1u : 0u;
}
