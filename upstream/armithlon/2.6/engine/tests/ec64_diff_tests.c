/* The 68100EC64's 32-bit mode against Musashi's 68040: random instructions, random registers,
 * the same memory; registers, flags, PC and memory must match afterwards. Cases where Musashi
 * takes an exception or the new core reports no such instruction do not count as compared. */
#include "qrx68100ec64.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WANT_DEFAULT 400000

#define RAM 0x40000u
#define CODE 0x20000u
#define VEC_TARGET 0x1000u
static uint8_t ram_m[RAM], ram_e[RAM];
static int musashi_out_of_range;
unsigned int m68k_read_memory_8(unsigned int a) { if (a >= RAM) { musashi_out_of_range = 1; return 0; } return ram_m[a]; }
unsigned int m68k_read_memory_16(unsigned int a) { return m68k_read_memory_8(a) << 8 | m68k_read_memory_8(a + 1); }
unsigned int m68k_read_memory_32(unsigned int a) { return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (a >= RAM) { musashi_out_of_range = 1; return; } ram_m[a] = (uint8_t)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { m68k_write_memory_8(a, v >> 8); m68k_write_memory_8(a + 1, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
void qrx_test_hook(unsigned int pc) { (void)pc; }

static unsigned long missing[1024];
static uint64_t rs = 0x9E3779B97F4A7C15u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint32_t rnd_data(void)
{
    switch (rnd() & 7) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return 0x80000000u;
    case 3: return 0x7FFFFFFFu;
    case 4: return rnd() & 0xFF;
    case 5: return rnd() & 0xFFFF;
    default: return rnd();
    }
}
int main(int argc, char **argv)
{
    unsigned long want = argc > 1 ? strtoul(argv[1], NULL, 10) : 400000;
    unsigned long compared = 0, skipped_unimpl = 0, skipped_exc = 0, skipped_fault = 0, bad = 0;
    unsigned long per_line[16] = { 0 };
    static qec64_cpu e;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    for (unsigned long n = 0; n < want; n++) {
        for (unsigned i = 0; i < RAM; i += 4) { if (i < 0x2000 || (i >> 12) == (CODE >> 12) || !(n & 63)) { uint32_t v = n & 1 ? rnd() : 0x01020304u * (i >> 2); ram_m[i] = (uint8_t)(v >> 24); ram_m[i + 1] = (uint8_t)(v >> 16); ram_m[i + 2] = (uint8_t)(v >> 8); ram_m[i + 3] = (uint8_t)v; } }
        /* every exception vector points at VEC_TARGET */
        for (unsigned v = 0; v < 256; v++) { unsigned a = v * 4; ram_m[a] = 0; ram_m[a + 1] = 0; ram_m[a + 2] = VEC_TARGET >> 8; ram_m[a + 3] = 0; }
        ram_m[0] = 0; ram_m[1] = 0; ram_m[2] = 0x30; ram_m[3] = 0;   /* reset SSP */
        ram_m[6] = CODE >> 8 >> 8; ram_m[5] = 0; ram_m[4] = 0; ram_m[7] = 0;
        ram_m[5] = (CODE >> 16) & 255; ram_m[6] = (CODE >> 8) & 255; ram_m[7] = CODE & 255;
        ram_m[VEC_TARGET] = 0x4E; ram_m[VEC_TARGET + 1] = 0x71;
        uint16_t op;
        do {
            op = (uint16_t)rnd();
        } while ((op >> 12) == 0xA || (op >> 12) == 0xF || (op & 0xFFF0) == 0x4E40 ||
                 /* CMP2.B and CHK2.B with a PC relative operand: Musashi reads those bounds without
                    the sign extension it uses for the same instruction elsewhere. */
                 op == 0x00FA || op == 0x00FB ||
                 op == 0x4E70 || op == 0x4E72 || op == 0x4E73 || op == 0x4E76 || op == 0x4E77 || op == 0x4AFC);
        ram_m[CODE] = (uint8_t)(op >> 8); ram_m[CODE + 1] = (uint8_t)op;
        /* extension words pointing near the data: small displacements, register numbers kept */
        for (unsigned i = 2; i < 16; i += 2) {
            uint16_t ext = (uint16_t)(rnd() & 0x71FF);
            if (rnd() & 1) ext = (uint16_t)(rnd() & 0xFC);
            ram_m[CODE + i] = (uint8_t)(ext >> 8); ram_m[CODE + i + 1] = (uint8_t)ext;
        }
        m68k_pulse_reset();
        m68k_execute(1);   /* only eats the reset's cycles */
        /* user mode so nothing privileged slips through as "works" */
        uint32_t d[8], a[8];
        unsigned ccr = rnd() & 0x1F;
        for (unsigned i = 0; i < 8; i++) {
            d[i] = rnd_data();
            a[i] = 0x8000u + (rnd() & 0x7FFE) + ((rnd() & 15) == 0 ? 1 : 0);
            m68k_set_reg(M68K_REG_D0 + i, d[i]);
            if (i < 7) m68k_set_reg(M68K_REG_A0 + i, a[i]);
        }
        a[7] = a[7] & ~1u;
        m68k_set_reg(M68K_REG_SR, ccr);
        m68k_set_reg(M68K_REG_USP, 0);
        m68k_set_reg(M68K_REG_SP, a[7]);
        m68k_set_reg(M68K_REG_ISP, 0x3000);
        m68k_set_reg(M68K_REG_PC, CODE);
        memcpy(ram_e, ram_m, RAM);
        musashi_out_of_range = 0;
        qec64_init(&e, ram_e, RAM, 0);
        for (unsigned i = 0; i < 8; i++) { e.d[i] = d[i]; e.a[i] = a[i]; }
        e.ccr = (uint8_t)ccr; e.pc = CODE;
        e.supervisor = false;   /* as Musashi above: user state */
        enum qec64_status st = qec64_run(&e, 1);
        if (st == QEC64_UNIMPLEMENTED) {
            skipped_unimpl++;
            m68k_execute(1);
            uint32_t p = m68k_get_reg(NULL, M68K_REG_PC);
            if (!(p >= VEC_TARGET && p < VEC_TARGET + 4) && !(m68k_get_reg(NULL, M68K_REG_SR) & 0x2000) && !musashi_out_of_range) missing[op >> 6]++;
            continue;
        }
        if (st != QEC64_RUNNING) { skipped_fault++; continue; }
        m68k_execute(1);
        uint32_t mpc = m68k_get_reg(NULL, M68K_REG_PC);
        if (musashi_out_of_range) { skipped_fault++; continue; }
        if (mpc >= VEC_TARGET && mpc < VEC_TARGET + 4) { skipped_exc++; continue; }
        if (m68k_get_reg(NULL, M68K_REG_SR) & 0x2000) {
            /* Musashi went supervisor: an exception whose handler we did not recognise */
            skipped_exc++;
            continue;
        }
        bool diff = (uint32_t)e.pc != mpc || (m68k_get_reg(NULL, M68K_REG_SR) & 0x1F) != e.ccr;
        for (unsigned i = 0; i < 8; i++) {
            if ((uint32_t)e.d[i] != m68k_get_reg(NULL, M68K_REG_D0 + i)) diff = true;
            if ((uint32_t)e.a[i] != m68k_get_reg(NULL, M68K_REG_A0 + i)) diff = true;
        }
        if (memcmp(ram_e, ram_m, RAM) != 0) diff = true;
        compared++;
        per_line[op >> 12]++;
        if (diff) {
            if (bad++ < 40) {
                printf("op %04X ext %02X%02X ccr in %02X: musashi pc %06X ccr %02X, ec64 pc %06llX ccr %02X\n", op, ram_m[CODE + 2], ram_m[CODE + 3], ccr,
                       mpc, (unsigned)(m68k_get_reg(NULL, M68K_REG_SR) & 0x1F), (unsigned long long)e.pc, e.ccr);
                for (unsigned i = 0; i < 8; i++) {
                    uint32_t md = m68k_get_reg(NULL, M68K_REG_D0 + i), ma = m68k_get_reg(NULL, M68K_REG_A0 + i);
                    if (md != (uint32_t)e.d[i]) printf("  d%u in %08X musashi %08X ec64 %08X\n", i, d[i], md, (uint32_t)e.d[i]);
                    if (ma != (uint32_t)e.a[i]) printf("  a%u in %08X musashi %08X ec64 %08X\n", i, a[i], ma, (uint32_t)e.a[i]);
                }
                if (memcmp(ram_e, ram_m, RAM)) for (unsigned i = 0; i < RAM; i++) if (ram_e[i] != ram_m[i]) { printf("  mem %05X musashi %02X ec64 %02X\n", i, ram_m[i], ram_e[i]); break; }
            }
        }
    }
    printf("compared %lu, differences %lu, not implemented %lu, Musashi exception %lu, outside RAM or stop %lu\n", compared, bad, skipped_unimpl, skipped_exc, skipped_fault);
    for (unsigned i = 0; i < 16; i++) if (per_line[i]) printf(" line %X: %lu", i, per_line[i]);
    puts("");
    /* What Musashi can do and this core cannot, by the top ten bits of the opcode. */
    for (unsigned i = 0; i < 1024; i++) if (missing[i]) printf("missing %04X: %lu\n", i << 6, missing[i]);
    return bad || compared < want / 20 ? 1 : 0;
}
