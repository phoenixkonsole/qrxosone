/* QRX68 full 68040 CPU execution adapter.
 * The pinned external Musashi source provides the actual ISA, FPU and exception
 * implementation. QRX owns RAM, 68k LVO HLE, deterministic call boundaries and
 * a bounded guest CPU scheduler. No host pointers or trap pages are executable.
 *
 * Musashi uses process-global state/callbacks. ONE CPU per QRX68 process here.
 * On a later SMP integration it must be hosted in separate processes or use
 * a context-aware Musashi fork with a verified memory callback ABI.
 */
#include "qrx68_hle.h"
#include "m68k.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static QRX68 *current;
static uint64_t instruction_hooks;
static uint32_t last_pc;
static bool bus_fault;

static bool region(uint32_t addr, unsigned width){
    if(!current || !current->memory || !width ||
       (uint64_t)addr+width>current->memory_size){
        bus_fault=true;
        if(current)current->failed=1;
        return false;
    }
    return true;
}
static unsigned read_bus(unsigned addr,unsigned width){
    if(!region(addr,width))return 0;
    uint32_t v=0;
    for(unsigned i=0;i<width;i++)v=(v<<8)|current->memory[addr+i];
    return v;
}
static void write_bus(unsigned addr,unsigned width,unsigned value){
    if(!region(addr,width))return;
    for(unsigned i=0;i<width;i++)
        current->memory[addr+i]=(uint8_t)(value>>(8u*(width-i-1u)));
    current->vm.ram_write_epoch++;
}
unsigned int m68k_read_memory_8(unsigned int addr){return read_bus(addr,1);}
unsigned int m68k_read_memory_16(unsigned int addr){return read_bus(addr,2);}
unsigned int m68k_read_memory_32(unsigned int addr){return read_bus(addr,4);}
void m68k_write_memory_8(unsigned int addr,unsigned int v){write_bus(addr,1,v);}
void m68k_write_memory_16(unsigned int addr,unsigned int v){write_bus(addr,2,v);}
void m68k_write_memory_32(unsigned int addr,unsigned int v){write_bus(addr,4,v);}
void m68k_write_memory_32_pd(unsigned int addr,unsigned int v){write_bus(addr,4,v);}
static void instruction_hook(unsigned int pc){
    instruction_hooks++;
    last_pc=pc;
    /* Stop before excessive interpreter executions in one Musashi timeslice.
     * Musashi still retires the current opcode after this hook, so count it. */
    if(instruction_hooks>2)m68k_end_timeslice();
}
static void cpu_to_guest(QRX68 *r){
    QRXCPU *c=&r->vm.cpu;
    for(unsigned i=0;i<8;i++){
        c->d[i]=m68k_get_reg(NULL,(m68k_register_t)(M68K_REG_D0+i));
        c->a[i]=m68k_get_reg(NULL,(m68k_register_t)(M68K_REG_A0+i));
    }
    c->pc=m68k_get_reg(NULL,M68K_REG_PC);
    c->sr=(uint16_t)m68k_get_reg(NULL,M68K_REG_SR);
    c->usp=m68k_get_reg(NULL,M68K_REG_USP);
    c->ssp=m68k_get_reg(NULL,M68K_REG_ISP);
}
static void guest_to_cpu(QRX68 *r){
    QRXCPU *c=&r->vm.cpu;
    /* A7 is the currently active stack; change SR before assigning A7. */
    m68k_set_reg(M68K_REG_USP,c->usp);
    m68k_set_reg(M68K_REG_ISP,c->ssp);
    m68k_set_reg(M68K_REG_SR,c->sr);
    for(unsigned i=0;i<8;i++){
        m68k_set_reg((m68k_register_t)(M68K_REG_D0+i),c->d[i]);
        if(i!=7)m68k_set_reg((m68k_register_t)(M68K_REG_A0+i),c->a[i]);
    }
    m68k_set_reg(M68K_REG_A7,c->a[7]);
    m68k_set_reg(M68K_REG_PC,c->pc);
}
bool qrx68_musashi_begin(QRX68 *r){
    if(!r||!r->memory||current)return false;
    current=r;bus_fault=false;instruction_hooks=0;last_pc=0;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    m68k_pulse_reset();
    m68k_set_instr_hook_callback(instruction_hook);
    guest_to_cpu(r);
    return true;
}
int qrx68_musashi_slice(QRX68 *r,uint64_t step_budget){
    if(!r||current!=r||!step_budget)return -1;
    int result=1;
    for(uint64_t i=0;i<step_budget;i++){
        cpu_to_guest(r);
        int b=qrx68_boundary(r);
        if(b<0){result=r->complete?0:-1;break;}
        if(b==2){result=2;break;}
        if(b>0){guest_to_cpu(r);continue;}
        instruction_hooks=0;
        int cycles=m68k_execute(1);
        if(bus_fault||r->failed||cycles<0){r->failed=1;result=-1;break;}
        if(!instruction_hooks){r->failed=1;result=-1;break;}
        r->steps+=instruction_hooks;
        if(instruction_hooks>2){r->failed=1;result=-1;break;}
    }
    cpu_to_guest(r);
    if(result==1 && r->vm.cpu.pc==QRX68_RETURN_PC){r->complete=1;result=0;}
    else if(result==1 && r->failed)result=-1;
    return result;
}
void qrx68_musashi_end(QRX68 *r){
    if(current==r){cpu_to_guest(r);current=NULL;}
}
int qrx68_run_musashi(QRX68 *r,uint64_t budget){
    if(!qrx68_musashi_begin(r))return -1;
    int result=qrx68_musashi_slice(r,budget);
    qrx68_musashi_end(r);
    return result;
}
