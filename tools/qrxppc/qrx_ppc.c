/*
 * QRXPPC - portable PPC32 interpreter core (P1.3 reference).
 *
 * The decoder is intentionally table-free so every unsupported instruction
 * fails visibly with QRX_PPC_ILLEGAL instead of silently doing nothing.
 * Memory is a flat, explicitly sized byte array; all multi-byte accesses are
 * big-endian and alignment-checked, independent of host endianness.  FPU
 * arithmetic and FPSCR state live in qrx_ppc_fpu.c.
 */
#include "qrx_ppc.h"
#include "qrx_ppc_fpu.h"

#include <string.h>

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

#define RT ((insn >> 21) & 0x1Fu)
#define RS RT
#define RA ((insn >> 16) & 0x1Fu)
#define RB ((insn >> 11) & 0x1Fu)
#define SH ((insn >> 11) & 0x1Fu)
#define MB ((insn >> 6) & 0x1Fu)
#define ME ((insn >> 1) & 0x1Fu)
#define RC (insn & 1u)
#define OE ((insn >> 10) & 1u)
#define UIMM (insn & 0xFFFFu)
#define SIMM ((int32_t)(int16_t)(insn & 0xFFFFu))

static int mem_read(qrx_ppc_t *p, uint32_t addr, unsigned bytes, uint32_t *out)
{
    uint32_t v = 0;
    unsigned i;

    if (bytes > 4 || addr > p->mem.size || bytes > p->mem.size - addr) {
        p->fault_addr = addr;
        return 0;
    }
    for (i = 0; i < bytes; i++)
        v = (v << 8) | p->mem.data[addr + i];
    *out = v;
    return 1;
}

static int mem_write(qrx_ppc_t *p, uint32_t addr, unsigned bytes, uint32_t v)
{
    unsigned i;

    if (bytes > 4 || addr > p->mem.size || bytes > p->mem.size - addr) {
        p->fault_addr = addr;
        return 0;
    }
    for (i = 0; i < bytes; i++)
        p->mem.data[addr + bytes - 1 - i] = (uint8_t)(v >> (i * 8));
    return 1;
}

static void cr_set(qrx_ppc_t *p, unsigned n, uint32_t v)
{
    uint32_t shift = 28 - n * 4;
    uint32_t mask = 0xFu << shift;

    p->cpu.cr = (p->cpu.cr & ~mask) | ((v & 0xFu) << shift);
}

static int cr_bit(uint32_t cr, unsigned bi)
{
    return (int)((cr >> (31 - bi)) & 1u);
}

static int mem_read64(qrx_ppc_t *p, uint32_t addr, uint64_t *out)
{
    uint64_t v = 0;
    unsigned i;

    if (addr > p->mem.size || 8u > p->mem.size - addr) {
        p->fault_addr = addr;
        return 0;
    }
    for (i = 0; i < 8; i++)
        v = (v << 8) | p->mem.data[addr + i];
    *out = v;
    return 1;
}

static int mem_write64(qrx_ppc_t *p, uint32_t addr, uint64_t v)
{
    unsigned i;

    if (addr > p->mem.size || 8u > p->mem.size - addr) {
        p->fault_addr = addr;
        return 0;
    }
    for (i = 0; i < 8; i++)
        p->mem.data[addr + 7 - i] = (uint8_t)(v >> (i * 8));
    return 1;
}

static void set_cr0_result(qrx_ppc_t *p, uint32_t value)
{
    int32_t v = (int32_t)value;
    uint32_t cr = 0;

    if (v < 0)
        cr |= 8;
    else if (v > 0)
        cr |= 4;
    else
        cr |= 2;
    if (p->cpu.xer & XER_SO)
        cr |= 1;
    cr_set(p, 0, cr);
}

static void set_cr_compare_s(qrx_ppc_t *p, unsigned field, int32_t a, int32_t b)
{
    uint32_t cr = 0;

    if (a < b)
        cr |= 8;
    else if (a > b)
        cr |= 4;
    else
        cr |= 2;
    if (p->cpu.xer & XER_SO)
        cr |= 1;
    cr_set(p, field, cr);
}

static void set_cr_compare_u(qrx_ppc_t *p, unsigned field, uint32_t a, uint32_t b)
{
    uint32_t cr = 0;

    if (a < b)
        cr |= 8;
    else if (a > b)
        cr |= 4;
    else
        cr |= 2;
    if (p->cpu.xer & XER_SO)
        cr |= 1;
    cr_set(p, field, cr);
}

static void set_ca(qrx_ppc_t *p, unsigned ca)
{
    p->cpu.xer = (p->cpu.xer & ~XER_CA) | (ca ? XER_CA : 0);
}

static unsigned get_ca(const qrx_ppc_t *p)
{
    return (p->cpu.xer & XER_CA) ? 1u : 0u;
}

static void set_ov(qrx_ppc_t *p, int ov)
{
    p->cpu.xer = (p->cpu.xer & ~XER_OV) | (ov ? XER_OV : 0);
}

static uint32_t add_carry(uint32_t a, uint32_t b, uint32_t cin, unsigned *ca)
{
    uint64_t s = (uint64_t)a + b + cin;

    *ca = (unsigned)((s >> 32) & 1u);
    return (uint32_t)s;
}

static uint32_t sub_carry(uint32_t a, uint32_t b, uint32_t cin, unsigned *ca)
{
    uint64_t s = (uint64_t)a + (uint32_t)~b + cin;

    *ca = (unsigned)((s >> 32) & 1u);
    return (uint32_t)s;
}

static int add_overflow(int32_t a, int32_t b, int32_t r)
{
    return ((a ^ r) & (b ^ r)) < 0;
}

static int sub_overflow(int32_t a, int32_t b, int32_t r)
{
    return ((a ^ b) & (a ^ r)) < 0;
}

static uint32_t rotl32(uint32_t v, unsigned n)
{
    n &= 31u;
    if (n == 0)
        return v;
    return (v << n) | (v >> (32 - n));
}

static uint32_t mask_mb_me(unsigned mb, unsigned me)
{
    uint32_t mask = 0;
    unsigned b;

    for (b = 0; b < 32; b++) {
        int in_range = (mb <= me) ? (b >= mb && b <= me)
                                  : (b >= mb || b <= me);
        if (in_range)
            mask |= 1u << (31 - b);
    }
    return mask;
}

static unsigned count_leading_zeroes(uint32_t v)
{
    unsigned n = 0;

    if (v == 0)
        return 32;
    while ((v & 0x80000000u) == 0) {
        v <<= 1;
        n++;
    }
    return n;
}

static uint32_t spr_get(const qrx_ppc_t *p, uint32_t spr)
{
    switch (spr) {
    case 1:   return p->cpu.xer;
    case 8:   return p->cpu.lr;
    case 9:   return p->cpu.ctr;
    case 26:  return p->cpu.srr0;
    case 27:  return p->cpu.srr1;
    case 287: return 0; /* PVR: no claim of a specific G4 stepping */
    default:  return 0;
    }
}

static void spr_set(qrx_ppc_t *p, uint32_t spr, uint32_t v)
{
    switch (spr) {
    case 1:  p->cpu.xer = v; break;
    case 8:  p->cpu.lr = v; break;
    case 9:  p->cpu.ctr = v; break;
    case 26: p->cpu.srr0 = v; break;
    case 27: p->cpu.srr1 = v; break;
    default: break;
    }
}

static double fpr_get_d(const qrx_ppc_t *p, unsigned r)
{
    double d;
    uint64_t u = p->cpu.fpr[r];

    memcpy(&d, &u, sizeof(d));
    return d;
}

static void fpr_set_d(qrx_ppc_t *p, unsigned r, double d)
{
    uint64_t u;

    memcpy(&u, &d, sizeof(u));
    p->cpu.fpr[r] = u;
}

static int trap_condition(unsigned to, uint32_t a, uint32_t b)
{
    return ((to & 0x10u) && (int32_t)a < (int32_t)b) ||
           ((to & 0x08u) && (int32_t)a > (int32_t)b) ||
           ((to & 0x04u) && a == b) ||
           ((to & 0x02u) && a < b) ||
           ((to & 0x01u) && a > b);
}

static void do_branch(qrx_ppc_t *p, uint32_t target, int link)
{
    if (link)
        p->cpu.lr = p->cpu.pc;
    p->cpu.pc = target;
}

/*
 * BO bit meanings used by the PowerPC bc/bclr/bcctr forms:
 *   0x10: decrement CTR
 *   0x08: branch if the selected CR bit is true (with 0x04)
 *   0x04: test CR
 *   0x02: after decrementing, branch if CTR == 0
 * BO=20 is the unconditional form used by blr/bctr.
 */
static int branch_condition(qrx_ppc_t *p, unsigned bo, unsigned bi)
{
    int ctr_ok = 1;
    int cr_ok = 1;

    if (bo == 20)
        return 1;

    if (bo & 0x10u) {
        p->cpu.ctr--;
        ctr_ok = (bo & 0x02u) ? (p->cpu.ctr == 0) : (p->cpu.ctr != 0);
    }
    if (bo & 0x04u) {
        int bit = cr_bit(p->cpu.cr, bi);
        int want = (bo & 0x08u) ? 1 : 0;
        cr_ok = (bit == want);
    }
    return ctr_ok && cr_ok;
}

static qrx_ppc_status_t fail_at(qrx_ppc_t *p, qrx_ppc_status_t status,
                                uint32_t pc)
{
    p->status = status;
    p->cpu.pc = pc;
    return status;
}

qrx_ppc_status_t qrx_ppc_step(qrx_ppc_t *p)
{
    uint32_t insn;
    uint32_t pc0 = p->cpu.pc;
    unsigned op;

    p->status = QRX_PPC_OK;
    p->fault_addr = 0;

    if (!mem_read(p, pc0, 4, &insn))
        return fail_at(p, QRX_PPC_FAULT, pc0);

    p->cpu.pc = pc0 + 4;
    op = insn >> 26;

    switch (op) {
    /* twi */
    case 3: {
        unsigned to = (insn >> 21) & 0x1Fu;
        if (trap_condition(to, p->cpu.gpr[RA], (uint32_t)SIMM)) {
            p->cpu.srr0 = pc0;
            p->cpu.srr1 = p->cpu.msr;
            p->status = QRX_PPC_TRAP;
            return QRX_PPC_TRAP;
        }
        break;
    }
    /* tw */
    case 4: {
        unsigned to = (insn >> 21) & 0x1Fu;
        if (trap_condition(to, p->cpu.gpr[RA], p->cpu.gpr[RB])) {
            p->cpu.srr0 = pc0;
            p->cpu.srr1 = p->cpu.msr;
            p->status = QRX_PPC_TRAP;
            return QRX_PPC_TRAP;
        }
        break;
    }
    /* mulli */
    case 7:
        p->cpu.gpr[RT] = (uint32_t)((int32_t)p->cpu.gpr[RA] * SIMM);
        break;
    /* subfic */
    case 8: {
        unsigned ca;
        uint32_t r = sub_carry((uint32_t)SIMM, p->cpu.gpr[RA], 1, &ca);
        p->cpu.gpr[RT] = r;
        set_ca(p, ca);
        break;
    }
    /* cmpli */
    case 10: {
        unsigned field = (insn >> 23) & 7u;
        unsigned l = (insn >> 21) & 1u;
        uint32_t a = p->cpu.gpr[RA];
        uint32_t b = UIMM;
        if (l) {
            a &= 0xFFFFu;
            b &= 0xFFFFu;
        }
        set_cr_compare_u(p, field, a, b);
        break;
    }
    /* cmpi */
    case 11: {
        unsigned field = (insn >> 23) & 7u;
        unsigned l = (insn >> 21) & 1u;
        int32_t a = (int32_t)p->cpu.gpr[RA];
        int32_t b = SIMM;
        if (l) {
            a = (int16_t)a;
            b = (int16_t)b;
        }
        set_cr_compare_s(p, field, a, b);
        break;
    }
    /* addic / addic. */
    case 12:
    case 13: {
        unsigned ca;
        uint32_t r = add_carry(p->cpu.gpr[RA], (uint32_t)SIMM, 0, &ca);
        p->cpu.gpr[RT] = r;
        set_ca(p, ca);
        if (op == 13)
            set_cr0_result(p, r);
        break;
    }
    /* addi */
    case 14:
        p->cpu.gpr[RT] = p->cpu.gpr[RA] + (uint32_t)SIMM;
        break;
    /* addis */
    case 15:
        p->cpu.gpr[RT] = p->cpu.gpr[RA] + ((uint32_t)SIMM << 16);
        break;
    /* bc / bcl */
    case 16: {
        unsigned bo = (insn >> 21) & 0x1Fu;
        unsigned bi = (insn >> 16) & 0x1Fu;
        int32_t disp = (int32_t)(insn & 0x0000FFFCu);
        uint32_t target;

        if (disp & 0x00002000)
            disp |= (int32_t)0xFFFFC000u;
        target = (insn & 2u) ? (uint32_t)disp
                             : (uint32_t)((int32_t)pc0 + disp);
        if (branch_condition(p, bo, bi))
            do_branch(p, target, (int)(insn & 1u));
        break;
    }
    /* sc */
    case 17:
        p->cpu.srr0 = pc0;
        p->cpu.srr1 = p->cpu.msr;
        p->status = QRX_PPC_TRAP;
        return QRX_PPC_TRAP;
    /* b / bl / ba / bla */
    case 18: {
        int32_t disp = (int32_t)(insn & 0x03FFFFFCu);
        uint32_t target;

        if (disp & 0x02000000)
            disp |= (int32_t)0xFC000000u;
        target = (insn & 2u) ? (uint32_t)disp
                             : (uint32_t)((int32_t)pc0 + disp);
        do_branch(p, target, (int)(insn & 1u));
        break;
    }
    /* bclr / bcctr */
    case 19: {
        unsigned bo = (insn >> 21) & 0x1Fu;
        unsigned bi = (insn >> 16) & 0x1Fu;
        unsigned xo = (insn >> 1) & 0x3FFu;
        uint32_t target;

        if (xo == 16)
            target = p->cpu.lr;
        else if (xo == 528)
            target = p->cpu.ctr;
        else
            return fail_at(p, QRX_PPC_ILLEGAL, pc0);
        if (branch_condition(p, bo, bi))
            do_branch(p, target, (int)(insn & 1u));
        break;
    }
    /* rlwimi */
    case 20: {
        uint32_t mask = mask_mb_me(MB, ME);
        uint32_t rotated = rotl32(p->cpu.gpr[RS], SH);
        uint32_t r = (p->cpu.gpr[RA] & ~mask) | (rotated & mask);
        p->cpu.gpr[RA] = r;
        if (RC)
            set_cr0_result(p, r);
        break;
    }
    /* rlwinm */
    case 21: {
        uint32_t mask = mask_mb_me(MB, ME);
        uint32_t r = rotl32(p->cpu.gpr[RS], SH) & mask;
        p->cpu.gpr[RA] = r;
        if (RC)
            set_cr0_result(p, r);
        break;
    }
    /* rlwnm */
    case 23: {
        uint32_t mask = mask_mb_me(MB, ME);
        uint32_t r = rotl32(p->cpu.gpr[RS], p->cpu.gpr[RB] & 31u) & mask;
        p->cpu.gpr[RA] = r;
        if (RC)
            set_cr0_result(p, r);
        break;
    }
    /* ori / oris / xori / xoris */
    case 24:
        p->cpu.gpr[RA] = p->cpu.gpr[RS] | UIMM;
        break;
    case 25:
        p->cpu.gpr[RA] = p->cpu.gpr[RS] | (UIMM << 16);
        break;
    case 26:
        p->cpu.gpr[RA] = p->cpu.gpr[RS] ^ UIMM;
        break;
    case 27:
        p->cpu.gpr[RA] = p->cpu.gpr[RS] ^ (UIMM << 16);
        break;
    /* andi. / andis. */
    case 28: {
        uint32_t r = p->cpu.gpr[RS] & UIMM;
        p->cpu.gpr[RA] = r;
        set_cr0_result(p, r);
        break;
    }
    case 29: {
        uint32_t r = p->cpu.gpr[RS] & (UIMM << 16);
        p->cpu.gpr[RA] = r;
        set_cr0_result(p, r);
        break;
    }
    /* Integer load/store */
    case 32: /* lwz */
    case 33: /* lwzu */
    case 34: /* lbz */
    case 35: /* lbzu */
    case 36: /* stw */
    case 37: /* stwu */
    case 38: /* stb */
    case 39: /* stbu */
    case 40: /* lhz */
    case 41: /* lhzu */
    case 42: /* lha */
    case 43: /* lhau */
    case 44: /* sth */
    case 45: /* sthu */
    case 46: /* lmw */
    case 47: /* stmw */
    {
        uint32_t ea = (RA ? p->cpu.gpr[RA] : 0) + (uint32_t)SIMM;
        int update = (op == 33 || op == 35 || op == 37 || op == 39 ||
                      op == 41 || op == 43 || op == 45);
        int store = (op == 36 || op == 37 || op == 38 || op == 39 ||
                     op == 44 || op == 45 || op == 47);
        unsigned bytes = (op == 32 || op == 33 || op == 36 || op == 37 ||
                          op == 46 || op == 47) ? 4 :
                         (op == 34 || op == 35 || op == 38 || op == 39) ? 1 : 2;
        uint32_t v = 0;

        if (update && RA == 0)
            return fail_at(p, QRX_PPC_FAULT, pc0);
        if ((bytes == 4 && (ea & 3u)) || (bytes == 2 && (ea & 1u))) {
            p->fault_addr = ea;
            return fail_at(p, QRX_PPC_FAULT, pc0);
        }

        if (op == 46) { /* lmw */
            uint32_t i;
            for (i = RT; i < 32; i++) {
                if (!mem_read(p, ea + (i - RT) * 4u, 4, &v))
                    return fail_at(p, QRX_PPC_FAULT, pc0);
                p->cpu.gpr[i] = v;
            }
            break;
        }
        if (op == 47) { /* stmw */
            uint32_t i;
            for (i = RT; i < 32; i++) {
                if (!mem_write(p, ea + (i - RT) * 4u, 4, p->cpu.gpr[i]))
                    return fail_at(p, QRX_PPC_FAULT, pc0);
            }
            break;
        }
        if (!store) {
            if (!mem_read(p, ea, bytes, &v))
                return fail_at(p, QRX_PPC_FAULT, pc0);
            if (op == 42 || op == 43)
                v = (uint32_t)(int32_t)(int16_t)v;
            p->cpu.gpr[RT] = v;
        } else {
            if (!mem_write(p, ea, bytes, p->cpu.gpr[RT]))
                return fail_at(p, QRX_PPC_FAULT, pc0);
        }
        if (update)
            p->cpu.gpr[RA] = ea;
        break;
    }
    /* Floating-point load/store: lfs, lfsu, lfd, lfdu, stfs, stfsu, stfd, stfdu */
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55: {
        uint32_t ea = (RA ? p->cpu.gpr[RA] : 0) + (uint32_t)SIMM;
        int update = (op == 49 || op == 51 || op == 53 || op == 55);
        int store = (op >= 52);
        int dbl = (op == 50 || op == 51 || op == 54 || op == 55);
        unsigned frt = RT;

        if (update && RA == 0)
            return fail_at(p, QRX_PPC_FAULT, pc0);
        if (ea & 3u) {
            p->fault_addr = ea;
            return fail_at(p, QRX_PPC_FAULT, pc0);
        }
        if (!store) {
            if (dbl) {
                uint64_t u;
                if (!mem_read64(p, ea, &u))
                    return fail_at(p, QRX_PPC_FAULT, pc0);
                p->cpu.fpr[frt] = u;
            } else {
                uint32_t u;
                float f;
                if (!mem_read(p, ea, 4, &u))
                    return fail_at(p, QRX_PPC_FAULT, pc0);
                memcpy(&f, &u, sizeof(f));
                fpr_set_d(p, frt, (double)f);
            }
        } else {
            if (dbl) {
                if (!mem_write64(p, ea, p->cpu.fpr[frt]))
                    return fail_at(p, QRX_PPC_FAULT, pc0);
            } else {
                float f = (float)fpr_get_d(p, frt);
                uint32_t u;
                memcpy(&u, &f, sizeof(u));
                if (!mem_write(p, ea, 4, u))
                    return fail_at(p, QRX_PPC_FAULT, pc0);
            }
        }
        if (update)
            p->cpu.gpr[RA] = ea;
        break;
    }
    /* X-form */
    case 31: {
        unsigned xo = (insn >> 1) & 0x3FFu;
        uint32_t a = p->cpu.gpr[RA];
        uint32_t b = p->cpu.gpr[RB];
        uint32_t r = 0;

        switch (xo) {
        case 0: { /* cmp */
            unsigned field = (insn >> 23) & 7u;
            unsigned l = (insn >> 21) & 1u;
            int32_t sa = (int32_t)a;
            int32_t sb = (int32_t)b;
            if (l) {
                sa = (int16_t)sa;
                sb = (int16_t)sb;
            }
            set_cr_compare_s(p, field, sa, sb);
            break;
        }
        case 4: { /* tw */
            unsigned to = (insn >> 21) & 0x1Fu;
            if (trap_condition(to, a, b)) {
                p->cpu.srr0 = pc0;
                p->cpu.srr1 = p->cpu.msr;
                p->status = QRX_PPC_TRAP;
                return QRX_PPC_TRAP;
            }
            break;
        }
        case 8: { /* subfc */
            unsigned ca;
            r = sub_carry(b, a, 1, &ca);
            set_ca(p, ca);
            if (OE)
                set_ov(p, sub_overflow((int32_t)b, (int32_t)a, (int32_t)r));
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 10: { /* addc */
            unsigned ca;
            r = add_carry(a, b, 0, &ca);
            set_ca(p, ca);
            if (OE)
                set_ov(p, add_overflow((int32_t)a, (int32_t)b, (int32_t)r));
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 11: /* mulhwu */
            r = (uint32_t)(((uint64_t)a * b) >> 32);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 19: /* mfcr */
            p->cpu.gpr[RT] = p->cpu.cr;
            break;
        case 24: { /* slw */
            unsigned sh = b & 31u;
            r = sh ? (a << sh) : a;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 26: /* cntlzw */
            r = count_leading_zeroes(p->cpu.gpr[RS]);
            p->cpu.gpr[RA] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 28: /* and */
            r = a & b;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 32: { /* cmpl */
            unsigned field = (insn >> 23) & 7u;
            unsigned l = (insn >> 21) & 1u;
            if (l) {
                a &= 0xFFFFu;
                b &= 0xFFFFu;
            }
            set_cr_compare_u(p, field, a, b);
            break;
        }
        case 40: { /* subf */
            r = b - a;
            if (OE)
                set_ov(p, sub_overflow((int32_t)b, (int32_t)a, (int32_t)r));
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 60: /* andc */
            r = a & ~b;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 75: /* mulhw */
            r = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 104: { /* neg */
            r = 0u - a;
            if (OE)
                set_ov(p, a == 0x80000000u);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 124: /* nor */
            r = ~(a | b);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 136: { /* subfe */
            unsigned ca;
            r = sub_carry(b, a, get_ca(p), &ca);
            set_ca(p, ca);
            if (OE)
                set_ov(p, sub_overflow((int32_t)b, (int32_t)a, (int32_t)r));
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 138: { /* adde */
            unsigned ca;
            r = add_carry(a, b, get_ca(p), &ca);
            set_ca(p, ca);
            if (OE)
                set_ov(p, add_overflow((int32_t)a, (int32_t)b, (int32_t)r));
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 144: { /* mtcrf */
            unsigned crm = (insn >> 12) & 0xFFu;
            unsigned i;
            for (i = 0; i < 8; i++) {
                if (crm & (0x80u >> i))
                    cr_set(p, i, (p->cpu.gpr[RS] >> (28 - i * 4)) & 0xFu);
            }
            break;
        }
        case 200: /* subfze */
            r = ~a + get_ca(p);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 202: /* addze */
            r = a + get_ca(p);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 232: /* subfme */
            r = ~a + get_ca(p) - 1u;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 234: /* addme */
            r = a + get_ca(p) - 1u;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 235: { /* mullw */
            int64_t prod = (int64_t)(int32_t)a * (int32_t)b;
            r = (uint32_t)prod;
            if (OE)
                set_ov(p, prod < INT32_MIN || prod > INT32_MAX);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 266: { /* add */
            r = a + b;
            if (OE)
                set_ov(p, add_overflow((int32_t)a, (int32_t)b, (int32_t)r));
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 284: /* eqv */
            r = ~(a ^ b);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 316: /* xor */
            r = a ^ b;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 339: { /* mfspr */
            uint32_t spr = ((insn >> 16) & 0x1Fu) |
                           ((insn >> 6) & 0x3E0u);
            p->cpu.gpr[RT] = spr_get(p, spr);
            break;
        }
        case 371: /* mftb */
            p->cpu.gpr[RT] = 0; /* no time-base device in the reference core */
            break;
        case 412: /* orc */
            r = a | ~b;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 444: /* or */
            r = a | b;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 459: { /* divwu */
            if (b == 0)
                return fail_at(p, QRX_PPC_FAULT, pc0);
            r = a / b;
            p->cpu.gpr[RT] = r;
            if (OE)
                set_ov(p, 0);
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 467: { /* mtspr */
            uint32_t spr = ((insn >> 16) & 0x1Fu) |
                           ((insn >> 6) & 0x3E0u);
            spr_set(p, spr, p->cpu.gpr[RS]);
            break;
        }
        case 476: /* nand */
            r = ~(a & b);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 491: { /* divw */
            int32_t sa = (int32_t)a;
            int32_t sb = (int32_t)b;
            int ov = 0;

            if (sb == 0)
                return fail_at(p, QRX_PPC_FAULT, pc0);
            if (sa == INT32_MIN && sb == -1) {
                r = (uint32_t)INT32_MIN;
                ov = 1;
            } else {
                r = (uint32_t)(sa / sb);
            }
            if (OE)
                set_ov(p, ov);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 536: { /* srw */
            unsigned sh = b & 31u;
            r = sh ? (a >> sh) : a;
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 150: /* isync */
        case 598: /* sync */
        case 854: /* eieio */
            break;
        case 792: { /* sraw */
            unsigned sh = b & 31u;
            r = sh ? (uint32_t)((int32_t)a >> sh) : a;
            if (sh)
                set_ca(p, (a >> (sh - 1)) & 1u);
            p->cpu.gpr[RT] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 824: { /* srawi */
            unsigned sh = SH;
            int32_t sa = (int32_t)a;
            r = sh ? (uint32_t)(sa >> sh) : a;
            if (sh)
                set_ca(p, (a >> (sh - 1)) & 1u);
            p->cpu.gpr[RA] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        }
        case 922: /* extsh */
            r = (uint32_t)(int32_t)(int16_t)a;
            p->cpu.gpr[RA] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        case 954: /* extsb */
            r = (uint32_t)(int32_t)(int8_t)a;
            p->cpu.gpr[RA] = r;
            if (RC)
                set_cr0_result(p, r);
            break;
        default:
            return fail_at(p, QRX_PPC_ILLEGAL, pc0);
        }
        break;
    }
    /* Floating-point arithmetic (A-form).  Single-precision variants use
     * the same XO values in this reference core and are currently evaluated
     * in double precision. */
    case 59: {
        unsigned xo = (insn >> 1) & 0x1Fu;
        unsigned frt = RT;
        unsigned fra = RA;
        unsigned frb = RB;
        unsigned frc = (insn >> 6) & 0x1Fu;
        uint64_t ua = p->cpu.fpr[fra];
        uint64_t ub = p->cpu.fpr[frb];
        uint64_t result;

        switch (xo) {
        case 18: result = qrx_fp_div(p, ua, ub); break;
        case 20: result = qrx_fp_add(p, ua, ub, 1); break;
        case 21: result = qrx_fp_add(p, ua, ub, 0); break;
        case 23: /* fsel: select on the sign bit of FRC */
            result = (p->cpu.fpr[frc] & 0x8000000000000000ull) ? ub : ua;
            break;
        case 25: result = qrx_fp_mul(p, ua, ub); break;
        default:
            return fail_at(p, QRX_PPC_ILLEGAL, pc0);
        }
        p->cpu.fpr[frt] = result;
        qrx_fp_class(p, result);
        break;
    }
    /* Floating-point X-form */
    case 63: {
        unsigned xo = (insn >> 1) & 0x3FFu;
        unsigned frt = RT;
        unsigned fra = RA;
        unsigned frb = RB;
        uint64_t ua = p->cpu.fpr[fra];
        uint64_t ub = p->cpu.fpr[frb];

        switch (xo) {
        case 0: /* fcmpu */
            qrx_fp_compare(p, (insn >> 23) & 7u, ua, ub, 0);
            break;
        case 32: /* fcmpo */
            qrx_fp_compare(p, (insn >> 23) & 7u, ua, ub, 1);
            break;
        case 12: /* frsp */
            p->cpu.fpr[frt] = qrx_fp_frsp(p, ub);
            break;
        case 14: /* fctiw */
            p->cpu.fpr[frt] = qrx_fp_fctiw(p, ub, 0);
            break;
        case 15: /* fctiwz */
            p->cpu.fpr[frt] = qrx_fp_fctiw(p, ub, 1);
            break;
        case 40: /* fneg */
            p->cpu.fpr[frt] = ub ^ 0x8000000000000000ull;
            qrx_fp_class(p, p->cpu.fpr[frt]);
            break;
        case 72: /* fmr */
            p->cpu.fpr[frt] = ub;
            qrx_fp_class(p, p->cpu.fpr[frt]);
            break;
        case 136: /* fnabs */
            p->cpu.fpr[frt] = ub | 0x8000000000000000ull;
            qrx_fp_class(p, p->cpu.fpr[frt]);
            break;
        case 264: /* fabs */
            p->cpu.fpr[frt] = ub & ~0x8000000000000000ull;
            qrx_fp_class(p, p->cpu.fpr[frt]);
            break;
        case 583: /* mffs */
            p->cpu.fpr[frt] = p->cpu.fpscr;
            break;
        case 711: /* mtfsf; FM selects 4-bit FPSCR fields */
            qrx_fp_mtfsf(p, p->cpu.fpr[frb], (insn >> 17) & 0xFFu);
            break;
        default:
            return fail_at(p, QRX_PPC_ILLEGAL, pc0);
        }
        break;
    }

    default:
        return fail_at(p, QRX_PPC_ILLEGAL, pc0);
    }

    p->status = QRX_PPC_OK;
    return QRX_PPC_OK;
}

void qrx_ppc_init(qrx_ppc_t *p, uint8_t *mem, uint32_t size)
{
    memset(p, 0, sizeof(*p));
    p->mem.data = mem;
    p->mem.size = size;
    p->status = QRX_PPC_OK;
}

qrx_ppc_status_t qrx_ppc_run(qrx_ppc_t *p, uint32_t max_insns)
{
    qrx_ppc_status_t status = QRX_PPC_OK;

    p->insns = 0;
    while (p->insns < max_insns) {
        if (p->cpu.pc == 0) {
            p->status = QRX_PPC_HALT;
            return QRX_PPC_HALT;
        }
        status = qrx_ppc_step(p);
        p->insns++;
        if (status != QRX_PPC_OK)
            return status;
    }
    p->status = QRX_PPC_OK;
    return QRX_PPC_OK;
}
