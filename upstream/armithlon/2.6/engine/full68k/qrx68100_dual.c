#include "qrx68100_dual.h"
#include "qrx68100_parallel.h"
#include "qrx68100_vampire.h"
#include "m68k.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define DUAL_SIGNATURE UINT32_C(0x51525844)
/* Musashi's frontend has global callback state. The QRX board already only
 * permits one bound instance. A dedicated secondary callback preserves the
 * host instruction counter when a blank, independent CPU context is booted. */
static QRXDual *bound_dual;
static void dual_instr_hook(unsigned pc) {
    (void)pc;
    if(bound_dual && bound_dual->board)bound_dual->board->instructions++;
}
static int dual_int_ack(int level) {
    (void)level;
    return (int)M68K_INT_ACK_AUTOVECTOR;
}
static bool legal_isa(uint8_t isa) { return isa>=QRX_DUAL_ISA_68K && isa<=QRX_DUAL_ISA_ARM; }
static uint32_t slot_hash(uint32_t pc,uint32_t asid,uint8_t isa) {
    uint32_t v=(pc>>1)^ (asid*UINT32_C(0x9e3779b9)) ^ ((uint32_t)isa*UINT32_C(0x85ebca6b));
    v ^= v>>16;
    return (v & ((QRX_DUAL_CACHE_SLOTS / QRX_DUAL_CACHE_WAYS)-1u))*QRX_DUAL_CACHE_WAYS;
}
static bool bound(const QRXDual *d,uint32_t addr,unsigned len) {
    return d && d->board && len && (uint64_t)addr+len <= d->board->vm.ram.size;
}
static void switch_core(QRXDual *d,unsigned to) {
    if(d->loaded==to)return;
    m68k_get_context(d->core[d->loaded].musashi_context);
    m68k_set_context(d->core[to].musashi_context);
    d->loaded=to;d->board->aux_cpu_index=to;
    /* IRQs belong to CPU contexts, never to the common bus. */
    unsigned irq=(to==0?d->board->irq_level:0u);
    if(to==0 && d->board->irq_pending && irq<2)irq=2;
    if(d->core[to].ipi_pending && irq<2)irq=2;
    m68k_set_irq(irq);
}
static bool valid_board(QRXDual *d) {return d && d->initialized && d->board && d->board->reset_done;}
static bool aux_read(void *arg,uint32_t addr,unsigned n,uint32_t *out) {
    QRXDual *d=arg;
    if(addr >= QRX_VAMP_BASE && addr < QRX_VAMP_BASE+0x100u)
        return qrx_vamp_mmio_read(d,addr,n,out);
    if(addr < QRX_DUAL_BASE || addr >= QRX_DUAL_BASE+0x40u) return false;
    if(!d || d->mode!=QRX_BOOT_QRX_DUAL || n!=4 || (addr&3u)) return false;
    uint64_t hits=d->core[0].jit_hits+d->core[1].jit_hits;
    uint64_t miss=d->core[0].jit_misses+d->core[1].jit_misses;
    switch(addr) {
    case QRX_DUAL_MAGIC:*out=DUAL_SIGNATURE;return true;
    case QRX_DUAL_ABI:*out=0x00010000u;return true;
    case QRX_DUAL_FEATURES:*out=QRX_DUAL_FEATURE_DUAL|QRX_DUAL_FEATURE_IPI|QRX_DUAL_FEATURE_CACHE_COHERENCE|QRX_DUAL_FEATURE_ISA_TAGGED_BLOCK_CACHE;return true;
    case QRX_DUAL_STATUS:*out=1u | (d->core[1].online?2u:0u);return true;
    case QRX_DUAL_CORE1_PC:*out=d->core1_pc;return true;
    case QRX_DUAL_CORE1_SP:*out=d->core1_sp;return true;
    case QRX_DUAL_CORE1_CTL:*out=d->boot_requested?1u:(d->park_requested?2u:0u);return true;
    case QRX_DUAL_IPI_SET:*out=(d->core[0].ipi_pending?1u:0u)|(d->core[1].ipi_pending?2u:0u);return true;
    case QRX_DUAL_IPI_ACK:*out=(d->core[d->loaded].ipi_pending?1u:0u);return true;
    case QRX_DUAL_CODE_GEN:*out=d->code_generation;return true;
    case QRX_DUAL_JIT_HITS:*out=(uint32_t)hits;return true;
    case QRX_DUAL_JIT_MISS:*out=(uint32_t)miss;return true;
    case QRX_DUAL_CACHE_FLUSH:*out=d->global_generation;return true;
    case QRX_DUAL_CORE_ID:*out=d->loaded;return true;
    default:return false;
    }
}
void qrx_dual_jit_flush(QRXDual *d) {
    if(!d || !d->initialized)return;
    /* A zero generation is reserved; wrap invalidates by explicit clearing. */
    if(++d->global_generation==0) {
        d->global_generation=1;
        for(unsigned i=0;i<2;i++)memset(d->core[i].blocks,0,sizeof(d->core[i].blocks));
    }
}
void qrx_dual_notify_ram_write(QRXDual *d,uint32_t addr,unsigned width) {
    if(!d || !d->initialized || !bound(d,addr,width))return;
    qrx_vamp_invalidate(d,addr,width);
    size_t p0=addr/QRX_DUAL_PAGE_SIZE, p1=(addr+width-1u)/QRX_DUAL_PAGE_SIZE;
    for(size_t p=p0;p<=p1;p++){
        if(++d->page_epochs[p]==0) { /* wrap: force global epoch; avoid false hits */
            d->page_epochs[p]=1;qrx_dual_jit_flush(d);
        }
    }
    if(++d->code_generation==0) { d->code_generation=1;qrx_dual_jit_flush(d); }
}
static void ram_write(void *arg,uint32_t addr,unsigned n){qrx_dual_notify_ram_write((QRXDual*)arg,addr,n);}
static bool aux_write(void *arg,uint32_t addr,unsigned n,uint32_t val) {
    QRXDual *d=arg;
    if(addr >= QRX_VAMP_BASE && addr < QRX_VAMP_BASE+0x100u)
        return qrx_vamp_mmio_write(d,addr,n,val);
    if(addr < QRX_DUAL_BASE || addr >= QRX_DUAL_BASE+0x40u) return false;
    if(!d || d->mode!=QRX_BOOT_QRX_DUAL || n!=4 || (addr&3u)) return false;
    /* Hardware service control is kernel/supervisor-only. Guest userspace
     * must not be allowed to boot a second CPU or flush the code cache. */
    if(!(m68k_get_reg(NULL,M68K_REG_SR)&0x2000u)) return false;
    switch(addr) {
    case QRX_DUAL_CORE1_PC:d->core1_pc=val;return true;
    case QRX_DUAL_CORE1_SP:d->core1_sp=val;return true;
    case QRX_DUAL_CORE1_CTL:
        if(val==1u && !d->core[1].online) d->boot_requested=true;
        else if(val==2u) d->park_requested=true;
        return val==1u || val==2u;
    case QRX_DUAL_IPI_SET:
        if(val & ~3u)return false;
        for(unsigned i=0;i<2;i++)if(val&(1u<<i)){d->core[i].ipi_pending=1;d->core[i].idle=0;}
        /* Do not switch Musashi contexts inside an in-flight guest instruction. */
        return true;
    case QRX_DUAL_IPI_ACK:
        if(val!=1u)return false;
        d->core[d->loaded].ipi_pending=0;
        unsigned irq=d->loaded==0 ? d->board->irq_level : 0;
        if(d->loaded==0 && d->board->irq_pending && irq<2)irq=2;
        m68k_set_irq(irq);
        return true;
    case QRX_DUAL_CACHE_FLUSH:if(val!=1u)return false;qrx_dual_jit_flush(d);return true;
    default:return false;
    }
}
static void unhook(QRXDual *d) {
    if(d && d->board && d->board->aux_opaque==d)
        qrx_full_set_aux(d->board,NULL,NULL,NULL,NULL);
}
bool qrx_dual_init(QRXDual *d,QRXFullCPU *board) {
    if(!d || !board || board->cpu_type!=68040 || !board->reset_done || board->aux_opaque) return false;
    memset(d,0,sizeof(*d));
    d->board=board;
    d->page_count=(board->vm.ram.size+QRX_DUAL_PAGE_SIZE-1u)/QRX_DUAL_PAGE_SIZE;
    d->page_epochs=calloc(d->page_count,sizeof(uint32_t));
    if(!d->page_epochs)goto fail;
    unsigned bytes=m68k_context_size();
    if(bytes==0 || bytes>1u<<20)goto fail;
    for(unsigned i=0;i<2;i++)if(!(d->core[i].musashi_context=calloc(1,bytes)))goto fail;
    m68k_get_context(d->core[0].musashi_context);
    d->core[0].online=1;
    d->loaded=0;
    d->global_generation=1;
    d->code_generation=1;
    d->mode=QRX_BOOT_LEGACY_AMIGA;
    d->initialized=true;
    if(!qrx_vamp_init(d))goto fail;
    bound_dual=d;
    qrx_full_set_aux(board,aux_read,aux_write,ram_write,d);
    return true;
fail:
    for(unsigned i=0;i<2;i++)free(d->core[i].musashi_context);
    qrx_vamp_destroy(d);free(d->page_epochs);memset(d,0,sizeof(*d));return false;
}
void qrx_dual_reset(QRXDual *d) {
    if(!valid_board(d))return;
    if(d->loaded!=0)switch_core(d,0);
    m68k_get_context(d->core[0].musashi_context);
    d->core[0].online=1;
    d->core[0].idle=d->core[0].ipi_pending=0;
    d->core[0].cycles=d->core[0].instructions=0;
    d->core[1].online=d->core[1].idle=d->core[1].ipi_pending=0;
    d->core[1].cycles=d->core[1].instructions=0;
    d->boot_requested=d->park_requested=false;
    d->core1_pc=d->core1_sp=0;
    qrx_dual_jit_flush(d);
    qrx_vamp_reset(d);
}
void qrx_dual_destroy(QRXDual *d) {
    if(!d)return;
    if(d->initialized && d->loaded==1)switch_core(d,0);
    unhook(d);
    if(bound_dual==d)bound_dual=NULL;
    qrx_dual_parallel_release(d);
    qrx_vamp_destroy(d);
    for(unsigned i=0;i<2;i++)free(d->core[i].musashi_context);
    free(d->page_epochs);
    memset(d,0,sizeof(*d));
}
bool qrx_dual_set_mode(QRXDual *d,QRXDualMode mode) {
    if(!valid_board(d)||(mode!=QRX_BOOT_LEGACY_AMIGA && mode!=QRX_BOOT_QRX_DUAL))return false;
    if(d->loaded!=0)switch_core(d,0);
    d->mode=mode;
    if(mode==QRX_BOOT_LEGACY_AMIGA){
        d->core[1].online=d->core[1].ipi_pending=0;
        d->boot_requested=d->park_requested=false;
    }
    qrx_dual_jit_flush(d);
    qrx_vamp_reset(d);
    return true;
}
bool qrx_dual_commit(QRXDual *d) {
    if(!valid_board(d))return false;
    if(d->loaded!=0)switch_core(d,0);
    if(d->mode!=QRX_BOOT_QRX_DUAL)return true;
    if(d->park_requested){
        d->core[1].online=0;d->core[1].ipi_pending=0;
        d->park_requested=false;d->boot_requested=false;
        qrx_vamp_reset_core(d,1);
    }
    if(!d->boot_requested)return true;
    /* Start secondary from explicit QRX BSP entry, never AmigaOS reset vectors. */
    if(d->core[1].online || !d->core1_pc || !d->core1_sp ||
       (d->core1_pc&1u)||(d->core1_sp&1u)||
       !bound(d,d->core1_pc,2)||d->core1_sp>d->board->vm.ram.size) {
        d->boot_requested=false;return false;
    }
    m68k_get_context(d->core[0].musashi_context);
    /* Do NOT reset the current core0 context in place: Musashi reset keeps
     * several data/FPU registers intact. Boot from a zeroed independent
     * CPU context, with fresh Musashi callbacks and 68040-specific state. */
    memset(d->core[1].musashi_context,0,m68k_context_size());
    m68k_set_context(d->core[1].musashi_context);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    m68k_set_int_ack_callback(dual_int_ack);
    m68k_set_instr_hook_callback(dual_instr_hook);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_A7,d->core1_sp);
    m68k_set_reg(M68K_REG_PC,d->core1_pc);
    m68k_set_irq(0);
    m68k_get_context(d->core[1].musashi_context);
    m68k_set_context(d->core[0].musashi_context);
    d->core[1].cycles=d->core[1].instructions=0;
    qrx_vamp_reset_core(d,1);
    d->core[1].idle=0;d->core[1].online=1;
    d->boot_requested=false;
    return true;
}
int qrx_dual_run(QRXDual *d,uint64_t per_core_budget) {
    if(!valid_board(d)||!per_core_budget)return -1;
    if(!qrx_dual_commit(d))return -1;
    uint64_t spent[2]={0,0};
    bool ran=false;
    while(!d->board->vm.cpu.halted && !d->board->mem_fault) {
        bool need=false,progress=false;
        for(unsigned i=0;i<2;i++){
            if(i==1 && (d->mode==QRX_BOOT_LEGACY_AMIGA || !d->core[1].online))continue;
            if(spent[i]>=per_core_budget || d->core[i].idle)continue;
            need=true;
            switch_core(d,i);
            uint64_t instr_before=d->board->instructions,cycles_before=d->board->cycles;
            uint64_t quota=per_core_budget-spent[i];if(quota>256)quota=256;
            int rc=qrx_full_run_cycles(d->board,quota);
            uint64_t used=d->board->cycles-cycles_before;
            spent[i]+=used;d->core[i].cycles+=used;
            d->core[i].instructions+=d->board->instructions-instr_before;
            if(rc<0){switch_core(d,0);return -1;}
            if(rc==2){d->core[i].idle=1;}
            else {progress=true;ran=true;}
            /* Save the CPU which just ran before servicing pending boot/park. */
            m68k_get_context(d->core[i].musashi_context);
            if(d->boot_requested || d->park_requested){switch_core(d,0);if(!qrx_dual_commit(d))return -1;}
        }
        if(!need || !progress)break;
    }
    switch_core(d,0);
    if(d->board->mem_fault)return -1;
    if(d->board->vm.cpu.halted)return 0;
    return ran?1:2;
}
uint32_t qrx_dual_reg(QRXDual *d,unsigned cpu,unsigned reg) {
    if(!valid_board(d)||cpu>1 || (cpu==1&&!d->core[1].online))return 0;
    unsigned prev=d->loaded;
    switch_core(d,cpu);
    uint32_t val=m68k_get_reg(NULL,(m68k_register_t)reg);
    switch_core(d,prev);
    return val;
}
uint32_t qrx_dual_pc(QRXDual *d,unsigned cpu){return qrx_dual_reg(d,cpu,M68K_REG_PC);}
/* 4-way set associative, direct guest-page generation validation.  These
 * records identify translated blocks, not executable native machine code. */
bool qrx_dual_jit_insert(QRXDual *d,unsigned core,uint32_t asid,uint8_t isa,uint32_t pc,uint16_t len) {
    if(!valid_board(d)||core>1 || (core==1&&!d->core[1].online)||!legal_isa(isa)||
       len==0||len>128||!bound(d,pc,len)||pc/QRX_DUAL_PAGE_SIZE!=(pc+len-1u)/QRX_DUAL_PAGE_SIZE)return false;
    uint32_t set=slot_hash(pc,asid,isa),pagegen=d->page_epochs[pc/QRX_DUAL_PAGE_SIZE];
    QRXJITBlock *choice=NULL;
    for(unsigned way=0;way<QRX_DUAL_CACHE_WAYS;way++) {
        QRXJITBlock *b=&d->core[core].blocks[set+way];
        if(b->valid && b->pc==pc && b->asid==asid && b->isa==isa) {choice=b;break;}
        if(!b->valid || b->global_generation!=d->global_generation ||
           b->page_generation!=d->page_epochs[b->pc/QRX_DUAL_PAGE_SIZE]) {
            if(!choice)choice=b;
        } else if(!choice || (choice->valid && b->uses<choice->uses))choice=b;
    }
    if(!choice)return false;
    *choice=(QRXJITBlock){.pc=pc,.asid=asid,.isa=isa,.valid=1,.guest_bytes=len,
      .page_generation=pagegen,.global_generation=d->global_generation};
    return true;
}
bool qrx_dual_jit_lookup(QRXDual *d,unsigned core,uint32_t asid,uint8_t isa,uint32_t pc) {
    if(!valid_board(d)||core>1 || (core==1&&!d->core[1].online)||!legal_isa(isa)||!bound(d,pc,1))return false;
    uint32_t set=slot_hash(pc,asid,isa);
    for(unsigned way=0;way<QRX_DUAL_CACHE_WAYS;way++){
        QRXJITBlock *b=&d->core[core].blocks[set+way];
        if(b->valid && b->pc==pc && b->asid==asid && b->isa==isa &&
           b->global_generation==d->global_generation &&
           b->page_generation==d->page_epochs[pc/QRX_DUAL_PAGE_SIZE]){
            b->uses++;d->core[core].jit_hits++;return true;
        }
    }
    d->core[core].jit_misses++;return false;
}
