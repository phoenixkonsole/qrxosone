#include "qrx68100_full.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Musashi is intentionally single-context: no silent CPU-state crossover. */
static QRXFullCPU *active;
static unsigned full_model(unsigned m) {
    switch (m) {
    case 68000: return M68K_CPU_TYPE_68000;
    case 68010: return M68K_CPU_TYPE_68010;
    case 68020: return M68K_CPU_TYPE_68020;
    case 68030: return M68K_CPU_TYPE_68030;
    case 68040: return M68K_CPU_TYPE_68040;
    default: return M68K_CPU_TYPE_INVALID;
    }
}
static bool upper_io(uint32_t addr,unsigned width) {
    return (uint64_t)addr>=QRX_FULL_IO_BASE && (uint64_t)addr+width<=(uint64_t)QRX_FULL_IO_BASE+QRX_IO_LENGTH;
}
static uint32_t norm_addr(uint32_t addr,unsigned width) {
    return upper_io(addr,width) ? QRX_IO_BASE+(addr-QRX_FULL_IO_BASE) : addr;
}
static bool rom_read(const QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out) {
    const uint8_t *rom = NULL;
    uint64_t ofs = 0;
    size_t len = 0;
    if (c->rom_main && addr >= QRX_FULL_ROM_MAIN_BASE &&
        (uint64_t)addr + width <= (uint64_t)QRX_FULL_ROM_MAIN_BASE + c->rom_main_size) {
        rom=c->rom_main;len=c->rom_main_size;ofs=addr-QRX_FULL_ROM_MAIN_BASE;
    } else if (c->rom_ext && addr >= QRX_FULL_ROM_EXT_BASE &&
        (uint64_t)addr + width <= (uint64_t)QRX_FULL_ROM_EXT_BASE + c->rom_ext_size) {
        rom=c->rom_ext;len=c->rom_ext_size;ofs=addr-QRX_FULL_ROM_EXT_BASE;
    } else if (c->rom_overlay && c->rom_main && (uint64_t)addr + width <= 8u) {
        rom=c->rom_main;len=c->rom_main_size;ofs=addr;
    }
    if (!rom || ofs + width > len) return false;
    uint32_t v=0;
    for (unsigned i=0;i<width;i++) v=(v<<8)|rom[ofs+i];
    *out=v;
    return true;
}
static bool rom_overlaps(const QRXFullCPU *c, uint32_t a, unsigned width) {
    uint64_t e=(uint64_t)a+width;
    if (c->rom_main && a < (uint64_t)QRX_FULL_ROM_MAIN_BASE+c->rom_main_size && e>QRX_FULL_ROM_MAIN_BASE) return true;
    if (c->rom_ext && a < (uint64_t)QRX_FULL_ROM_EXT_BASE+c->rom_ext_size && e>QRX_FULL_ROM_EXT_BASE) return true;
    if (c->rom_overlay && c->rom_main && a<8 && e>0) return true;
    return false;
}
static bool full_internal_read(QRXFullCPU *c,uint32_t a,unsigned n,uint32_t *out) {
    if(a==QRX_FULL_TIMER_CTRL && n==4) {*out=c->timer_control;return true;}
    if(a==QRX_FULL_TIMER_PERIOD && n==4) {*out=c->timer_period;return true;}
    if(a==QRX_FULL_IRQ_PENDING && n==4) {*out=c->irq_pending ? 1u<<2 : 0;return true;}
    if(a==QRX_FULL_ROM_OVERLAY_CTRL && n==4) {*out=c->rom_overlay ? 1u : 0u;return true;}
    return false;
}
static bool full_internal_write(QRXFullCPU *c,uint32_t a,unsigned n,uint32_t val) {
    if (n!=4) return false;
    if(a==QRX_FULL_TIMER_CTRL) {
        c->timer_control=val&3u;
        if(c->timer_control&QRX_FULL_TIMER_ENABLE) c->timer_remaining=c->timer_period;
        return true;
    }
    if(a==QRX_FULL_TIMER_PERIOD) {
        c->timer_period=val;
        c->timer_remaining=val;
        return true;
    }
    if(a==QRX_FULL_IRQ_ACK) {
        if(val&(1u<<2)) {c->irq_pending=0; if(c->aux_cpu_index==0) m68k_set_irq(c->irq_level);}
        return true;
    }
    if(a==QRX_FULL_ROM_OVERLAY_CTRL) {c->rom_overlay=(val&1u)!=0;return true;}
    return false;
}
static bool overlap_internal(uint32_t a,unsigned n) {
    return (uint64_t)a<QRX_FULL_ROM_OVERLAY_CTRL+4u && (uint64_t)a+n>QRX_FULL_TIMER_CTRL;
}
bool qrx_full_bus_read(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t *out) {
    if (!c || !out || (width!=1&&width!=2&&width!=4)) return false;
    if (upper_io(addr,width)) {
        if (c->aux_read && c->aux_read(c->aux_opaque, addr, width, out)) return true;
        if (overlap_internal(addr,width)) return full_internal_read(c,addr,width,out);
        return qrx_vm_read(&c->vm,norm_addr(addr,width),width,out);
    }
    /* Old FF0000 virtual-board alias retained for RAM boot-monitor tests; it
       is shadowed by the actual F80000 Kickstart ROM when that ROM is present. */
    if (rom_read(c,addr,width,out)) return true;
    if (rom_overlaps(c,addr,width)) return false;
    return qrx_vm_read(&c->vm,addr,width,out);
}
bool qrx_full_bus_write(QRXFullCPU *c, uint32_t addr, unsigned width, uint32_t value) {
    if (!c || (width!=1&&width!=2&&width!=4)) return false;
    if(upper_io(addr,width)) {
        if(c->aux_write && c->aux_write(c->aux_opaque,addr,width,value)) return true;
        if(overlap_internal(addr,width)) return full_internal_write(c,addr,width,value);
        addr=norm_addr(addr,width);
    } else if(rom_overlaps(c,addr,width)) return false;
    bool ok=qrx_vm_write(&c->vm,addr,width,value);
    if (ok && c->ram_write && (uint64_t)addr < c->vm.ram.size)
        c->ram_write(c->aux_opaque,addr,width);
    if (ok && c->vm.cpu.halted && c->reset_done) m68k_end_timeslice();
    return ok;
}
static unsigned int core_read(unsigned address,unsigned width) {
    uint32_t out=0;
    if(!active || !qrx_full_bus_read(active,address,width,&out)) {
        if(active) {active->mem_fault=true;active->fault_address=address;m68k_end_timeslice();}
        return UINT32_MAX;
    }
    return out;
}
static void core_write(unsigned address,unsigned width,unsigned value) {
    if(!active || !qrx_full_bus_write(active,address,width,value)) {
        if(active) {active->mem_fault=true;active->fault_address=address;m68k_end_timeslice();}
    }
}
unsigned int m68k_read_memory_8(unsigned int a){return core_read(a,1);}
unsigned int m68k_read_memory_16(unsigned int a){return core_read(a,2);}
unsigned int m68k_read_memory_32(unsigned int a){return core_read(a,4);}
void m68k_write_memory_8(unsigned int a,unsigned int v){core_write(a,1,v);}
void m68k_write_memory_16(unsigned int a,unsigned int v){core_write(a,2,v);}
void m68k_write_memory_32(unsigned int a,unsigned int v){core_write(a,4,v);}
void m68k_write_memory_32_pd(unsigned int a,unsigned int v) {
    core_write(a+2u,2,v&0xffffu);core_write(a,2,(v>>16)&0xffffu);
}
static void instr_hook(unsigned pc){
    if(!active)return;
    /* The next fetched instruction is a harmless NOP, which consumes the
       remaining slice; never execute the firmware PC during the subcall. */
    if(active->subroutine_armed && pc==active->subroutine_sentinel){
        active->subroutine_returned=true;
        m68k_end_timeslice();
    }
    active->instructions++;
}
static int int_ack(int level) {(void)level;return (int)M68K_INT_ACK_AUTOVECTOR;}
bool qrx_full_init(QRXFullCPU *c,size_t ram_size,unsigned model) {
    if(!c || active || !full_model(model) || ram_size<1024 || ram_size>QRX_RAM_MAX_BYTES) return false;
    memset(c,0,sizeof(*c));
    if(!qrx_ram_allocate(&c->host_allocation,ram_size))return false;
    c->ram_owned=(uint8_t*)c->host_allocation.addr;
    qrx_vm_init(&c->vm,c->ram_owned,ram_size);
    c->cpu_type=(int)model;
    active=c;
    m68k_init();
    m68k_set_cpu_type(full_model(model));
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(instr_hook);
    return true;
}
void qrx_full_destroy(QRXFullCPU *c){
    if(!c)return;
    if(active==c){m68k_set_irq(0);active=NULL;}
    qrx_vm_destroy(&c->vm);
    free(c->rom_main);free(c->rom_ext);qrx_ram_release(&c->host_allocation);
    memset(c,0,sizeof(*c));
}
bool qrx_full_load_rom(QRXFullCPU *c,uint32_t base,const void *data,size_t size){
    if(!c||!data||!size||size>QRX_FULL_ROM_MAX) return false;
    if(base!=QRX_FULL_ROM_MAIN_BASE && base!=QRX_FULL_ROM_EXT_BASE) return false;
    if(base==QRX_FULL_ROM_MAIN_BASE && size!=512u*1024u) return false;
    if(base==QRX_FULL_ROM_EXT_BASE && size!=512u*1024u) return false;
    uint8_t *newrom=(uint8_t*)malloc(size);
    if(!newrom)return false;
    memcpy(newrom,data,size);
    if(base==QRX_FULL_ROM_MAIN_BASE){free(c->rom_main);c->rom_main=newrom;c->rom_main_size=size;}
    else {free(c->rom_ext);c->rom_ext=newrom;c->rom_ext_size=size;}
    return true;
}
void qrx_full_set_rom_overlay(QRXFullCPU *c,bool enabled){if(c)c->rom_overlay=enabled;}
void qrx_full_set_host(QRXFullCPU *c,QRXConsoleFn out,QRXClockFn ms,void *arg){if(c)qrx_vm_set_host(&c->vm,out,ms,arg);}
bool qrx_full_reset(QRXFullCPU *c){
    if (!c || active!=c) return false;
    uint32_t sp=0,pc=0;
    if(!qrx_full_bus_read(c,0,4,&sp)||!qrx_full_bus_read(c,4,4,&pc)||
       (sp&1u)||(pc&1u)||!sp||!pc) return false;
    c->mem_fault=false;c->fault_address=0;c->instructions=0;c->cycles=0;
    c->vm.cpu.halted=0;c->vm.cpu.fault=0;
    c->irq_level=c->irq_pending=0;
    c->timer_control=c->timer_period=c->timer_remaining=0;
    m68k_set_irq(0);
    m68k_pulse_reset();
    c->reset_done=true;
    return !c->mem_fault;
}
void qrx_full_irq(QRXFullCPU *c,unsigned level){
    if(!c || c!=active || level>7)return;
    c->irq_level=(uint8_t)level;
    m68k_set_irq(c->irq_pending && level<2 ? 2u : level);
}
static void timer_tick(QRXFullCPU *c,unsigned cycles){
    if(!(c->timer_control&QRX_FULL_TIMER_ENABLE)||!c->timer_period||c->irq_pending)return;
    if(cycles < c->timer_remaining) {c->timer_remaining-=cycles;return;}
    c->irq_pending=1;c->timer_remaining=c->timer_period;
    if(!(c->timer_control&QRX_FULL_TIMER_AUTO))c->timer_control&=~QRX_FULL_TIMER_ENABLE;
    m68k_set_irq(c->irq_level>2 ? c->irq_level : 2);
}
int qrx_full_run_cycles(QRXFullCPU *c,uint64_t budget){
    if(!c||active!=c||!c->reset_done||!budget)return -1;
    uint64_t elapsed=0;
    while(elapsed<budget && !c->vm.cpu.halted && !c->mem_fault) {
        int slice=(int)((budget-elapsed)>256?256:(budget-elapsed));
        int taken=m68k_execute(slice);
        if(taken<0) {c->mem_fault=true;break;}
        if(taken==0) return 2; /* STOP with no active IRQ, avoid busy-spin */
        elapsed+=(unsigned)taken;c->cycles+=(unsigned)taken;
        if(c->aux_cpu_index==0)timer_tick(c,(unsigned)taken);
        if(elapsed>budget+4096u)break; /* instruction granularity overshoot guard */
    }
    if(c->mem_fault)return -1;
    return c->vm.cpu.halted ? 0 : 1;
}
bool qrx_full_run_subroutine(QRXFullCPU *c,uint32_t entry,
                             uint64_t cycle_budget,uint64_t *retired){
    /* The firmware's guest stack and return trampoline are not host pointers.
       The sentinel is inside the reserved boot region above the loaded app. */
    const uint32_t sentinel=0x1e000u;
    if(!c||active!=c||!c->reset_done||c->subroutine_armed||
       !cycle_budget||cycle_budget>100000000u||entry&1u||
       (uint64_t)entry+2>c->vm.ram.size||
       (uint64_t)sentinel+2>c->vm.ram.size)return false;
    uint32_t oldpc=m68k_get_reg(NULL,M68K_REG_PC);
    uint32_t oldsp=m68k_get_reg(NULL,M68K_REG_A7);
    if(oldsp<4u||(oldsp&1u)||oldsp>c->vm.ram.size)return false;
    uint32_t saved_sentinel=0;
    if(!qrx_full_bus_read(c,sentinel,2,&saved_sentinel)||
       !qrx_full_bus_write(c,sentinel,2,0x4e71u)||
       !qrx_full_bus_write(c,oldsp-4u,4,sentinel))return false;
    const uint64_t start=c->instructions;
    c->subroutine_sentinel=sentinel;
    c->subroutine_returned=false;
    c->subroutine_armed=true;
    m68k_set_reg(M68K_REG_A7,oldsp-4u);
    m68k_set_reg(M68K_REG_PC,entry);
    uint64_t consumed=0;
    while(consumed<cycle_budget && !c->mem_fault && !c->subroutine_returned){
        int sl=(int)((cycle_budget-consumed)>128u?128u:cycle_budget-consumed);
        int got=qrx_full_run_cycles(c,(unsigned)sl);
        if(got<0||got==2)break;
        consumed+=(unsigned)sl;
    }
    bool ok=c->subroutine_returned&&!c->mem_fault;
    if(retired)*retired=c->instructions-start;
    c->subroutine_armed=false;
    m68k_set_reg(M68K_REG_PC,oldpc);
    m68k_set_reg(M68K_REG_A7,oldsp);
    (void)qrx_full_bus_write(c,sentinel,2,saved_sentinel);
    return ok;
}
uint32_t qrx_full_pc(const QRXFullCPU *c){return c&&c==active?m68k_get_reg(NULL,M68K_REG_PC):0;}
uint32_t qrx_full_reg(const QRXFullCPU *c,unsigned r){return c&&c==active?m68k_get_reg(NULL,(m68k_register_t)r):0;}

void qrx_full_set_aux(QRXFullCPU *c, QRXAuxReadFn rd, QRXAuxWriteFn wr,
                      QRXRamWriteFn ram, void *opaque) {
    if (!c) return;
    c->aux_read=rd; c->aux_write=wr; c->ram_write=ram; c->aux_opaque=opaque;
    c->vm.dma_ram_write=ram; c->vm.dma_opaque=opaque;
}
