/* QRX68100 Vampire-inspired, independent non-68080-specific implementation.
 * No proprietary Apollo instruction encoding, no measured superscalar IPC.
 * Cache is a functional 64-byte-line coherent JIT frontend, not guest timing.
 */
#include "qrx68100_vampire.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>

#define IC_LINES (QRX_VAMP_ICACHE_BYTES / QRX_VAMP_LINE_BYTES)
#define DC_LINES (QRX_VAMP_DCACHE_BYTES / QRX_VAMP_LINE_BYTES)
#define CACHE_WAYS 4u
#define BP_ENTRIES 256u
typedef struct {
    uint32_t tag;
    uint64_t stamp;
    uint8_t valid, bytes[QRX_VAMP_LINE_BYTES];
} VLine;
typedef struct {
    VLine line[DC_LINES];
    uint64_t hits,misses,clock;
} VDataCache;
typedef struct {
    VLine line[IC_LINES];
    uint64_t hits,misses,clock;
} VInstCache;
typedef struct {
    uint64_t vector[QRX_VAMP_VEC_REGS];
    uint32_t control,sel,src,dst,status;
    uint8_t branch[BP_ENTRIES];
    uint64_t predicted, mispredicted;
    VDataCache dc;
    VInstCache ic;
} VCore;
struct QRXVampState {VCore cores[2];};
static VCore *vc(QRXDual*d,unsigned core) {
    if(!d||!d->vamp||!d->initialized||d->mode!=QRX_BOOT_QRX_DUAL||core>1 ||
        (core==1&&!d->core[1].online))return NULL;
    return &d->vamp->cores[core];
}
bool qrx_vamp_init(QRXDual *d) {
    if(!d||d->vamp)return false;
    d->vamp=calloc(1,sizeof(*d->vamp));
    if(!d->vamp)return false;
    qrx_vamp_reset(d);
    return true;
}
void qrx_vamp_destroy(QRXDual *d){if(!d)return;free(d->vamp);d->vamp=NULL;}
void qrx_vamp_reset_core(QRXDual *d,unsigned cpu){
    if(!d||!d->vamp||cpu>1)return;
    VCore*v=&d->vamp->cores[cpu];
    memset(v,0,sizeof(*v));
    v->control=QRX_VAMP_CTRL_MASK;
    memset(v->branch,1,BP_ENTRIES);
}
void qrx_vamp_reset(QRXDual *d){
    qrx_vamp_reset_core(d,0);
    qrx_vamp_reset_core(d,1);
}
/* Per-set, 4-way invalidation instead of scanning 16K+32K for every store. */
void qrx_vamp_invalidate(QRXDual *d,uint32_t addr,unsigned n){
    if(!d||!d->vamp||!n)return;
    uint64_t end=(uint64_t)addr+n;
    if(n==UINT32_MAX){
        for(unsigned c=0;c<2;c++){
            VCore*v=&d->vamp->cores[c];
            memset(v->ic.line,0,sizeof(v->ic.line));
            memset(v->dc.line,0,sizeof(v->dc.line));
        }
        return;
    }
    uint64_t first=(uint64_t)addr&~(uint64_t)(QRX_VAMP_LINE_BYTES-1);
    for(uint64_t tag=first;tag<end;tag+=QRX_VAMP_LINE_BYTES){
        for(unsigned cpu=0;cpu<2;cpu++){
            VCore*v=&d->vamp->cores[cpu];
            unsigned ii=(unsigned)((tag/QRX_VAMP_LINE_BYTES)% (IC_LINES/CACHE_WAYS))*CACHE_WAYS;
            unsigned di=(unsigned)((tag/QRX_VAMP_LINE_BYTES)% (DC_LINES/CACHE_WAYS))*CACHE_WAYS;
            for(unsigned i=0;i<CACHE_WAYS;i++){
                if(v->ic.line[ii+i].valid && v->ic.line[ii+i].tag==tag)v->ic.line[ii+i].valid=0;
                if(v->dc.line[di+i].valid && v->dc.line[di+i].tag==tag)v->dc.line[di+i].valid=0;
            }
        }
    }
}
static bool cache_read(QRXDual *d,VCore*v,uint32_t addr,unsigned width,bool ins,uint32_t*out){
    if(!d || !d->board || !out || (width!=1&&width!=2&&width!=4))return false;
    uint64_t end=(uint64_t)addr+width;
    if(end>d->board->vm.ram.size)return qrx_full_bus_read(d->board,addr,width,out);
    /* Reset vectors can be mapped to the ROM overlay, so bypass entirely. */
    if(d->board->rom_overlay && addr<8)return qrx_full_bus_read(d->board,addr,width,out);
    if(!(v->control&QRX_VAMP_CTRL_CACHE))return qrx_full_bus_read(d->board,addr,width,out);
    uint32_t tag=addr&~(QRX_VAMP_LINE_BYTES-1u);
    if(end>(uint64_t)tag+QRX_VAMP_LINE_BYTES || (uint64_t)tag+QRX_VAMP_LINE_BYTES>d->board->vm.ram.size)
        return qrx_full_bus_read(d->board,addr,width,out);
    VLine*line=ins?v->ic.line:v->dc.line;
    unsigned count=ins?IC_LINES:DC_LINES;
    uint64_t *hits=ins?&v->ic.hits:&v->dc.hits;
    uint64_t *misses=ins?&v->ic.misses:&v->dc.misses;
    uint64_t *clk=ins?&v->ic.clock:&v->dc.clock;
    unsigned sets=count/CACHE_WAYS;
    unsigned index=(tag/QRX_VAMP_LINE_BYTES)%sets*CACHE_WAYS;
    VLine*found=NULL,*victim=NULL;
    for(unsigned way=0;way<CACHE_WAYS;way++){
        VLine*l=&line[index+way];
        if(l->valid&&l->tag==tag){found=l;break;}
        if(!l->valid){if(!victim)victim=l;}
        else if(!victim || (victim->valid && l->stamp<victim->stamp))victim=l;
    }
    if(found){(*hits)++;}
    else{
        (*misses)++;
        found=victim;
        if(!found)return false;
        memcpy(found->bytes,d->board->ram_owned+tag,QRX_VAMP_LINE_BYTES);
        found->tag=tag;found->valid=1;
    }
    found->stamp=++*clk;
    uint32_t value=0;unsigned off=addr-tag;
    for(unsigned i=0;i<width;i++)value=(value<<8)|found->bytes[off+i];
    *out=value;
    return true;
}
bool qrx_vamp_cached_read(QRXDual*d,unsigned cpu,uint32_t a,unsigned n,bool instruction,uint32_t*out){
    VCore*v=vc(d,cpu);
    return v?cache_read(d,v,a,n,instruction,out):false;
}
bool qrx_vamp_prefetch(QRXDual*d,unsigned core,uint32_t a){
    VCore*v=vc(d,core);if(!v || (v->control&(QRX_VAMP_CTRL_PREFETCH|QRX_VAMP_CTRL_CACHE))!=
                  (QRX_VAMP_CTRL_PREFETCH|QRX_VAMP_CTRL_CACHE))return false;
    uint32_t tmp;
    return cache_read(d,v,a,1,true,&tmp);
}
static unsigned branch_index(uint32_t pc){return ((pc>>1)^(pc>>9))&(BP_ENTRIES-1u);}
bool qrx_vamp_predict(QRXDual*d,unsigned cpu,uint32_t pc){
    VCore*v=vc(d,cpu);return v && (v->control&QRX_VAMP_CTRL_BRPRED) && v->branch[branch_index(pc)]>=2;
}
void qrx_vamp_branch(QRXDual*d,unsigned cpu,uint32_t pc,bool taken){
    VCore*v=vc(d,cpu);if(!v || !(v->control&QRX_VAMP_CTRL_BRPRED))return;
    unsigned i=branch_index(pc);uint8_t p=v->branch[i];
    if((p>=2)==taken)v->predicted++;else v->mispredicted++;
    if(taken){if(p<3)p++;}else if(p>0)p--;
    v->branch[i]=p;
}
uint64_t qrx_vamp_vector(QRXDual*d,unsigned cpu,unsigned r){
    VCore*v=vc(d,cpu);return v && r<QRX_VAMP_VEC_REGS?v->vector[r]:0;
}
static uint64_t packed_op(uint64_t a,uint64_t b,unsigned lane,unsigned kind){
    unsigned bits=lane*8;
    uint64_t mask=(UINT64_C(1)<<bits)-1u;
    uint64_t out=0;
    for(unsigned i=0;i<64/bits;i++){
        unsigned shift=i*bits;
        uint64_t x=(a>>shift)&mask,y=(b>>shift)&mask,z=0;
        if(kind==QRX_VAMP_CMD_PADDB || kind==QRX_VAMP_CMD_PADDW)z=(x+y)&mask;
        else if(kind==QRX_VAMP_CMD_PSUBB)z=(x-y)&mask;
        else if(kind==QRX_VAMP_CMD_PAVGB)z=(x+y+1u)>>1;
        else if(kind==QRX_VAMP_CMD_PADDSB){z=x+y;if(z>mask)z=mask;}
        out|=z<<shift;
    }
    return out;
}
static void command(VCore*v,uint32_t cmd){
    uint64_t a=v->vector[v->sel],b=v->vector[v->src],r=0;
    switch(cmd){
    case QRX_VAMP_CMD_PADDB: r=packed_op(a,b,1,cmd);break;
    case QRX_VAMP_CMD_PADDW: r=packed_op(a,b,2,cmd);break;
    case QRX_VAMP_CMD_PSUBB: r=packed_op(a,b,1,cmd);break;
    case QRX_VAMP_CMD_PXOR:r=a^b;break;
    case QRX_VAMP_CMD_PAND:r=a&b;break;
    case QRX_VAMP_CMD_POR:r=a|b;break;
    case QRX_VAMP_CMD_MUL64:r=a*b;break; /* defined unsigned wrap */
    case QRX_VAMP_CMD_DIV64:
        if(!b){v->status=QRX_VAMP_STATUS_DIV_ZERO;return;}
        r=a/b;break;
    case QRX_VAMP_CMD_PAVGB:r=packed_op(a,b,1,cmd);break;
    case QRX_VAMP_CMD_PADDSB:r=packed_op(a,b,1,cmd);break;
    default:v->status=QRX_VAMP_STATUS_BAD_CMD;return;
    }
    v->vector[v->dst]=r;v->status=QRX_VAMP_STATUS_OK;
}
bool qrx_vamp_vector_write(QRXDual*d,unsigned cpu,unsigned r,uint64_t value){
    VCore*v=vc(d,cpu);if(!v || r>=QRX_VAMP_VEC_REGS)return false;
    v->vector[r]=value;return true;
}
unsigned qrx_vamp_vector_execute(QRXDual*d,unsigned cpu,unsigned a,unsigned b,unsigned dst,unsigned cmd){
    VCore*v=vc(d,cpu);if(!v||a>=QRX_VAMP_VEC_REGS||b>=QRX_VAMP_VEC_REGS||dst>=QRX_VAMP_VEC_REGS)
        return QRX_VAMP_STATUS_BAD_CMD;
    v->sel=a;v->src=b;v->dst=dst;
    command(v,cmd);
    return v->status;
}
static bool inrange(uint32_t a){return a>=QRX_VAMP_BASE&&a<QRX_VAMP_BASE+0x100u;}
bool qrx_vamp_mmio_read(QRXDual*d,uint32_t a,unsigned n,uint32_t*out){
    if(!out||!inrange(a)||n!=4||(a&3))return false;
    VCore*v=vc(d,d?d->loaded:0);if(!v)return false;
    switch(a){
    case QRX_VAMP_MAGIC:*out=UINT32_C(0x51563137);break;
    case QRX_VAMP_ABI:*out=0x00010000u;break;
    case QRX_VAMP_FEATURES:*out=QRX_VAMP_CAPABILITIES;break;
    case QRX_VAMP_CPU_ID:*out=0x00068100u;break;
    case QRX_VAMP_CTRL:*out=v->control;break;
    case QRX_VAMP_IC_BYTES:*out=QRX_VAMP_ICACHE_BYTES;break;
    case QRX_VAMP_DC_BYTES:*out=QRX_VAMP_DCACHE_BYTES;break;
    case QRX_VAMP_L2_BYTES:*out=0;break;
    case QRX_VAMP_VSEL:*out=v->sel;break;
    case QRX_VAMP_VSRC:*out=v->src;break;
    case QRX_VAMP_VDST:*out=v->dst;break;
    case QRX_VAMP_VLO:*out=(uint32_t)v->vector[v->sel];break;
    case QRX_VAMP_VHI:*out=(uint32_t)(v->vector[v->sel]>>32);break;
    case QRX_VAMP_VCMD:*out=0;break;
    case QRX_VAMP_VSTATUS:*out=v->status;break;
    case QRX_VAMP_IC_HITS:*out=(uint32_t)v->ic.hits;break;
    case QRX_VAMP_IC_MISSES:*out=(uint32_t)v->ic.misses;break;
    case QRX_VAMP_DC_HITS:*out=(uint32_t)v->dc.hits;break;
    case QRX_VAMP_DC_MISSES:*out=(uint32_t)v->dc.misses;break;
    case QRX_VAMP_BP_GOOD:*out=(uint32_t)v->predicted;break;
    case QRX_VAMP_BP_BAD:*out=(uint32_t)v->mispredicted;break;
    default:return false;
    }
    return true;
}
bool qrx_vamp_mmio_write(QRXDual*d,uint32_t a,unsigned n,uint32_t val){
    if(!inrange(a)||n!=4||(a&3))return false;
    VCore*v=vc(d,d?d->loaded:0);if(!v)return false;
    switch(a){
    case QRX_VAMP_CTRL:
        /* Supervisor-only, like control register programming on 68040. */
        if(!(m68k_get_reg(NULL,M68K_REG_SR)&0x2000u)|| (val&~QRX_VAMP_CTRL_MASK))return false;
        if((v->control^val)&QRX_VAMP_CTRL_CACHE){
            memset(&v->ic,0,sizeof(v->ic));memset(&v->dc,0,sizeof(v->dc));
        }
        v->control=val;return true;
    case QRX_VAMP_CACHE_INV:
        if(val!=1u || !(m68k_get_reg(NULL,M68K_REG_SR)&0x2000u))return false;
        qrx_vamp_invalidate(d,0,UINT32_MAX);return true;
    case QRX_VAMP_VSEL:if(val>=QRX_VAMP_VEC_REGS)return false;v->sel=val;return true;
    case QRX_VAMP_VSRC:if(val>=QRX_VAMP_VEC_REGS)return false;v->src=val;return true;
    case QRX_VAMP_VDST:if(val>=QRX_VAMP_VEC_REGS)return false;v->dst=val;return true;
    case QRX_VAMP_VLO:v->vector[v->sel]=(v->vector[v->sel]&UINT64_C(0xffffffff00000000))|val;return true;
    case QRX_VAMP_VHI:v->vector[v->sel]=(v->vector[v->sel]&UINT64_C(0xffffffff))|((uint64_t)val<<32);return true;
    case QRX_VAMP_VCMD:command(v,val);return true;
    default:return false;
    }
}
