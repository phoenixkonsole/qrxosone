/* ARMITHLON 1.6 parallel QRX68100 JIT, restricted and fail-closed.
 * Workers execute self-contained guest ALU loops, not Musashi itself.
 * JIT emits checked AArch64 instructions and uses W^X / MAP_JIT.
 * Original ARMITHLON project code; see the repository license terms.
 */
#include "qrx68100_parallel.h"
#include "qrx68100_vampire.h"
#include "m68k.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(__aarch64__)
# include <sys/mman.h>
# include <unistd.h>
# if defined(__APPLE__)
#  include <libkern/OSCacheControl.h>
# endif
#endif

typedef struct {
    uint32_t value;
    uint32_t iterations;
    uint32_t result;
} QRXNativeArgs;
typedef void (*NativeFn)(QRXNativeArgs *);
typedef struct {
    uint32_t pc, page_epoch, global_epoch;
    unsigned reg, increment, subtract;
    void *code;
    size_t mapped_size;
    int valid;
} QRXNativeEntry;
struct QRXFastCache { QRXNativeEntry cpu[2]; };
typedef struct {
    QRXNativeEntry *entry;
    QRXNativeArgs args;
    unsigned step, subtract;
    int use_jit;
} Work;

static void free_entry(QRXNativeEntry *entry) {
    if (!entry) return;
#if defined(__aarch64__)
    if (entry->code && entry->mapped_size) munmap(entry->code,entry->mapped_size);
#endif
    memset(entry,0,sizeof(*entry));
}
void qrx_dual_parallel_release(QRXDual *d) {
    if (!d || !d->fast_cache) return;
    for (unsigned c=0;c<2;c++) free_entry(&d->fast_cache->cpu[c]);
    free(d->fast_cache);
    d->fast_cache=NULL;
}
int qrx_dual_native_supported(void) {
#if defined(__aarch64__)
    return 1;
#else
    return 0;
#endif
}

#if defined(__aarch64__)
/* Arguments: x0 points to QRXNativeArgs; w9 value, w10 loop count,
 * w11 decoded ADDQ increment. Each virtual ADDQ is one native ADD;
 * B.NE controls the counted branch. w9 is committed only at exit.
 *  0 ldr w9,[x0,#0]
 *  1 ldr w10,[x0,#4]
 *  2 movz w11,#step
 *  3 add/sub w9,w9,w11
 *  4 subs w10,w10,#1
 *  5 b.ne 3
 *  6 str w9,[x0,#8]
 *  7 ret
 */
static int compile_entry(QRXNativeEntry *entry,unsigned step,unsigned subtract) {
    long n=sysconf(_SC_PAGESIZE);
    if(n<=0)return 0;
    size_t cap=(size_t)n;
    int flags=MAP_PRIVATE;
# if defined(MAP_ANON)
    flags|=MAP_ANON;
# else
    flags|=MAP_ANONYMOUS;
# endif
# if defined(__APPLE__)
    flags|=MAP_JIT;
    void *p=mmap(NULL,cap,PROT_READ|PROT_WRITE|PROT_EXEC,flags,-1,0);
# else
    void *p=mmap(NULL,cap,PROT_READ|PROT_WRITE,flags,-1,0);
# endif
    if(p==MAP_FAILED)return 0;
    const uint32_t code[] = {
        0xb9400009u,0xb940040au,0x5280000bu|((step&0xffffu)<<5),
        (subtract?0x4b0b0129u:0x0b0b0129u),0x7100054au,0x54ffffc1u,0xb9000809u,0xd65f03c0u
    };
# if defined(__APPLE__)
    pthread_jit_write_protect_np(0);
# endif
    memcpy(p,code,sizeof(code));
# if defined(__APPLE__)
    sys_icache_invalidate(p,sizeof(code));
    pthread_jit_write_protect_np(1);
# else
    __builtin___clear_cache((char*)p,(char*)p+sizeof(code));
    if(mprotect(p,cap,PROT_READ|PROT_EXEC)!=0){munmap(p,cap);return 0;}
# endif
    entry->code=p; entry->mapped_size=cap;
    return 1;
}
#endif
static void *worker(void *opaque) {
    Work *w=(Work*)opaque;
#if defined(__aarch64__)
    if(w->use_jit) {
        /* The function pointer conversion uses the AAPCS64 ABI, documented
         * by the host platform; ISO C does not otherwise define this cast. */
        NativeFn fn=NULL;
        memcpy(&fn,&w->entry->code,sizeof(fn));
        fn(&w->args);
        return NULL;
    }
#endif
    uint32_t val=w->args.value;
    /* The fallback performs the same bounded loop, NOT a fake FPS score. */
    for(uint32_t i=0;i<w->args.iterations;i++)
        val=w->subtract? val-w->step : val+w->step;
    w->args.result=val;
    return NULL;
}
static int decode(QRXDual *d,unsigned core,QRXNativeEntry *entry){
    uint32_t pc=qrx_dual_pc(d,core),add,bra;
    if(pc&1u || (uint64_t)pc+4u>d->board->vm.ram.size ||
       pc/QRX_DUAL_PAGE_SIZE!=(pc+3u)/QRX_DUAL_PAGE_SIZE)return 0;
    if(!qrx_vamp_cached_read(d,core,pc,2,true,&add) ||
       !qrx_vamp_cached_read(d,core,pc+2,2,true,&bra))return 0;
    /* ADDQ.L or SUBQ.L #1..8,D0..D7 followed by BRA.S -4, no RAM access,
     * exceptions, I/O, or processor-state observation within loop. */
    if((add&0xf0f8u)!=0x5080u || bra!=0x60fcu)return 0;
    entry->pc=pc;
    entry->reg=add&7u;
    entry->subtract=(add&0x0100u)!=0u;
    entry->increment=(add>>9)&7u;
    if(!entry->increment)entry->increment=8;
    entry->page_epoch=d->page_epochs[pc/QRX_DUAL_PAGE_SIZE];
    entry->global_epoch=d->global_generation;
    return 1;
}
static uint16_t flags_after(uint16_t sr,uint32_t oldval,uint32_t step,uint32_t count,unsigned subtract){
    /* Last op changes X/N/Z/V/C. Computation is 32-bit modular arithmetic. */
    uint32_t before=subtract ? oldval-(count-1u)*step : oldval+(count-1u)*step;
    uint32_t after=subtract ? before-step : before+step;
    unsigned carry=subtract ? before<step : (uint64_t)before+(uint64_t)step>UINT32_MAX;
    unsigned overflow=subtract ? (((before^step)&(before^after))>>31)&1u
                               : ((~(before^step)&(before^after))>>31)&1u;
    uint16_t ccr=(uint16_t)((carry?0x11u:0u) | (after&0x80000000u?8u:0u)
                    | (after==0u?4u:0u) | (overflow?2u:0u));
    return (uint16_t)((sr&~0x1fu)|ccr);
}
int qrx_dual_parallel_loops(QRXDual *d,uint32_t iterations) {
    if(!d || !d->initialized || !d->board || !iterations) return -1;
    if(d->mode!=QRX_BOOT_QRX_DUAL) return 0;
    if(!qrx_dual_commit(d) || !d->core[1].online || d->board->mem_fault ||
       d->board->vm.cpu.halted || d->core[0].ipi_pending || d->core[1].ipi_pending ||
       d->board->irq_pending || d->board->irq_level ||
       (d->board->timer_control & QRX_FULL_TIMER_ENABLE)) return 0;
    /* No guest memory/IO is touched by worker threads. The API is called on
     * the owner thread, with all external device/DMA writes quiescent. */
    if(!d->fast_cache){
        d->fast_cache=calloc(1,sizeof(*d->fast_cache));
        if(!d->fast_cache)return -1;
    }
    QRXNativeEntry fresh[2]={{0}};
    unsigned reg[2],step[2],subtract[2];uint32_t value[2];uint16_t sr[2];
    for(unsigned c=0;c<2;c++){
        /* Decode into scratch first. Ineligible code MUST leave state alone. */
        if(!decode(d,c,&fresh[c]))return 0;
        reg[c]=fresh[c].reg;step[c]=fresh[c].increment;subtract[c]=fresh[c].subtract;
        value[c]=qrx_dual_reg(d,c,(unsigned)(M68K_REG_D0+reg[c]));
        sr[c]=(uint16_t)qrx_dual_reg(d,c,M68K_REG_SR);
    }
    for(unsigned c=0;c<2;c++){
        QRXNativeEntry *e=&d->fast_cache->cpu[c];
        if(!e->valid || e->pc!=fresh[c].pc || e->reg!=fresh[c].reg ||
           e->increment!=fresh[c].increment || e->subtract!=fresh[c].subtract || e->page_epoch!=fresh[c].page_epoch ||
           e->global_epoch!=fresh[c].global_epoch){
            free_entry(e);
            *e=fresh[c];
#if defined(__aarch64__)
            if(!compile_entry(e,e->increment,e->subtract)) return -1;
#endif
            e->valid=1;
        }
    }
    Work job[2];pthread_t threads[2];unsigned started=0;
    memset(job,0,sizeof(job));
    for(unsigned c=0;c<2;c++){
        job[c].entry=&d->fast_cache->cpu[c];
        job[c].args=(QRXNativeArgs){.value=value[c],.iterations=iterations,.result=0};
        job[c].step=step[c];job[c].subtract=subtract[c];
        job[c].use_jit=qrx_dual_native_supported();
    }
    /* Threads are launched before joining, so real simultaneous execution is
     * possible. On pthread creation error, join existing workers and return
     * without committing any guest register changes. */
    for(unsigned c=0;c<2;c++){
        if(pthread_create(&threads[c],NULL,worker,&job[c])!=0){
            for(unsigned j=0;j<started;j++)pthread_join(threads[j],NULL);
            return -1;
        }
        started++;
    }
    for(unsigned c=0;c<2;c++) pthread_join(threads[c],NULL);
    /* Atomic guest-state commit occurs on the owner thread AFTER both join.
     * Musashi's global state never runs on the worker threads. */
    for(unsigned c=0;c<2;c++){
        unsigned prev=d->loaded;
        /* qrx_dual_reg loads a context but does not allow writes; we use
         * the same Musashi context API and then restore the previous core. */
        if(d->loaded!=c){
            m68k_get_context(d->core[d->loaded].musashi_context);
            m68k_set_context(d->core[c].musashi_context);
            d->loaded=c;d->board->aux_cpu_index=c;
        }
        m68k_set_reg((m68k_register_t)(M68K_REG_D0+reg[c]),job[c].args.result);
        m68k_set_reg(M68K_REG_SR,flags_after(sr[c],value[c],step[c],iterations,subtract[c]));
        m68k_get_context(d->core[c].musashi_context);
        if(prev!=c){
            m68k_set_context(d->core[prev].musashi_context);
            d->loaded=prev;d->board->aux_cpu_index=prev;
        }
        d->core[c].instructions+=(uint64_t)iterations*2u;
        d->core[c].parallel_iterations+=iterations;
        d->core[c].native_jit_runs+=job[c].use_jit?1u:0u;
    }
    d->board->instructions+=(uint64_t)iterations*4u;
    /* One branch history sample per finished compiled block, not a fabricated
     * prediction count for unobserved individual guest iterations. */
    for(unsigned c=0;c<2;c++)qrx_vamp_branch(d,c,fresh[c].pc+2,true);
    /* No fake cycle count is added. This is an optimized logical execution
     * of complete, branch-stable 68k loops, NOT a calibrated 68040 clock. */
    return qrx_dual_native_supported()?1:2;
}
