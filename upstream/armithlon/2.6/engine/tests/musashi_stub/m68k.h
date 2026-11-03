/* TEST DOUBLE ONLY: not an instruction-set emulator. Included solely for bus tests. */
#ifndef QRX_TEST_MUSASHI_STUB_H
#define QRX_TEST_MUSASHI_STUB_H
typedef enum { M68K_REG_D0,M68K_REG_D1,M68K_REG_D2,M68K_REG_D3,M68K_REG_D4,M68K_REG_D5,M68K_REG_D6,M68K_REG_D7,M68K_REG_A0,M68K_REG_A1,M68K_REG_A2,M68K_REG_A3,M68K_REG_A4,M68K_REG_A5,M68K_REG_A6,M68K_REG_A7,M68K_REG_PC,M68K_REG_SR } m68k_register_t;
#define M68K_CPU_TYPE_INVALID 0
#define M68K_CPU_TYPE_68000 1
#define M68K_CPU_TYPE_68010 2
#define M68K_CPU_TYPE_68020 3
#define M68K_CPU_TYPE_68030 4
#define M68K_CPU_TYPE_68040 5
#define M68K_INT_ACK_AUTOVECTOR 0xffffffffu
void m68k_init(void);
void m68k_set_cpu_type(unsigned int);
void m68k_set_int_ack_callback(int (*)(int));
void m68k_set_instr_hook_callback(void (*)(unsigned int));
void m68k_pulse_reset(void);
void m68k_set_irq(unsigned int);
void m68k_end_timeslice(void);
int m68k_execute(int);
unsigned int m68k_get_reg(void*,m68k_register_t);
#endif

unsigned int m68k_context_size(void);
unsigned int m68k_get_context(void*);
void m68k_set_context(void*);
void m68k_set_reg(m68k_register_t,unsigned int);
