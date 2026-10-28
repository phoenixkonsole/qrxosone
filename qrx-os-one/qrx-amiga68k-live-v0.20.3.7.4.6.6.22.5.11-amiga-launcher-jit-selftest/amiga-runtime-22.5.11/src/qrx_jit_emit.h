/* QRX OS One 22.5.11 - minimal m68k byte emitter for the opt-in JIT self-test.
 * Portable (host + target).  Encodings are verified against GNU as (m68k) by
 * tests/test-jit-emit-host.sh; they are NOT used by QRX86GRTExec, whose native
 * JIT stays quarantined. */
#ifndef QRX_JIT_EMIT_H
#define QRX_JIT_EMIT_H

struct qrx_emit {
    unsigned char *p;
    unsigned long cap, len;
    int overflow;
};

static void qe_init(struct qrx_emit *e, void *buf, unsigned long cap)
{
    e->p = (unsigned char *)buf; e->cap = cap; e->len = 0; e->overflow = 0;
}
static void qe_w16(struct qrx_emit *e, unsigned long v)
{
    if (e->cap - e->len < 2 || e->len > e->cap) { e->overflow = 1; return; }
    e->p[e->len++] = (unsigned char)(v >> 8);
    e->p[e->len++] = (unsigned char)v;
}
static void qe_w32(struct qrx_emit *e, unsigned long v) { qe_w16(e, (v >> 16) & 0xFFFFUL); qe_w16(e, v & 0xFFFFUL); }
/* MOVEQ #imm8,Dn */
static void qe_moveq(struct qrx_emit *e, int imm, int dn) { qe_w16(e, 0x7000UL | ((unsigned long)(dn & 7) << 9) | ((unsigned long)imm & 0xFFUL)); }
/* MOVE.L #imm32,Dn */
static void qe_move_l_imm(struct qrx_emit *e, unsigned long imm, int dn) { qe_w16(e, 0x203CUL | ((unsigned long)(dn & 7) << 9)); qe_w32(e, imm); }
/* ADDQ.L #q,Dn  (q = 1..8) */
static void qe_addq_l(struct qrx_emit *e, int q, int dn) { qe_w16(e, 0x5080UL | ((unsigned long)(q & 7) << 9) | (unsigned long)(dn & 7)); }
/* JSR abs.l */
static void qe_jsr_abs(struct qrx_emit *e, unsigned long addr) { qe_w16(e, 0x4EB9UL); qe_w32(e, addr); }
static void qe_nop(struct qrx_emit *e) { qe_w16(e, 0x4E71UL); }
static void qe_rts(struct qrx_emit *e) { qe_w16(e, 0x4E75UL); }

#endif
