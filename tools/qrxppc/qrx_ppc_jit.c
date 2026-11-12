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
#define JTX_LINK_MAX 2
#define JTX_EXIT_MAX 4
#define JTX_SMC_MAX 16
#define XER_SO 0x80000000u

/* m68k opcodes; all multi-byte values are big endian. */
#define M68_MOVE_L_D0_IMM 0x203Cu
#define M68_MOVE_L_D1_IMM 0x223Cu
#define M68_EOR_L_D1_D0 0xB380u
#define M68_ADD_L_D1_D0 0xD081u
#define M68_CMP_L_D1_D0 0xB081u
#define M68_MOVE_L_A0_D0 0x2028u
#define M68_MOVE_L_A1_D0 0x2029u
#define M68_MOVE_L_A1_D1 0x2229u
#define M68_MOVE_L_D0_A0 0x2140u
#define M68_MOVE_L_D0_A1 0x2340u
#define M68_MOVE_L_A0_D1 0x2228u
#define M68_MOVE_L_D0_D1 0x2200u
#define M68_MOVE_L_8SP_A0 0x206Fu
#define M68_MOVE_L_8SP_A1 0x226Fu
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
#define M68_BRA_B 0x6000u
#define M68_BEQ_B 0x6700u
#define M68_BGT_B 0x6E00u
#define M68_PUSH_D0 0x2F00u
#define M68_PUSH_D1 0x2F01u
#define M68_PUSH_D2 0x2F02u
#define M68_PUSH_A0 0x2F08u
#define M68_PUSH_A1 0x2F09u
#define M68_TST_L_D0 0x4A80u
#define M68_TST_L_D1 0x4A81u
#define M68_POP_D2 0x241Fu
#define M68_CLR_L_D2 0x4282u
#define M68_ADDQ_L_8_SP 0x508Fu
#define M68_ADDQ_L_4_SP 0x588Fu
#define M68_JSR_ABS 0x4EB9u
#define M68_JMP_ABS 0x4EF9u
#define M68_JMP_IND_A0 0x4ED0u
#define M68_MOVE_L_D0_A0REG 0x2040u
#define M68_RTS 0x4E75u

typedef struct {
    uint8_t *p;
    uint32_t cap;
    uint32_t len;
    int overflow;
} jqe;

typedef struct {
    uint32_t pc;
    uint32_t patch;
} jtx_link;

typedef struct {
    jqe e;
    uint32_t patch[JTX_PATCH_MAX];
    unsigned npatch;
    uint32_t exit_patch[JTX_EXIT_MAX];
    unsigned nexits;
    uint32_t smc_exit[JTX_SMC_MAX];
    unsigned nsmc;
    jtx_link links[JTX_LINK_MAX];
    unsigned nlinks;
    uint32_t start_pc;
    uint32_t block_insns;
    int bad;
    int cr0_dirty;
    int has_store;
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
    /* The context argument is at 8(%sp), both for the normal prologue
     * (return address + saved D2) and for a chain entry (caller's saved D2). */
    je_w16(e, M68_MOVE_L_8SP_A0);
    je_w16(e, 8);
}

static void je_movea_jit(jqe *e)
{
    /* Second C argument, used for chain counters and budget checks. */
    je_w16(e, M68_MOVE_L_8SP_A1);
    je_w16(e, 12);
}

static void je_load_d0(jqe *e, uint32_t disp)
{
    je_w16(e, M68_MOVE_L_A0_D0);
    je_w16(e, disp);
}

static void je_load_d0_a1(jqe *e, uint32_t disp)
{
    je_w16(e, M68_MOVE_L_A1_D0);
    je_w16(e, disp);
}

static void je_store_d0_a1(jqe *e, uint32_t disp)
{
    je_w16(e, M68_MOVE_L_D0_A1);
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

static void je_load_imm_d1(jqe *e, uint32_t value)
{
    je_w16(e, M68_MOVE_L_D1_IMM);
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

static void je_push_a1(jqe *e)
{
    je_w16(e, M68_PUSH_A1);
}

static uint32_t je_branch_placeholder(jqe *e, uint32_t opcode)
{
    uint32_t off = e->len;

    je_w16(e, opcode);
    je_w16(e, 0);
    return off;
}

static void je_patch_disp16(jqe *e, uint32_t opcode_off, uint32_t target_off)
{
    int32_t disp = (int32_t)target_off - (int32_t)(opcode_off + 2u);

    if (e->overflow || opcode_off + 4u > e->len ||
        disp < -32768 || disp > 32767) {
        e->overflow = 1;
        return;
    }
    e->p[opcode_off + 2u] = (uint8_t)((uint32_t)disp >> 8);
    e->p[opcode_off + 3u] = (uint8_t)disp;
}

static void je_pop_d2(jqe *e)
{
    je_w16(e, M68_POP_D2);
}

static void je_moveq_d2_0(jqe *e)
{
    je_w16(e, M68_MOVEQ_D2_0);
}

static void je_moveq_d1(jqe *e, uint32_t v)
{
    je_w16(e, 0x7200u | (v & 0xFFu));
}

static void je_move_d0_d1(jqe *e)
{
    je_w16(e, M68_MOVE_L_D0_D1);
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

static void je_cleanup4(jqe *e)
{
    je_w16(e, M68_ADDQ_L_8_SP);
    je_w16(e, M68_ADDQ_L_8_SP);
}

static void je_cleanup6(jqe *e)
{
    je_w16(e, M68_ADDQ_L_8_SP);
    je_w16(e, M68_ADDQ_L_8_SP);
    je_w16(e, M68_ADDQ_L_8_SP);
}

static void je_jsr_abs(jqe *e, uintptr_t addr)
{
    je_w16(e, M68_JSR_ABS);
    je_w32(e, (uint32_t)addr);
}

static uint32_t je_jmp_placeholder(jqe *e)
{
    uint32_t off = e->len;

    je_w16(e, M68_JMP_ABS);
    je_w32(e, 0);
    return off + 2u;
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

/* A store that writes into the source bytes of the block it is running in
 * leaves that block's own translation stale: the instructions after the store
 * were fetched before the write.  The helper reports the overlap (jit->smc_lo
 * .. jit->smc_hi is the source range of the running block) and the emitted
 * code leaves the block instead of executing a stale successor. */
static int jit_smc_overlap(const qrx_ppc_jit_t *jit, uint32_t ea, uint32_t width)
{
    return jit && ea < jit->smc_hi && ea + width > jit->smc_lo;
}

uint32_t qrx_ppc_jit_store8(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit, uint32_t ea,
                            uint32_t value)
{
    int smc;

    if (ea >= ppc->mem.size) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return 0;
    }
    smc = jit_smc_overlap(jit, ea, 1u);
    ppc->mem.data[ea] = (uint8_t)value;
    return (uint32_t)smc;
}

uint32_t qrx_ppc_jit_store16(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit, uint32_t ea,
                             uint32_t value)
{
    int smc;

    if ((ea & 1u) || ea >= ppc->mem.size ||
        ppc->mem.size - ea < 2u) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return 0;
    }
    smc = jit_smc_overlap(jit, ea, 2u);
    ppc->mem.data[ea] = (uint8_t)(value >> 8);
    ppc->mem.data[ea + 1u] = (uint8_t)value;
    return (uint32_t)smc;
}

uint32_t qrx_ppc_jit_store32(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit, uint32_t ea,
                             uint32_t value)
{
    unsigned i;
    int smc;

    if ((ea & 3u) || ea > ppc->mem.size ||
        ppc->mem.size - ea < 4u) {
        ppc->fault_addr = ea;
        ppc->status = QRX_PPC_FAULT;
        return 0;
    }
    smc = jit_smc_overlap(jit, ea, 4u);
    for (i = 0; i < 4; i++)
        ppc->mem.data[ea + 3u - i] = (uint8_t)(value >> (i * 8));
    return (uint32_t)smc;
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

static void jit_cr_set(qrx_ppc_t *ppc, unsigned field, uint32_t value)
{
    uint32_t shift = 28u - (field & 7u) * 4u;
    uint32_t mask = 0xFu << shift;

    ppc->cpu.cr = (ppc->cpu.cr & ~mask) | ((value & 0xFu) << shift);
}

void qrx_ppc_jit_compare_s(qrx_ppc_t *ppc, uint32_t field_l, uint32_t a,
                           uint32_t b)
{
    unsigned field = field_l & 7u;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    uint32_t cr = 0;

    if (field_l & 8u) {
        sa = (int16_t)sa;
        sb = (int16_t)sb;
    }
    if (sa < sb)
        cr |= 8u;
    else if (sa > sb)
        cr |= 4u;
    else
        cr |= 2u;
    if (ppc->cpu.xer & XER_SO)
        cr |= 1u;
    jit_cr_set(ppc, field, cr);
}

void qrx_ppc_jit_compare_u(qrx_ppc_t *ppc, uint32_t field_l, uint32_t a,
                           uint32_t b)
{
    unsigned field = field_l & 7u;
    uint32_t cr = 0;

    if (field_l & 8u) {
        a &= 0xFFFFu;
        b &= 0xFFFFu;
    }
    if (a < b)
        cr |= 8u;
    else if (a > b)
        cr |= 4u;
    else
        cr |= 2u;
    if (ppc->cpu.xer & XER_SO)
        cr |= 1u;
    jit_cr_set(ppc, field, cr);
}

static int jit_branch_condition(qrx_ppc_t *ppc, uint32_t bo, uint32_t bi)
{
    int ctr_ok = 1;
    int cr_ok = 1;

    if (bo == 20u)
        return 1;
    if (bo & 0x10u) {
        ppc->cpu.ctr--;
        ctr_ok = (bo & 0x02u) ? (ppc->cpu.ctr == 0)
                              : (ppc->cpu.ctr != 0);
    }
    if (bo & 0x04u) {
        int bit = (int)((ppc->cpu.cr >> (31u - (bi & 31u))) & 1u);
        int want = (bo & 0x08u) ? 1 : 0;

        cr_ok = (bit == want);
    }
    return ctr_ok && cr_ok;
}

uint32_t qrx_ppc_jit_branch(qrx_ppc_t *ppc, uint32_t bo, uint32_t bi,
                            uint32_t target, uint32_t fallthrough,
                            uint32_t link)
{
    uint32_t taken = (uint32_t)(jit_branch_condition(ppc, bo, bi) != 0);

    if (taken) {
        ppc->cpu.pc = target;
        if (link)
            ppc->cpu.lr = fallthrough;
    } else {
        ppc->cpu.pc = fallthrough;
    }
    return taken;
}

/* bclr/bcctr: same condition logic as bc, but the target is read from LR or
 * CTR.  Like the interpreter's do_branch(), the target is used unchanged and
 * LR receives the sequential PC when the linked form is taken. */
uint32_t qrx_ppc_jit_branch_indirect(qrx_ppc_t *ppc, uint32_t bo, uint32_t bi,
                                     uint32_t from_ctr, uint32_t fallthrough,
                                     uint32_t link)
{
    uint32_t target = from_ctr ? ppc->cpu.ctr : ppc->cpu.lr;
    uint32_t taken = (uint32_t)(jit_branch_condition(ppc, bo, bi) != 0);

    if (taken) {
        ppc->cpu.pc = target;
        if (link)
            ppc->cpu.lr = fallthrough;
    } else {
        ppc->cpu.pc = fallthrough;
    }
    return taken;
}

static void jtx_check_status(jtx *x, uint32_t executed)
{
    if (x->npatch >= JTX_PATCH_MAX) {
        x->bad = 1;
        return;
    }
    /* On a fault the abort path adds only the instructions that actually
     * ran.  D1 is scratch here and is not live across helper calls. */
    je_moveq_d1(&x->e, executed);
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
    je_movea_jit(&x->e);
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
    je_movea_jit(&x->e);
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
    je_movea_jit(&x->e);
    jtx_check_status(x, ((pc - x->start_pc) >> 2) + 1u);
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

static void jtx_add_chain(jtx *x, uint32_t insns);
static void jtx_emit_exit(jtx *x, uint32_t target_pc, int linkable);

static void jtx_store(jtx *x, unsigned rt, unsigned ra, int32_t simm,
                      uint32_t pc, uintptr_t helper)
{
    uint32_t executed = ((pc - x->start_pc) >> 2) + 1u;
    uint32_t smc_skip;

    x->has_store = 1;
    jtx_commit_cr0(x);
    jtx_addr(x, ra, simm);
    je_load_d1(&x->e, GPR_OFF(rt));
    je_store_imm(&x->e, PC_OFF, pc);
    je_push_d1(&x->e);          /* value */
    je_push_d0(&x->e);          /* effective address */
    je_push_a1(&x->e);          /* jit */
    je_push_a0(&x->e);          /* ppc */
    je_jsr_abs(&x->e, helper);
    je_cleanup4(&x->e);
    je_movea_ctx(&x->e);
    je_movea_jit(&x->e);
    jtx_check_status(x, executed);

    /* Non-zero means the store rewrote this block's own source bytes: stop
     * here so the following instructions are fetched and translated again.
     * The stub reuses the block epilogue, so it needs no exit slot. */
    je_w16(&x->e, M68_TST_L_D0);
    /* 0x6700 plus the placeholder's extension word is Bcc.W. */
    smc_skip = je_branch_placeholder(&x->e, M68_BEQ_B);
    je_store_imm(&x->e, PC_OFF, pc + 4u);
    je_store_imm(&x->e, STATUS_OFF, (uint32_t)QRX_PPC_OK);
    jtx_add_chain(x, executed);
    if (x->nsmc >= JTX_SMC_MAX) {
        x->bad = 1;
        return;
    }
    x->smc_exit[x->nsmc++] = je_jmp_placeholder(&x->e);
    je_patch_disp16(&x->e, smc_skip, x->e.len);
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

static void jtx_compare(jtx *x, unsigned field, unsigned l, unsigned ra,
                        int rb_is_reg, unsigned rb, uint32_t imm,
                        uintptr_t helper)
{
    jtx_commit_cr0(x);
    je_load_d0(&x->e, GPR_OFF(ra));
    if (rb_is_reg)
        je_load_d1(&x->e, GPR_OFF(rb));
    else
        je_load_imm_d1(&x->e, imm);
    je_push_d1(&x->e);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, ((uint32_t)field & 7u) |
                           (((uint32_t)l & 1u) << 3));
    je_push_d0(&x->e);
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, helper);
    je_cleanup4(&x->e);
    je_movea_ctx(&x->e);
    je_movea_jit(&x->e);
}

static void jtx_compare_reg(jtx *x, unsigned field, unsigned l, unsigned ra,
                            unsigned rb, int is_signed)
{
    jtx_compare(x, field, l, ra, 1, rb, 0u,
                is_signed ? (uintptr_t)qrx_ppc_jit_compare_s
                          : (uintptr_t)qrx_ppc_jit_compare_u);
}

static void jtx_compare_imm(jtx *x, unsigned field, unsigned l, unsigned ra,
                            uint32_t imm, int is_signed)
{
    jtx_compare(x, field, l, ra, 0, 0, imm,
                is_signed ? (uintptr_t)qrx_ppc_jit_compare_s
                          : (uintptr_t)qrx_ppc_jit_compare_u);
}

static void jtx_add_chain(jtx *x, uint32_t insns)
{
    je_load_d0_a1(&x->e, (uint32_t)offsetof(qrx_ppc_jit_t, chain_insns));
    je_addi_d0(&x->e, insns);
    je_store_d0_a1(&x->e, (uint32_t)offsetof(qrx_ppc_jit_t, chain_insns));
}

static void jtx_add_chain_d1(jtx *x)
{
    je_load_d0_a1(&x->e, (uint32_t)offsetof(qrx_ppc_jit_t, chain_insns));
    je_w16(&x->e, M68_ADD_L_D1_D0);
    je_store_d0_a1(&x->e, (uint32_t)offsetof(qrx_ppc_jit_t, chain_insns));
}

/* Emit a tail jump.  The normal fallback goes to the block epilogue, which
 * pops the current D2 frame.  Links point at another block's chain entry,
 * which deliberately does not push a second D2 frame. */
static void jtx_emit_exit(jtx *x, uint32_t target_pc, int linkable)
{
    uint32_t patch = je_jmp_placeholder(&x->e);

    if (linkable && target_pc != 0 && !x->has_store) {
        if (x->nlinks >= JTX_LINK_MAX) {
            x->bad = 1;
            return;
        }
        x->links[x->nlinks].pc = target_pc;
        x->links[x->nlinks].patch = patch;
        x->nlinks++;
    } else {
        if (x->nexits >= JTX_EXIT_MAX) {
            x->bad = 1;
            return;
        }
        x->exit_patch[x->nexits++] = patch;
    }
}

static void jtx_branch(jtx *x, uint32_t bo, uint32_t bi, uint32_t target,
                       uint32_t fallthrough, uint32_t link)
{
    jtx_commit_cr0(x);
    je_load_imm_d0(&x->e, link);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, fallthrough);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, target);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, bi);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, bo);
    je_push_d0(&x->e);
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, (uintptr_t)qrx_ppc_jit_branch);
    je_cleanup6(&x->e);
    je_movea_ctx(&x->e);
    je_movea_jit(&x->e);
    je_store_imm(&x->e, STATUS_OFF, (uint32_t)QRX_PPC_OK);
    je_move_d0_d1(&x->e);       /* keep the branch result out of D0 */
    jtx_add_chain(x, x->block_insns);
    je_w16(&x->e, M68_TST_L_D1); /* the chain counter changed the flags */
    je_w16(&x->e, M68_BEQ_B | 6u);
    jtx_emit_exit(x, target, 1);
    jtx_emit_exit(x, fallthrough, 1);
}

/* Indirect branch terminator.  The target is only known at run time, so the
 * taken path looks it up in the two-way set-associative target cache and tail-jumps
 * into the target's chain entry; the entry re-checks source bytes and budget.
 * A cache miss returns to the runner with PC already set, which translates the
 * target and fills the cache for the next execution. */
static void jtx_branch_indirect(jtx *x, uint32_t bo, uint32_t bi, int from_ctr,
                                uint32_t fallthrough, uint32_t link)
{
    uint32_t bne_patch;

    jtx_commit_cr0(x);
    je_load_imm_d0(&x->e, link);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, fallthrough);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, (uint32_t)(from_ctr != 0));
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, bi);
    je_push_d0(&x->e);
    je_load_imm_d0(&x->e, bo);
    je_push_d0(&x->e);
    je_push_a0(&x->e);
    je_jsr_abs(&x->e, (uintptr_t)qrx_ppc_jit_branch_indirect);
    je_cleanup6(&x->e);
    je_movea_ctx(&x->e);
    je_movea_jit(&x->e);
    je_store_imm(&x->e, STATUS_OFF, (uint32_t)QRX_PPC_OK);
    je_move_d0_d1(&x->e);       /* keep the branch result out of D0 */
    jtx_add_chain(x, x->block_insns);
    je_w16(&x->e, M68_TST_L_D1); /* the chain counter changed the flags */

    bne_patch = je_branch_placeholder(&x->e, M68_BNE_W);
    jtx_emit_exit(x, fallthrough, 1);
    je_patch_disp16(&x->e, bne_patch, x->e.len);

    if (x->has_store) {
        /* A store may have invalidated the target; let the runner recheck it. */
        jtx_emit_exit(x, 0, 0);
        return;
    }

    je_load_d0(&x->e, PC_OFF);  /* target resolved by the branch helper */
    je_push_d0(&x->e);
    je_push_a1(&x->e);
    je_jsr_abs(&x->e, (uintptr_t)qrx_ppc_jit_chain_target);
    je_cleanup2(&x->e);
    je_movea_ctx(&x->e);
    je_w16(&x->e, M68_MOVE_L_D0_A0REG); /* A0 = chain entry, or 0 */
    je_w16(&x->e, M68_TST_L_D0);
    je_w16(&x->e, M68_BEQ_B | 2u);
    je_w16(&x->e, M68_JMP_IND_A0);
    je_pop_d2(&x->e);
    je_rts(&x->e);
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
    case 10: /* cmpli */
    case 11: { /* cmpi */
        unsigned field = (insn >> 23) & 7u;
        unsigned l = (insn >> 21) & 1u;
        uint32_t imm = (op == 10) ? (insn & 0xFFFFu) : (uint32_t)simm;

        jtx_compare_imm(x, field, l, ra, imm, op == 11);
        return 1;
    }
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
    case 16: { /* bc / bcl */
        unsigned bo = (insn >> 21) & 0x1Fu;
        unsigned bi = (insn >> 16) & 0x1Fu;
        int32_t disp = (int32_t)(insn & 0x0000FFFCu);
        uint32_t target;

        if (disp & 0x00002000)
            disp |= (int32_t)0xFFFFC000u;
        target = (insn & 2u) ? (uint32_t)disp
                             : (uint32_t)((int32_t)pc + disp);
        jtx_branch(x, bo, bi, target, pc + 4u, insn & 1u);
        return 2;
    }
    case 17: /* sc */
        jtx_commit_cr0(x);
        je_store_imm(&x->e, PC_OFF, pc + 4u);
        je_store_imm(&x->e, SRR0_OFF, pc);
        je_load_d0(&x->e, MSR_OFF);
        je_store_d0(&x->e, SRR1_OFF);
        je_store_imm(&x->e, STATUS_OFF, (uint32_t)QRX_PPC_TRAP);
        jtx_add_chain(x, x->block_insns);
        jtx_emit_exit(x, 0, 0);
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
        jtx_add_chain(x, x->block_insns);
        jtx_emit_exit(x, target, 1);
        return 2;
    }
    case 19: { /* bclr / bcctr */
        unsigned bo = (insn >> 21) & 0x1Fu;
        unsigned bi = (insn >> 16) & 0x1Fu;
        unsigned xo = (insn >> 1) & 0x3FFu;

        if (xo != 16u && xo != 528u)
            return 0;
        jtx_branch_indirect(x, bo, bi, xo == 528u, pc + 4u, insn & 1u);
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
        case 0: { /* cmp */
            unsigned field = (insn >> 23) & 7u;
            unsigned l = (insn >> 21) & 1u;

            jtx_compare_reg(x, field, l, ra, rb, 1);
            return 1;
        }
        case 32: { /* cmpl */
            unsigned field = (insn >> 23) & 7u;
            unsigned l = (insn >> 21) & 1u;

            jtx_compare_reg(x, field, l, ra, rb, 0);
            return 1;
        }
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

static void je_patch_u32(jqe *e, uint32_t patch, uint32_t value)
{
    if (e->overflow || patch + 4u > e->len)
        return;
    e->p[patch + 0u] = (uint8_t)(value >> 24);
    e->p[patch + 1u] = (uint8_t)(value >> 16);
    e->p[patch + 2u] = (uint8_t)(value >> 8);
    e->p[patch + 3u] = (uint8_t)value;
}

static void jit_patch_code(qrx_ppc_jit_t *jit, uint32_t patch,
                           uint32_t target_off)
{
    uint32_t addr = (uint32_t)(uintptr_t)(jit->code + target_off);

    if (patch + 4u > jit->code_size)
        return;
    jit->code[patch + 0u] = (uint8_t)(addr >> 24);
    jit->code[patch + 1u] = (uint8_t)(addr >> 16);
    jit->code[patch + 2u] = (uint8_t)(addr >> 8);
    jit->code[patch + 3u] = (uint8_t)addr;
}

static qrx_ppc_jit_block_t *jit_find_block(qrx_ppc_jit_t *jit, uint32_t pc)
{
    uint32_t i;

    for (i = 0; i < jit->nblocks; i++) {
        if (jit->blocks[i].pc == pc)
            return &jit->blocks[i];
    }
    return NULL;
}

static void jit_link_block(qrx_ppc_jit_t *jit, qrx_ppc_jit_block_t *newblock)
{
    uint32_t i;
    unsigned j;

    /* A block is usually translated before its successor.  Backpatch all
     * earlier jumps that named this PC, then resolve this block's targets. */
    for (i = 0; i < jit->nblocks; i++) {
        qrx_ppc_jit_block_t *b = &jit->blocks[i];

        for (j = 0; j < b->nchain; j++) {
            if (b->chain_pc[j] == newblock->pc)
                jit_patch_code(jit, b->chain_patch[j],
                               newblock->chain_offset);
        }
    }
    for (j = 0; j < newblock->nchain; j++) {
        qrx_ppc_jit_block_t *target = jit_find_block(jit,
                                                      newblock->chain_pc[j]);

        if (target)
            jit_patch_code(jit, newblock->chain_patch[j],
                           target->chain_offset);
    }
}

static void jit_cache_clear(qrx_ppc_jit_t *jit)
{
    memset(jit->cache, 0, sizeof(jit->cache));
}

static uint32_t jit_cache_set(uint32_t pc)
{
    return (pc >> 2) & (QRX_PPC_JIT_CACHE_SETS - 1u);
}

/* Insert or refresh a translated block.  A matching PC is updated in place,
 * otherwise a free way is used; only a fully populated set replaces the way
 * that was not used last, so two hot targets with the same set index both stay
 * resident instead of evicting each other on every dispatch. */
static void jit_cache_insert(qrx_ppc_jit_t *jit,
                             const qrx_ppc_jit_block_t *block)
{
    qrx_ppc_jit_cache_t *set =
        &jit->cache[jit_cache_set(block->pc) * QRX_PPC_JIT_CACHE_WAYS];
    qrx_ppc_jit_cache_t *slot = NULL;
    unsigned i;

    for (i = 0; i < QRX_PPC_JIT_CACHE_WAYS; i++) {
        if (set[i].valid && set[i].pc == block->pc)
            slot = &set[i];
    }
    if (!slot) {
        for (i = 0; i < QRX_PPC_JIT_CACHE_WAYS && !slot; i++) {
            if (!set[i].valid)
                slot = &set[i];
        }
    }
    if (!slot) {
        slot = &set[0];
        for (i = 1; i < QRX_PPC_JIT_CACHE_WAYS; i++) {
            if (!set[i].recent) {
                slot = &set[i];
                break;
            }
        }
    }
    for (i = 0; i < QRX_PPC_JIT_CACHE_WAYS; i++)
        set[i].recent = (uint8_t)(&set[i] == slot);
    slot->pc = block->pc;
    slot->chain_offset = block->chain_offset;
    slot->valid = 1;
}

uintptr_t qrx_ppc_jit_chain_target(qrx_ppc_jit_t *jit, uint32_t pc)
{
    qrx_ppc_jit_cache_t *set;
    uintptr_t target = (uintptr_t)0;
    unsigned i;

    if (!jit)
        return (uintptr_t)0;
    set = &jit->cache[jit_cache_set(pc) * QRX_PPC_JIT_CACHE_WAYS];
    for (i = 0; i < QRX_PPC_JIT_CACHE_WAYS; i++) {
        if (set[i].valid && set[i].pc == pc &&
            set[i].chain_offset < jit->code_used) {
            unsigned j;

            for (j = 0; j < QRX_PPC_JIT_CACHE_WAYS; j++)
                set[j].recent = (uint8_t)(j == i);
            target = (uintptr_t)(jit->code + set[i].chain_offset);
            break;
        }
    }
    if (!target) {
        jit->cache_misses++;
        return (uintptr_t)0;
    }
    jit->cache_hits++;
    return target;
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

/* Called by a chain entry before it runs a linked block.  The source check
 * keeps SMC safe even when an earlier block in the same native run modified
 * the target; the budget check preserves the runner's max_insns contract. */
static uint32_t qrx_ppc_jit_chain_enter(qrx_ppc_t *ppc, qrx_ppc_jit_t *jit,
                                        uint32_t pc)
{
    qrx_ppc_jit_block_t *block;

    /* Every legitimate chain arrival has PC set to this block's entry; the
     * check rejects a stale cache entry that survived an invalidation. */
    if (ppc->cpu.pc != pc)
        return 0;
    block = jit_find_block(jit, pc);
    if (!block || !source_matches(ppc, block))
        return 0;
    if (jit->chain_insns > jit->chain_limit)
        return 0;
    if (block->insns > jit->chain_limit - jit->chain_insns)
        return 0;
    jit->smc_lo = block->pc;
    jit->smc_hi = block->pc + block->source_len;
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
    uint32_t epilogue_off;
    uint32_t normal_entry;
    uint32_t off;
    uint32_t beq_patch;
    uint32_t bra_patch;
    uint32_t no_run_off;
    uint32_t body_off;
    unsigned i;
    int terminal = 0;

    if (jit->code_used > jit->code_size)
        return 0;
    off = jit->code_used;
    je_init(&x.e, jit->code + off, jit->code_size - off);

    /* Chain entry: no extra D2 frame, so a tail jump does not grow the stack.
     * The helper checks the target's source and the remaining instruction
     * budget before the target body is entered. */
    je_movea_jit(&x.e);
    je_movea_ctx(&x.e);
    je_load_imm_d0(&x.e, start_pc);
    je_push_d0(&x.e);
    je_push_a1(&x.e);
    je_push_a0(&x.e);
    je_jsr_abs(&x.e, (uintptr_t)qrx_ppc_jit_chain_enter);
    je_cleanup3(&x.e);
    je_movea_ctx(&x.e);
    je_movea_jit(&x.e);
    je_w16(&x.e, M68_TST_L_D0);
    beq_patch = je_branch_placeholder(&x.e, M68_BEQ_B);
    je_moveq_d2_0(&x.e);
    je_movea_ctx(&x.e);
    bra_patch = je_branch_placeholder(&x.e, M68_BRA_B);
    no_run_off = x.e.len;
    je_pop_d2(&x.e);
    je_rts(&x.e);

    normal_entry = x.e.len;
    je_push_d2(&x.e);
    je_moveq_d2_0(&x.e);
    je_movea_ctx(&x.e); /* first C argument: qrx_ppc_t * at 8(%sp) */
    je_movea_jit(&x.e);
    body_off = x.e.len;
    je_patch_disp16(&x.e, beq_patch, no_run_off);
    je_patch_disp16(&x.e, bra_patch, body_off);
    x.npatch = 0;
    x.nexits = 0;
    x.nsmc = 0;
    x.nlinks = 0;
    x.start_pc = start_pc;
    x.block_insns = 0;
    x.bad = 0;
    x.cr0_dirty = 0;
    x.has_store = 0;

    while (count < budget && source_len + 4u <= QRX_PPC_JIT_MAX_SOURCE) {
        uint32_t insn;
        uint8_t raw[4];
        int kind;

        if (!jit_read32(ppc, pc, &insn))
            break;
        x.block_insns = count + 1u;
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
        jtx_add_chain(&x, count);
        jtx_emit_exit(&x, 0, 0);
    }

    /* Common fallback epilogue for an unlinked exit.  A linked exit jumps
     * directly into the target block's chain entry and skips this pop. */
    epilogue_off = x.e.len;
    for (i = 0; i < x.nexits; i++)
        je_patch_u32(&x.e, x.exit_patch[i],
                     (uint32_t)(uintptr_t)(jit->code + off + epilogue_off));
    for (i = 0; i < x.nsmc; i++)
        je_patch_u32(&x.e, x.smc_exit[i],
                     (uint32_t)(uintptr_t)(jit->code + off + epilogue_off));
    for (i = 0; i < x.nlinks; i++)
        je_patch_u32(&x.e, x.links[i].patch,
                     (uint32_t)(uintptr_t)(jit->code + off + epilogue_off));
    je_pop_d2(&x.e);
    je_rts(&x.e);

    /* Fault path: D1 contains the number of instructions that ran in this
     * block, so a chained prefix is added correctly before returning. */
    abort_off = x.e.len;
    jtx_add_chain_d1(&x);
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
    if (off + x.e.len > jit->code_size)
        return 0;

    out->pc = start_pc;
    out->insns = count;
    out->code_offset = off + normal_entry;
    out->chain_offset = off;
    out->code_len = x.e.len;
    out->source_len = source_len;
    out->has_store = (uint8_t)(x.has_store != 0);
    out->nchain = (uint8_t)x.nlinks;
    for (i = 0; i < x.nlinks; i++) {
        out->chain_pc[i] = x.links[i].pc;
        out->chain_patch[i] = off + x.links[i].patch;
    }
    memcpy(out->source, source, source_len);
    jit->code_used += x.e.len;
    jit->nblocks++;
    jit_link_block(jit, out);
    jit_cache_insert(jit, out);
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
    jit->chain_insns = 0;
    jit->chain_limit = 0;
    jit->cache_hits = 0;
    jit->cache_misses = 0;
    jit->smc_lo = 0;
    jit->smc_hi = 0;
    jit_cache_clear(jit);
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
        jit->smc_lo = block->pc;
        jit->smc_hi = block->pc + block->source_len;
        jit->hits++;
        jit_cache_insert(jit, block);
        ppc->status = QRX_PPC_OK;
        ppc->fault_addr = 0;
        jit->chain_insns = 0;
        jit->chain_limit = budget;
        jit->smc_lo = block->pc;
        jit->smc_hi = block->pc + block->source_len;
        ((void (*)(qrx_ppc_t *, qrx_ppc_jit_t *))
         (void *)(jit->code + block->code_offset))(ppc, jit);
        ppc->insns += jit->chain_insns;
        return 1;
    }

    /* A changed source byte invalidates every block, including links that
     * could otherwise bypass the source check.  Keep counters for tests. */
    if (stale) {
        jit->nblocks = 0;
        jit->code_used = jit->code_align;
        jit_cache_clear(jit);
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
    jit->chain_insns = 0;
    jit->chain_limit = budget;
    jit->smc_lo = block->pc;
    jit->smc_hi = block->pc + block->source_len;
    ((void (*)(qrx_ppc_t *, qrx_ppc_jit_t *))
     (void *)(jit->code + block->code_offset))(ppc, jit);
    ppc->insns += jit->chain_insns;
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
