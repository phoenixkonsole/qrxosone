/* ABI LINKCHECK TEST DOUBLE, NOT Musashi and DOES NOT EXECUTE 68k!
 * This header uses the same symbols as pinned public Musashi m68k.h.
 */
#ifndef QRX68_MUSASHI_ABI_STUB_H
#define QRX68_MUSASHI_ABI_STUB_H
#include <stdint.h>
typedef enum {
 M68K_REG_D0,M68K_REG_D1,M68K_REG_D2,M68K_REG_D3,
 M68K_REG_D4,M68K_REG_D5,M68K_REG_D6,M68K_REG_D7,
 M68K_REG_A0,M68K_REG_A1,M68K_REG_A2,M68K_REG_A3,
 M68K_REG_A4,M68K_REG_A5,M68K_REG_A6,M68K_REG_A7,
 M68K_REG_PC,M68K_REG_SR,M68K_REG_SP,M68K_REG_USP,
 M68K_REG_ISP,M68K_REG_MSP
} m68k_register_t;
#define M68K_CPU_TYPE_68040 10u
void m68k_init(void);
void m68k_set_cpu_type(unsigned int);
void m68k_pulse_reset(void);
void m68k_set_instr_hook_callback(void (*cb)(unsigned int));
void m68k_end_timeslice(void);
int m68k_execute(int);
unsigned int m68k_get_reg(void*,m68k_register_t);
void m68k_set_reg(m68k_register_t,unsigned int);
#endif
