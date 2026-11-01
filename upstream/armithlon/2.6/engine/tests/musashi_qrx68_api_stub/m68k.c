/* Nonexecuting stub. Only proves the adapter compiles and links as a unit.
 * Actual Musashi ISA/FPU/exception proof requires downloaded pinned source. */
#include "m68k.h"
static unsigned regs[64];
void m68k_init(void){for(unsigned i=0;i<64;i++)regs[i]=0;}
void m68k_set_cpu_type(unsigned int type){(void)type;}
void m68k_pulse_reset(void){}
void m68k_set_instr_hook_callback(void (*cb)(unsigned int)){(void)cb;}
void m68k_end_timeslice(void){}
int m68k_execute(int n){(void)n;return 0;}
unsigned int m68k_get_reg(void *ctx,m68k_register_t reg){(void)ctx;return regs[reg];}
void m68k_set_reg(m68k_register_t reg,unsigned int value){regs[reg]=value;}
