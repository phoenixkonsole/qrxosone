#include "qrx68100.h"

static bool fetch16(const QRXRAM *r, uint32_t pc, uint16_t *op) {
    if (!r || !r->data || pc >= r->size || r->size - pc < 2) return false;
    *op = (uint16_t)(((uint16_t)r->data[pc] << 8) | r->data[pc + 1]);
    return true;
}

static void set_move_flags(QRXCPU *c, int32_t v) {
    /* Preserve X and the upper status register; clear NZVC. */
    c->sr = (uint16_t)((c->sr & 0xfff0u) | (v == 0 ? 4u : 0u) | (v < 0 ? 8u : 0u));
}

/* A single portable oracle step. TRAP #15 is a FOUNDATION-ONLY test halt. */
bool qrx_step(QRXCPU *c, const QRXRAM *ram) {
    if (!c || c->halted) return false;
    uint16_t op;
    if (!fetch16(ram, c->pc, &op)) return false;
    if ((op & 0xF100u) == 0x7000u) { /* MOVEQ #imm8,Dn */
        const unsigned reg = (op >> 9) & 7u;
        const int32_t val = (int8_t)(op & 0xffu);
        c->d[reg] = (uint32_t)val;
        set_move_flags(c, val);
        c->pc += 2;
        return true;
    }
    if (op == 0x4e71u) { /* NOP */
        c->pc += 2;
        return true;
    }
    if (op == 0x4e4fu) { /* TRAP #15 -> test-only halt */
        c->pc += 2;
        c->halted = 1;
        return true;
    }
    return false;
}

int qrx_run_interpreter(QRXCPU *c, const QRXRAM *ram, uint32_t max_steps) {
    uint32_t count = 0;
    while (!c->halted && count < max_steps) {
        if (!qrx_step(c, ram)) return -1;
        ++count;
    }
    return c->halted ? (int)count : -2;
}
