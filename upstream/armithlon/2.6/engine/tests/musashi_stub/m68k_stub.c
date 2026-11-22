/* TEST DOUBLE ONLY: models register contexts, NOT executing 68k instructions. */
#include "m68k.h"
#include <string.h>
extern unsigned int m68k_read_memory_32(unsigned int);
typedef struct { unsigned regs[18], irq, typ; } Context;
static Context cpu;
void m68k_init(void){memset(&cpu,0,sizeof(cpu));}
void m68k_set_cpu_type(unsigned int t){cpu.typ=t;}
void m68k_set_int_ack_callback(int (*cb)(int)){(void)cb;}
void m68k_set_instr_hook_callback(void (*cb)(unsigned int)){(void)cb;}
void m68k_pulse_reset(void){cpu.regs[M68K_REG_A7]=m68k_read_memory_32(0);cpu.regs[M68K_REG_PC]=m68k_read_memory_32(4);cpu.regs[M68K_REG_SR]=0x2700;}
void m68k_set_irq(unsigned int n){cpu.irq=n;}
void m68k_end_timeslice(void){}
int m68k_execute(int n){return n;}
unsigned int m68k_get_reg(void *c,m68k_register_t reg){(void)c;return cpu.regs[reg];}
void m68k_set_reg(m68k_register_t reg,unsigned int v){cpu.regs[reg]=v;}
unsigned int m68k_context_size(void){return (unsigned)sizeof cpu;}
unsigned int m68k_get_context(void *p){if(p)memcpy(p,&cpu,sizeof cpu);return sizeof cpu;}
void m68k_set_context(void *p){if(p)memcpy(&cpu,p,sizeof cpu);}
