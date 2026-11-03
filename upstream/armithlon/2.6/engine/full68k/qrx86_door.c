#include "qrx86_door.h"
/* The host build of the same interpreter the guest runs, with its internals in
 * reach: the door keeps a shadow page table of host pointers. */
#include "qrx86core.c"

/* struct door_req on the guest side; all fields big-endian. */
enum {
    REQ_R=0, REQ_EIP=32, REQ_EFLAGS=36, REQ_SREG=40, REQ_SBASE=64, REQ_TLS=88,
    REQ_ST=124, REQ_TOP=188, REQ_FSW=192, REQ_FCW=196, REQ_FTW=200,
    REQ_DIR=204, REQ_SERIAL=208, REQ_HLE_BASE=212, REQ_HLE_COUNT=216,
    REQ_STOP=220, REQ_LIMIT=224, REQ_EXECUTED=228, REQ_STATUS=232, REQ_SIZE=236
};
static x86cpu host;
static uint32_t shadow_serial, shadow_dir;
static bool shadow_valid;
static uint64_t door_calls, door_x86;

static inline uint32_t rd32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return __builtin_bswap32(v);}
static inline void wr32(uint8_t *p,uint32_t x){uint32_t v=__builtin_bswap32(x);memcpy(p,&v,4);}
static inline double rd_double(const uint8_t *p){uint64_t v;double d;memcpy(&v,p,8);v=__builtin_bswap64(v);memcpy(&d,&v,8);return d;}
static inline void wr_double(uint8_t *p,double d){uint64_t v;memcpy(&v,&d,8);v=__builtin_bswap64(v);memcpy(p,&v,8);}

/* Guest x86page is { uint8_t *mem; uint32_t flags; }, 8 bytes on the 68k. */
static void shadow_rebuild(QRXFullCPU *c,uint32_t dir){
    const uint8_t *ram=c->ram_owned;
    size_t size=c->vm.ram.size;
    for(unsigned i=0;i<1024;i++){
        uint32_t gt=rd32(ram+dir+i*4u);
        x86page *t=host.dir[i];
        if(!gt){
            if(t){free(t);host.dir[i]=NULL;}
            continue;
        }
        if((uint64_t)gt+1024u*8u>size)continue;
        if(!t){t=calloc(1024,sizeof(x86page));host.dir[i]=t;if(!t)continue;}
        for(unsigned j=0;j<1024;j++){
            uint32_t mem=rd32(ram+gt+j*8u);
            t[j].flags=rd32(ram+gt+j*8u+4u);
            t[j].mem=(mem&&(uint64_t)mem+X86_PAGE_SIZE<=size)?(uint8_t*)ram+mem:NULL;
        }
    }
    tlb_flush(&host);
    shadow_dir=dir;
    shadow_valid=true;
}
/* Anything the guest must decide stops the run with eip on that instruction;
 * the guest interprets it, so its behaviour stays in exactly one place. */
static int door_hle_hook(x86cpu *h,uint32_t index){(void)index;h->stop=1;return 1;}
static int door_int_hook(x86cpu *h,int vector){(void)vector;h->eip=h->cur_eip;h->stop=1;return 1;}

static void door_run(QRXFullCPU *c,uint32_t req){
    if((uint64_t)req+REQ_SIZE>c->vm.ram.size)return;
    uint8_t *q=c->ram_owned+req;
    uint32_t dir=rd32(q+REQ_DIR),serial=rd32(q+REQ_SERIAL);
    if((uint64_t)dir+4096u>c->vm.ram.size)return;
    if(!shadow_valid||dir!=shadow_dir||serial!=shadow_serial){
        shadow_serial=serial;
        shadow_rebuild(c,dir);
    }
    for(unsigned i=0;i<8;i++){host.r[i]=rd32(q+REQ_R+i*4u);host.st[i]=rd_double(q+REQ_ST+i*8u);}
    host.eip=rd32(q+REQ_EIP);
    x86_set_flags(&host,rd32(q+REQ_EFLAGS));
    for(unsigned i=0;i<6;i++){host.sreg[i]=(uint16_t)rd32(q+REQ_SREG+i*4u);host.sbase[i]=rd32(q+REQ_SBASE+i*4u);}
    for(unsigned i=0;i<3;i++){
        host.tls[i].base=rd32(q+REQ_TLS+i*12u);host.tls[i].limit=rd32(q+REQ_TLS+i*12u+4u);host.tls[i].used=(int)rd32(q+REQ_TLS+i*12u+8u);
    }
    host.top=(int)rd32(q+REQ_TOP);host.fsw=(uint16_t)rd32(q+REQ_FSW);
    host.fcw=(uint16_t)rd32(q+REQ_FCW);host.ftw=(uint16_t)rd32(q+REQ_FTW);
    host.hle_base=rd32(q+REQ_HLE_BASE);host.hle_count=rd32(q+REQ_HLE_COUNT);
    host.hle_hook=door_hle_hook;host.int_hook=door_int_hook;
    uint32_t limit=rd32(q+REQ_LIMIT);
    host.fault=X86_OK;host.stop=0;
    host.icount=0;host.left=limit;host.lbase=limit;host.budget=limit;
    (void)x86_run(&host,rd32(q+REQ_STOP));
    uint32_t executed=limit-host.left;
    /* A fault or stop charged the instruction it did not finish. */
    if(host.fault||host.stop){
        if(executed)executed--;
        if(host.fault&&host.fault!=X86_FAULT_BUDGET)host.eip=host.fault_eip;
        else if(host.fault==X86_FAULT_BUDGET)executed=limit;
    }
    for(unsigned i=0;i<8;i++){wr32(q+REQ_R+i*4u,host.r[i]);wr_double(q+REQ_ST+i*8u,host.st[i]);}
    wr32(q+REQ_EIP,host.eip);
    wr32(q+REQ_EFLAGS,x86_get_flags(&host));
    for(unsigned i=0;i<6;i++){wr32(q+REQ_SREG+i*4u,host.sreg[i]);wr32(q+REQ_SBASE+i*4u,host.sbase[i]);}
    wr32(q+REQ_TOP,(uint32_t)host.top);wr32(q+REQ_FSW,host.fsw);wr32(q+REQ_FCW,host.fcw);wr32(q+REQ_FTW,host.ftw);
    wr32(q+REQ_EXECUTED,executed);
    wr32(q+REQ_STATUS,(uint32_t)host.fault);
    door_calls++;door_x86+=executed;
}
static bool door_read(QRXFullCPU *c,uint32_t addr,unsigned n,uint32_t *out){
    (void)c;
    if(addr==QRX86_DOOR_BASE&&n==4){*out=QRX86_DOOR_MAGIC;return true;}
    return false;
}
static bool door_write(QRXFullCPU *c,uint32_t addr,unsigned n,uint32_t value){
    if(addr==QRX86_DOOR_BASE+4u&&n==4){door_run(c,value);return true;}
    return false;
}
void qrx86_door_attach(QRXFullCPU *c){
    x86_init(&host);
    c->door_read=door_read;c->door_write=door_write;
}
void qrx86_door_report(void){
    if(door_calls)fprintf(stderr,"[QRX86-DOOR] calls=%llu x86=%llu\n",(unsigned long long)door_calls,(unsigned long long)door_x86);
}
