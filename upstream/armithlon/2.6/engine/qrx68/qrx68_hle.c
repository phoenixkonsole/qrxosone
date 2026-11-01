#define _POSIX_C_SOURCE 200809L
#include "qrx68_hle.h"
#include "qrx_hunk.h"
#include "qrx86_ipc.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Isolated-library, 6-byte jump vectors, same negative-LVO convention as
   AmigaOS 3.x. High trap addresses are NOT real guest code or host pointers. */
enum { LIB_EXEC=1, LIB_DOS=2, LIB_GFX=3, LIB_INTUITION=4 };
#define VECTOR_COUNT 176u /* 1056-byte negative table, supports DOS -948 */
#define HLE_VERSION 40u
#define OUTPUT_FH 0x100u
#define INPUT_FH  0x101u
#define USER_FH_BASE 0x200u

static bool ram_region(const QRX68 *r,uint32_t addr,size_t n){
    return r && r->memory && addr >= 8u && (uint64_t)addr+n <= r->memory_size;
}
static void wr16(uint8_t *p,uint16_t n){p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static void wr32(uint8_t *p,uint32_t n){wr16(p,(uint16_t)(n>>16));wr16(p+2,(uint16_t)n);}
static uint16_t rd16(const uint8_t *p){return ((uint16_t)p[0]<<8)|p[1];}
static uint32_t rd32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static bool read_str(const QRX68 *r,uint32_t p,char *buf,size_t max){
    if(!buf||max<2 || !ram_region(r,p,1))return false;
    for(size_t i=0;i<max;i++){
        if(!ram_region(r,p,(i+1)))return false;
        buf[i]=(char)r->memory[p+i];
        if(!buf[i])return true;
    }
    return false;
}
static bool basename_only(const char *in,const char **out){
    if(!in||!out)return false;
    const char *p=strchr(in,':');
    if(p){if(strncasecmp(in,"HOST:",5))return false;p=in+5;}else p=in;
    size_t len=strlen(p);
    if(len==0 || len>96 || p[0]=='.' || strstr(p,".."))return false;
    for(size_t i=0;i<len;i++){
        unsigned char c=(unsigned char)p[i];
        if(!(isalnum(c)||c=='_'||c=='-'||c=='.'))return false;
    }
    *out=p;return true;
}
static bool install_lib(QRX68 *r,unsigned lib,uint32_t base){
    uint8_t *m=r->memory;
    if(!ram_region(r,base-6u*VECTOR_COUNT,6u*VECTOR_COUNT+36))return false;
    for(uint32_t n=1;n<=VECTOR_COUNT;n++){
        uint32_t at=base-6u*n;
        wr16(m+at,0x4ef9); /* JMP absolute.l */
        wr32(m+at+2u,QRX68_TRAP_BASE|(lib<<12)|n);
    }
    wr16(m+base+16,6u*VECTOR_COUNT); /* lib_NegSize */
    wr16(m+base+18,36); /* lib_PosSize */
    wr16(m+base+20,HLE_VERSION); /* lib_Version */
    wr16(m+base+22,0); /* lib_Revision */
    return true;
}
static void vm_output(void *ctx,uint8_t ch){
    QRX68 *r=(QRX68*)ctx;
    if(r && r->output)r->output(r->output_ctx,ch);
}
bool qrx68_init(QRX68 *r,size_t bytes,QRXConsoleFn output,void *ctx){
    if(!r || bytes<512u*1024u || bytes>QRX_RAM_MAX_BYTES)return false;
    memset(r,0,sizeof(*r));r->shared_dirfd=-1;
    if(!qrx_ram_allocate(&r->memory_allocation,bytes))return false;
    r->memory=r->memory_allocation.addr;
    r->memory_size=bytes;
    qrx_vm_init(&r->vm,r->memory,bytes);
    r->output=output;r->output_ctx=ctx;
    qrx_vm_set_host(&r->vm,vm_output,NULL,r);
    r->heap_cursor=0x30000u;
    r->heap_limit=(uint32_t)(bytes-131072u);
    if(!install_lib(r,LIB_EXEC,QRX68_EXEC_BASE)||!install_lib(r,LIB_DOS,QRX68_DOS_BASE) ||
       !install_lib(r,LIB_GFX,QRX68_GFX_BASE) ||
       !install_lib(r,LIB_INTUITION,QRX68_INTUITION_BASE)){
        qrx68_destroy(r);return false;
    }
    wr32(r->memory+4,QRX68_EXEC_BASE); /* SysBase at absolute 4 */
    for(size_t i=0;i<QRX68_MAX_HANDLES;i++)r->files[i].fd=-1;
    /* Keep the fake-but-ABI-shaped Task OUTSIDE the growing HUNK image and
     * Exec heap. The high end reserves 128 KiB, before the two CPU stacks. */
    r->task_ptr=(uint32_t)(bytes-8192u);
    memcpy(r->memory+r->task_ptr+256u,"QRX68 Process",14);
    wr32(r->memory+r->task_ptr+10,r->task_ptr+256u); /* Node.ln_Name */
    strcpy(r->program_name,"QRX68-HUNK");
    strcpy(r->current_directory,"HOST:");
    /* The first task initially has no allocated signals. */
    wr32(r->memory+r->task_ptr+18,0);
    wr32(r->memory+r->task_ptr+22,0);
    wr32(r->memory+r->task_ptr+26,0);
    return true;
}
void qrx68_destroy(QRX68 *r){
    if(!r)return;
    for(size_t i=0;i<QRX68_MAX_HANDLES;i++)if(r->files[i].used)close(r->files[i].fd);
    if(r->shared_dirfd>=0)close(r->shared_dirfd);
    qrx_vm_destroy(&r->vm);
    for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++)free(r->windows[i].surface);
    free(r->framebuffer);qrx_ram_release(&r->memory_allocation);memset(r,0,sizeof(*r));r->shared_dirfd=-1;
}
bool qrx68_share(QRX68 *r,const char *dir,bool write_mode){
    if(!r||!r->memory||!dir||r->shared_dirfd>=0)return false;
    int fd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return false;
    r->shared_dirfd=fd;r->shared_write=write_mode;return true;
}
bool qrx68_load_hunk(QRX68 *r,const uint8_t *data,size_t len){
    if(!r||!r->memory || r->steps || r->complete)return false;
    uint32_t entry=0;size_t loaded=0;
    if(!qrx_hunk_load(data,len,r->memory,r->memory_size,
                      QRX_HUNK_STAGE_ADDR,&entry,&loaded))return false;
    /* HUNK data must never overlap the subsequently growing Exec heap.
     * Reserve the entire loaded span plus an aligned guard page. */
    uint64_t limit=(uint64_t)QRX_HUNK_STAGE_ADDR+loaded+4095u;
    uint64_t new_heap=limit&~UINT64_C(4095);
    if(new_heap>r->heap_limit)return false;
    if(new_heap>r->heap_cursor)r->heap_cursor=(uint32_t)new_heap;
    /* Real Amiga CLI entry contract: D0=arglen, A0=argptr, ExecBase at 4.w.
       Bare isolated HUNK program only: no full Process, WBStartup, DOS packets. */
    QRXCPU *c=&r->vm.cpu;
    memset(c,0,sizeof(*c));c->pc=entry;c->sr=0; /* user mode */
    c->a[6]=QRX68_EXEC_BASE;
    c->a[0]=0x21000;wr16(r->memory+0x21000,0);
    c->a[7]=(uint32_t)r->memory_size-4096u-4u;
    wr32(r->memory+c->a[7],QRX68_RETURN_PC);
    c->usp=c->a[7];c->ssp=(uint32_t)r->memory_size-2048u;
    return true;
}
static bool hle_ret(QRX68 *r){
    QRXCPU *c=&r->vm.cpu;
    if(!ram_region(r,c->a[7],4))return false;
    c->pc=rd32(r->memory+c->a[7]);
    c->a[7]+=4;
    return true;
}
static void unsupported(QRX68 *r,unsigned lib,unsigned lvo){
    r->unsupported++;r->failed=1;r->failed_lib=lib;r->failed_lvo=lvo;
    r->vm.cpu.halted=1;
}
/* Isolated Exec allocator: no host pointers exposed to the guest.
 * First-fit reuse of freed blocks; no silent memory leaks for valid FreeMem.
 * CHIP-only requests fail explicitly since no real Chip RAM is present. */
static uint32_t mem_alloc(QRX68 *r,uint32_t bytes,uint32_t attrs,bool vec){
    if(!bytes || (attrs&2u) || bytes>UINT32_MAX-15u)return 0;
    uint32_t rounded=(bytes+7u)&~7u;
    if(!rounded)return 0;
    QRX68Allocation *free_slot=NULL;
    for(unsigned i=0;i<QRX68_MAX_ALLOCS;i++){
        QRX68Allocation *a=&r->allocations[i];
        if(a->in_use)continue;
        if(!free_slot)free_slot=a;
        if(a->addr && a->rounded>=rounded){
            a->bytes=bytes;a->vec=vec;a->in_use=1;
            if(attrs&0x10000u)memset(r->memory+a->addr,0,rounded);
            return a->addr;
        }
    }
    if(!free_slot || r->heap_cursor>r->heap_limit ||
       rounded>r->heap_limit-r->heap_cursor)return 0;
    free_slot->addr=r->heap_cursor;
    free_slot->bytes=bytes;free_slot->rounded=rounded;
    free_slot->vec=vec;free_slot->in_use=1;
    r->heap_cursor+=rounded;
    if(attrs&0x10000u)memset(r->memory+free_slot->addr,0,rounded);
    return free_slot->addr;
}
static bool mem_free(QRX68 *r,uint32_t ptr,uint32_t bytes,bool vec){
    if(!ptr)return true; /* FreeVec(NULL) and FreeMem(NULL) are harmless */
    for(unsigned i=0;i<QRX68_MAX_ALLOCS;i++){
        QRX68Allocation *a=&r->allocations[i];
        if(a->in_use && a->addr==ptr && a->vec==(unsigned)vec &&
           (vec || bytes==a->bytes)){
            a->in_use=0;return true;
        }
    }
    return false;
}
/* Exec MsgPort layout on 32-bit AmigaOS: mp_Flags +14, mp_SigBit +15,
 * mp_SigTask +16. Signal bits 16..31 belong to program allocations. This
 * isolated, single-task runtime cannot run child processes yet. */
/* Classic 32-bit Task offsets (tc_SigAlloc=18, tc_SigWait=22,
 * tc_SigRecvd=26). Guest software may examine FindTask(NULL) directly. */
static void sync_task(QRX68 *r){
    if(r && r->task_ptr && ram_region(r,r->task_ptr,30)){
        wr32(r->memory+r->task_ptr+18,r->signal_alloc);
        wr32(r->memory+r->task_ptr+22,r->signal_wait);
        wr32(r->memory+r->task_ptr+26,r->signal_pending);
    }
}
static QRX68Port *port_find(QRX68 *r,uint32_t ptr){
    for(unsigned i=0;i<QRX68_MAX_PORTS;i++)
        if(r->ports[i].active && r->ports[i].ptr==ptr)return &r->ports[i];
    return NULL;
}
static QRX68Port *port_register(QRX68 *r,uint32_t ptr,bool window_owned){
    if(!ram_region(r,ptr,48) || port_find(r,ptr))return NULL;
    QRX68Port *freeport=NULL;
    for(unsigned i=0;i<QRX68_MAX_PORTS;i++)
        if(!r->ports[i].active){freeport=&r->ports[i];break;}
    if(!freeport)return NULL;
    unsigned sig=16;
    while(sig<32 && (r->signal_alloc&(UINT32_C(1)<<sig)))sig++;
    if(sig==32)return NULL;
    r->signal_alloc|=UINT32_C(1)<<sig;
    freeport->ptr=ptr;freeport->sigbit=(uint8_t)sig;
    freeport->active=1;freeport->window_owned=window_owned;
    sync_task(r);
    r->memory[ptr+14]=0; r->memory[ptr+15]=(uint8_t)sig;
    wr32(r->memory+ptr+16,r->task_ptr);
    wr32(r->memory+ptr+20,ptr+24); /* List.lh_Head empty-list invariant */
    wr32(r->memory+ptr+24,0);      /* List.lh_Tail */
    wr32(r->memory+ptr+28,ptr+20); /* List.lh_TailPred */
    return freeport;
}
static void port_unregister(QRX68 *r,uint32_t ptr){
    QRX68Port *p=port_find(r,ptr);if(!p)return;
    uint32_t mask=UINT32_C(1)<<p->sigbit;
    r->signal_alloc&=~mask;r->signal_pending&=~mask;
    p->active=0;sync_task(r);
    for(unsigned i=0;i<QRX68_MAX_EVENTS;i++){
        if(r->messages[i].port==ptr){r->messages[i].pending=false;r->messages[i].owned=false;}
    }
}
static void signal_port(QRX68 *r,uint32_t ptr){
    QRX68Port *p=port_find(r,ptr);
    if(p){r->signal_pending|=UINT32_C(1)<<p->sigbit;sync_task(r);}
}
/* AmigaOS 3.x 32-bit big-endian ABI offsets, not native host structs:
 * Window.UserPort +86, Window.IDCMPFlags +82, Window.RPort +50.
 * IntuiMessage starts with a 20-byte exec Message; Class +20, Code +24,
 * Qualifier +26, MouseX +32, MouseY +34, IDCMPWindow +44.
 * This intentionally exposes a bounded subset (no gadget/layers/message lists). */
unsigned qrx68_open_window_count(const QRX68 *r){
    unsigned n=0;if(r)for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++)n+=(unsigned)r->windows[i].open;
    return n;
}
static QRX68Message *next_message(QRX68 *r,uint32_t port){
    QRX68Message *oldest=NULL;
    for(unsigned i=0;i<QRX68_MAX_EVENTS;i++){
        QRX68Message *m=&r->messages[i];
        if(m->pending && m->port==port &&
           (!oldest || m->serial<oldest->serial))oldest=m;
    }
    return oldest;
}
static void sync_port_signal(QRX68 *r,uint32_t port){
    QRX68Port *p=port_find(r,port);if(!p)return;
    uint32_t mask=UINT32_C(1)<<p->sigbit;
    if(next_message(r,port))r->signal_pending|=mask;
    else r->signal_pending&=~mask;
    sync_task(r);
}
static uint32_t menu_at(QRX68 *r,QRX68Window *w,int x,unsigned *index);
static void compose(QRX68 *r);
static bool enqueue_event(QRX68 *r,unsigned wi,uint32_t cls,uint16_t code,
                          uint16_t qualifier,int16_t x,int16_t y,uint32_t address){
    if(!r||wi>=QRX68_MAX_WINDOWS || !r->windows[wi].open ||
       !r->windows[wi].userport || !cls || (cls&(cls-1u)) ||
       !(r->windows[wi].idcmp_flags & cls))return false;
    uint32_t port=r->windows[wi].userport;
    for(unsigned i=0;i<QRX68_MAX_EVENTS;i++){
        QRX68Message *m=&r->messages[i];
        if(m->pending || m->owned)continue;
        if(!m->ptr || m->external){
            m->ptr=mem_alloc(r,64,0x10000u,true);m->external=false;
            if(!m->ptr)return false;
        }
        uint8_t *b=r->memory+m->ptr;memset(b,0,64);
        wr16(b+18,52);wr32(b+20,cls);wr16(b+24,code);
        wr16(b+26,qualifier);wr32(b+28,address);
        wr16(b+32,(uint16_t)x);wr16(b+34,(uint16_t)y);
        wr32(b+44,r->windows[wi].window);
        m->port=port;m->window=wi;m->type=cls;
        m->serial=++r->event_serial;m->pending=true;
        signal_port(r,port);r->yielded=false;
        return true;
    }
    return false;
}
static uint32_t gadget_at(QRX68 *r,QRX68Window *w,int x,int y){
    uint32_t ptr=w->first_gadget;
    for(unsigned n=0;n<QRX68_MAX_GADGETS && ptr;n++){
        if(!ram_region(r,ptr,44))return 0;
        int gx=(int16_t)rd16(r->memory+ptr+4);
        int gy=(int16_t)rd16(r->memory+ptr+6);
        int gw=(int16_t)rd16(r->memory+ptr+8);
        int gh=(int16_t)rd16(r->memory+ptr+10);
        uint16_t flags=rd16(r->memory+ptr+12);
        if(!(flags&0x200u) && gw>0 && gh>0 &&
           x>=gx && y>=gy && x-gx<gw && y-gy<gh)return ptr;
        ptr=rd32(r->memory+ptr);
    }
    return 0;
}
/* Native host mouse input is translated into the SAME gadget IDCMP ABI that
 * a real Intuition program reads. No forged Gadget message from untrusted IPC. */
bool qrx68_post_event(QRX68 *r,unsigned wi,uint32_t cls,uint16_t code,
                      uint16_t qualifier,int16_t x,int16_t y){
    if(!r||wi>=QRX68_MAX_WINDOWS||!r->windows[wi].open)return false;
    QRX68Window *w=&r->windows[wi];
    if(cls==QRX68_IDCMP_MOUSEBUTTONS){
        int lx=x-w->x,ly=y-w->y;
        if(code==105 && w->menu_strip && ly>=0 && ly<14){
            unsigned idx=0;
            if(menu_at(r,w,lx,&idx)){
                w->active_menu=(int)idx;compose(r);r->frame_generation++;
                return true; /* right mouse MENU DOWN */
            }
        }
        if(code==233 && w->active_menu>=0){
            unsigned selected=(unsigned)w->active_menu;
            uint32_t menu=w->menu_strip;
            for(unsigned i=0;i<selected && menu && ram_region(r,menu,22);i++)
                menu=rd32(r->memory+menu);
            unsigned item_index=(ly>=15)?(unsigned)(ly-15)/14u:UINT32_MAX;
            uint32_t item=menu&&ram_region(r,menu,22)?rd32(r->memory+menu+18):0;
            bool found=false;
            for(unsigned i=0;i<QRX68_MAX_MENUITEMS && item;i++){
                if(!ram_region(r,item,34))break;
                if(i==item_index){found=true;break;}
                item=rd32(r->memory+item);
            }
            w->active_menu=-1;compose(r);r->frame_generation++;
            if(found && selected<32 && item_index<64 &&
               (w->idcmp_flags&QRX68_IDCMP_MENUPICK)){
                r->menu_picks++;
                return enqueue_event(r,wi,QRX68_IDCMP_MENUPICK,
                   (uint16_t)(selected | (item_index<<5)),qualifier,x,y,0);
            }
            return true;
        }
        if(code==104){ /* SELECTDOWN */
            w->active_gadget=gadget_at(r,w,lx,ly);
            if(w->active_gadget && (w->idcmp_flags&QRX68_IDCMP_GADGETDOWN)){
                return enqueue_event(r,wi,QRX68_IDCMP_GADGETDOWN,0,qualifier,x,y,w->active_gadget);
            }
        }else if(code==232){ /* SELECTUP */
            uint32_t hit=gadget_at(r,w,lx,ly);
            uint32_t active=w->active_gadget;w->active_gadget=0;
            if(hit && hit==active && (w->idcmp_flags&QRX68_IDCMP_GADGETUP)){
                r->gadget_picks++;
                return enqueue_event(r,wi,QRX68_IDCMP_GADGETUP,0,qualifier,x,y,hit);
            }
        }
    }
    return enqueue_event(r,wi,cls,code,qualifier,x,y,0);
}
static uint32_t exec_api(QRX68 *r,unsigned lvo,bool *known){
    QRXCPU *c=&r->vm.cpu;
    *known=true;
    switch(lvo){
        case 120: r->defer_count++;return c->d[0]; /* Forbid */
        case 126: if(r->defer_count)r->defer_count--;return c->d[0]; /* Permit */
        case 132: r->defer_count++;return c->d[0]; /* Disable */
        case 138: if(r->defer_count)r->defer_count--;return c->d[0]; /* Enable */
        case 294: /* FindTask: this isolated runtime owns exactly one Task */
            return c->a[1]==0?r->task_ptr:0;
        case 306: { /* SetSignal(D0=new,D1=mask) returns previous */
            uint32_t old=r->signal_pending;
            r->signal_pending=(old&~c->d[1])|(c->d[0]&c->d[1]);
            return old;
        }
        case 318: { /* Wait(D0=mask), returns and consumes pending bits */
            uint32_t matched=r->signal_pending&c->d[0];
            r->signal_pending&=~matched;
            r->signal_wait=0;
            return matched;
        }
        case 324: /* Signal(A1=task,D0=mask), current Task only */
            if(c->a[1]!=r->task_ptr){*known=false;return 0;}
            r->signal_pending|=c->d[0];r->yielded=false;return c->d[0];
        case 330: { /* AllocSignal(D0=bit or -1), only user signal bits */
            unsigned bit=c->d[0]==UINT32_MAX?16u:c->d[0];
            if(bit>31u)return UINT32_MAX;
            if(c->d[0]==UINT32_MAX)
                while(bit<32u && (r->signal_alloc&(1u<<bit)))bit++;
            if(bit>=32u || (r->signal_alloc&(1u<<bit)))return UINT32_MAX;
            r->signal_alloc|=1u<<bit;r->signal_pending&=~(1u<<bit);
            return bit;
        }
        case 336: { /* FreeSignal(D0=bit) */
            unsigned bit=c->d[0];
            if(bit<16u||bit>=32u||!(r->signal_alloc&(1u<<bit))){*known=false;return 0;}
            for(unsigned i=0;i<QRX68_MAX_PORTS;i++)
                if(r->ports[i].active && r->ports[i].sigbit==bit){*known=false;return 0;}
            r->signal_alloc&=~(1u<<bit);r->signal_pending&=~(1u<<bit);
            return c->d[0];
        }
        case 666: { /* CreateMsgPort() allocated by Exec */
            uint32_t ptr=mem_alloc(r,48,0x10000u,true);
            if(!ptr || !port_register(r,ptr,false))return 0;
            return ptr;
        }
        case 672: { /* DeleteMsgPort(A0) */
            QRX68Port *p=port_find(r,c->a[0]);
            if(!p || p->window_owned){*known=false;return 0;}
            port_unregister(r,c->a[0]);return c->d[0];
        }
        case 366: { /* PutMsg(A0=port,A1=message) */
            uint32_t port=c->a[0],msg=c->a[1];
            if(!port_find(r,port)||!ram_region(r,msg,20)){*known=false;return 0;}
            for(unsigned i=0;i<QRX68_MAX_EVENTS;i++){
                QRX68Message *m=&r->messages[i];
                if(m->pending||m->owned)continue;
                /* previous host-allocated buffer may be recycled later */
                m->ptr=msg;m->port=port;m->window=UINT32_MAX;
                m->type=0;m->serial=++r->event_serial;
                m->pending=true;m->external=true;
                signal_port(r,port);r->yielded=false;return c->d[0];
            }
            *known=false;return 0;
        }
        case 552: case 408: { /* OpenLibrary or OldOpenLibrary */
            char name[256];if(!read_str(r,c->a[1],name,sizeof(name)))return 0;
            if(c->d[0]>HLE_VERSION)return 0;
            if(!strcasecmp(name,"dos.library")){r->dos_open_count++;return QRX68_DOS_BASE;}
            if(!strcasecmp(name,"exec.library"))return QRX68_EXEC_BASE;
            if(r->experimental_gui && !strcasecmp(name,"graphics.library")){
                r->gfx_open_count++;return QRX68_GFX_BASE;
            }
            if(r->experimental_gui && !strcasecmp(name,"intuition.library")){
                r->intuition_open_count++;return QRX68_INTUITION_BASE;
            }
            return 0; /* Other libraries are not faked. */
        }
        case 414: /* CloseLibrary(A1=base) */
            if(c->a[1]==QRX68_DOS_BASE && r->dos_open_count)r->dos_open_count--;
            if(c->a[1]==QRX68_GFX_BASE && r->gfx_open_count)r->gfx_open_count--;
            if(c->a[1]==QRX68_INTUITION_BASE && r->intuition_open_count)r->intuition_open_count--;
            return c->d[0]; /* void: preserve d0 */
        case 198: return mem_alloc(r,c->d[0],c->d[1],false); /* AllocMem */
        case 684: return mem_alloc(r,c->d[0],c->d[1],true);  /* AllocVec */
        case 210: /* FreeMem(A1=ptr,D0=size), strict size accounting */
            if(!mem_free(r,c->a[1],c->d[0],false)) { *known=false;return 0; }
            return c->d[0];
        case 690: /* FreeVec(A1=ptr), size tracked by runtime */
            if(!mem_free(r,c->a[1],0,true)) { *known=false;return 0; }
            return c->d[0];
        case 624: case 630: { /* CopyMem/CopyMemQuick A0,A1,D0 */
            uint32_t n=c->d[0];
            if(n>16u*1024u*1024u||!ram_region(r,c->a[0],n)||
                !ram_region(r,c->a[1],n)) {*known=false;return 0;}
            if(lvo==630 && ((c->a[0]|c->a[1]|n)&3u)){*known=false;return 0;}
            memmove(r->memory+c->a[1],r->memory+c->a[0],n);
            return c->d[0];
        }
        case 216: /* AvailMem (simple isolated arena; no CHIP flags) */
            if(c->d[1]&2u)return 0; /* MEMF_CHIP is not emulated */
            return r->heap_limit-r->heap_cursor;
        case 372: { /* GetMsg(A0=MsgPort) */
            if(!port_find(r,c->a[0])){*known=false;return 0;}
            QRX68Message *m=next_message(r,c->a[0]);
            if(!m)return 0;
            m->pending=false;m->owned=true;
            sync_port_signal(r,c->a[0]);
            return m->ptr;
        }
        case 378: { /* ReplyMsg(A1=Message) */
            for(unsigned i=0;i<QRX68_MAX_EVENTS;i++){
                QRX68Message *m=&r->messages[i];
                if(m->owned && m->ptr==c->a[1]){
                    if(m->external && ram_region(r,m->ptr,20)){
                        uint32_t reply=rd32(r->memory+m->ptr+14);
                        if(reply && port_find(r,reply)){
                            m->port=reply;m->pending=true;m->owned=false;
                            m->serial=++r->event_serial;signal_port(r,reply);
                            return c->d[0];
                        }
                    }
                    m->owned=false;return c->d[0];
                }
            }
            *known=false;return 0;
        }
        case 384: { /* WaitPort: blocked until queued message */
            if(!port_find(r,c->a[0])){*known=false;return 0;}
            QRX68Message *m=next_message(r,c->a[0]);
            return m?m->ptr:0;
        }
        default: *known=false;return 0;
    }
}
static int get_fd(QRX68 *r,uint32_t handle){
    if(handle<USER_FH_BASE || handle>=USER_FH_BASE+QRX68_MAX_HANDLES)return -1;
    QRX68File *f=&r->files[handle-USER_FH_BASE];return f->used?f->fd:-1;
}
static uint32_t dos_api(QRX68 *r,unsigned lvo,bool *known){
    QRXCPU *c=&r->vm.cpu;
    *known=true;
    switch(lvo){
        case 144: /* Exit(D1=returncode) terminates current process */
            r->complete=1;return c->d[1];
        case 216: return (c->d[1]==INPUT_FH || c->d[1]==OUTPUT_FH)?1u:0u;
        case 294: { /* SelectInput(D1=handle), preserve current std input */
            return INPUT_FH;
        }
        case 300: return OUTPUT_FH; /* SelectOutput, isolated stdout */
        case 462: { /* SetIoErr(D1=code), returns previous */
            uint32_t old=r->ioerr;r->ioerr=c->d[1];return old;
        }
        case 492: return 0; /* Cli(): launched as a bare HUNK, no CLI */
        case 534: return r->cli_arg_ptr; /* GetArgStr */
        case 540: { /* SetArgStr(D1=new), returns old */
            uint32_t old=r->cli_arg_ptr;
            if(c->d[1] && !ram_region(r,c->d[1],1)){*known=false;return 0;}
            r->cli_arg_ptr=c->d[1];return old;
        }
        case 570: { /* SetProgramName(D1=STRPTR) */
            char name[128];
            if(!read_str(r,c->d[1],name,sizeof(name)))return 0;
            memcpy(r->program_name,name,strlen(name)+1u);return 1;
        }
        case 576: { /* GetProgramName(D1=buf,D2=size) */
            size_t len=strlen(r->program_name)+1u;
            if(len>c->d[2]||!ram_region(r,c->d[1],len))return 0;
            memcpy(r->memory+c->d[1],r->program_name,len);return 1;
        }
        case 558: { /* SetCurrentDirName(D1=STRPTR) virtual name only */
            char name[128];
            if(!read_str(r,c->d[1],name,sizeof(name)))return 0;
            if(strcmp(name,"HOST:") && strcmp(name,"SYS:"))return 0;
            strcpy(r->current_directory,name);return 1;
        }
        case 564: { /* GetCurrentDirName(D1=buf,D2=size) */
            size_t len=strlen(r->current_directory)+1u;
            if(len>c->d[2]||!ram_region(r,c->d[1],len))return 0;
            memcpy(r->memory+c->d[1],r->current_directory,len);return 1;
        }
        case 54:return INPUT_FH;
        case 60:return OUTPUT_FH;
        case 132:return r->ioerr;
        case 30:{ /* Open(D1=name,D2=mode), host dir only */
            char str[128];const char *name=NULL;
            if(r->shared_dirfd<0||!read_str(r,c->d[1],str,sizeof(str))||!basename_only(str,&name)){
                r->ioerr=205;return 0;
            }
            int flags=0;
            if(c->d[2]==1005)flags=O_RDONLY;
            else if(r->shared_write && c->d[2]==1006)flags=O_WRONLY|O_CREAT|O_TRUNC;
            else if(r->shared_write && c->d[2]==1004)flags=O_RDWR|O_CREAT;
            else{r->ioerr=214;return 0;}
            int fd=openat(r->shared_dirfd,name,flags|O_NOFOLLOW|O_CLOEXEC,0600);
            if(fd<0){r->ioerr=(errno==ENOENT?205:214);return 0;}
            struct stat st;
            if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)){close(fd);r->ioerr=214;return 0;}
            for(unsigned i=0;i<QRX68_MAX_HANDLES;i++)if(!r->files[i].used){
                r->files[i].fd=fd;r->files[i].used=1;r->ioerr=0;
                return USER_FH_BASE+i;
            }
            close(fd);r->ioerr=103;return 0;
        }
        case 36:{ /* Close(D1=handle) */
            uint32_t h=c->d[1];int fd=get_fd(r,h);
            if(fd<0){r->ioerr=214;return 0;}
            close(fd);r->files[h-USER_FH_BASE].used=0;r->files[h-USER_FH_BASE].fd=-1;
            r->ioerr=0;return 1;
        }
        case 42: case 48: { /* Read/Write(D1=BPTR,D2=buf,D3=count) */
            uint32_t h=c->d[1],p=c->d[2],len=c->d[3];
            if(len>1024u*1024u || !ram_region(r,p,len)){r->ioerr=214;return UINT32_MAX;}
            if(lvo==48 && h==OUTPUT_FH){
                for(uint32_t i=0;i<len;i++)vm_output(r,r->memory[p+i]);
                r->ioerr=0;return len;
            }
            int fd=get_fd(r,h);
            if(fd<0){r->ioerr=214;return UINT32_MAX;}
            ssize_t n=lvo==42?read(fd,r->memory+p,len):write(fd,r->memory+p,len);
            if(n<0){r->ioerr=214;return UINT32_MAX;}
            r->ioerr=0;return (uint32_t)n;
        }
        case 66:{ /* Seek(D1=handle,D2=offset,D3=mode), old position */
            int fd=get_fd(r,c->d[1]);
            int whence=c->d[3]==(uint32_t)-1?SEEK_SET:c->d[3]==0?SEEK_CUR:
                       c->d[3]==1?SEEK_END:-1;
            if(fd<0||whence<0){r->ioerr=214;return UINT32_MAX;}
            off_t old=lseek(fd,0,SEEK_CUR);
            if(old<0||lseek(fd,(int32_t)c->d[2],whence)<0){r->ioerr=214;return UINT32_MAX;}
            r->ioerr=0;return (uint32_t)old;
        }
        case 948:{ /* PutStr(D1=c_string) */
            char str[8192];
            if(!read_str(r,c->d[1],str,sizeof(str))){r->ioerr=214;return UINT32_MAX;}
            for(size_t i=0;str[i];i++)vm_output(r,(uint8_t)str[i]);
            r->ioerr=0;return 0;
        }
        default:*known=false;return 0;
    }
}
/* QRX68 GUI HLE is intentionally limited to plain drawing windows with
 * zero IDCMPFlags: it does not pretend to support messages, gadgets, layers,
 * Workbench startup packets or other complex Intuition contracts. */
static QRX68Window *find_rp(QRX68 *r,uint32_t ptr){
    for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++)
        if(r->windows[i].open && r->windows[i].rastport==ptr)return &r->windows[i];
    return NULL;
}
static QRX68Window *find_window(QRX68 *r,uint32_t ptr){
    for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++)
        if(r->windows[i].open && r->windows[i].window==ptr)return &r->windows[i];
    return NULL;
}
/* Separate opaque window surfaces. The visible buffer is composed according
 * to Z order after each drawing API call (not once per pixel). This prevents
 * one window's redraw from permanently destroying a window underneath it. */
/* Bounded one-level classic Menu/MenuItem traversal; never follow invalid
 * pointers or circular lists indefinitely. Menu strip is rendered as simple
 * rectangles: rich text and nested submenus are NOT implemented. */
static uint32_t menu_at(QRX68 *r,QRX68Window *w,int x,unsigned *index){
    uint32_t menu=w->menu_strip;
    for(unsigned i=0;i<16 && menu;i++){
        if(!ram_region(r,menu,22))break;
        int left=(int16_t)rd16(r->memory+menu+4);
        int width=(int16_t)rd16(r->memory+menu+8);
        if(width>0 && width<=640 && x>=left && x-left<width){
            if(index)*index=i;
            return menu;
        }
        uint32_t next=rd32(r->memory+menu);
        if(next==menu)break;
        menu=next;
    }
    return 0;
}
static void overlay_rect(QRX68 *r,int x,int y,int w,int h,uint8_t color){
    for(int cy=0;cy<h;cy++){
        int py=y+cy;if(py<0||py>=(int)QRX68_FB_HEIGHT)continue;
        for(int cx=0;cx<w;cx++){
            int px=x+cx;if(px<0||px>=(int)QRX68_FB_WIDTH)continue;
            r->framebuffer[(size_t)py*QRX68_FB_WIDTH+(unsigned)px]=color;
        }
    }
}
static void draw_menu_overlay(QRX68 *r,QRX68Window *w){
    uint32_t menu=w->menu_strip;unsigned i=0;
    while(menu && i<16 && ram_region(r,menu,22)){
        int left=(int16_t)rd16(r->memory+menu+4);
        int width=(int16_t)rd16(r->memory+menu+8);
        if(width<=0||width>640)break;
        overlay_rect(r,w->x+left,w->y,width,14,8);
        overlay_rect(r,w->x+left+1,w->y+1,width-2,12, i==(unsigned)w->active_menu?4:1);
        if(i==(unsigned)w->active_menu){
            uint32_t item=rd32(r->memory+menu+18);
            for(unsigned n=0;n<QRX68_MAX_MENUITEMS && item;n++){
                if(!ram_region(r,item,34))break;
                int height=14;
                overlay_rect(r,w->x+left,w->y+15+(int)n*height,width,
                             height,8);
                overlay_rect(r,w->x+left+1,w->y+16+(int)n*height,
                             width-2,height-2,1);
                uint32_t next=rd32(r->memory+item);
                if(next==item)break;
                item=next;
            }
        }
        uint32_t next=rd32(r->memory+menu);
        if(next==menu)break;
        menu=next;i++;
    }
}
static void compose(QRX68 *r){
    if(!r->framebuffer)return;
    memset(r->framebuffer,0,(size_t)QRX68_FB_WIDTH*QRX68_FB_HEIGHT);
    for(unsigned pass=0;pass<QRX68_MAX_WINDOWS;pass++){
        QRX68Window *w=NULL;
        for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++){
            QRX68Window *candidate=&r->windows[i];
            if(!candidate->open||!candidate->surface)continue;
            if(w && candidate->z_order>=w->z_order)continue;
            /* Sort by scanning smallest remaining Z rank. */
            unsigned before=0;
            for(unsigned j=0;j<QRX68_MAX_WINDOWS;j++)
                if(r->windows[j].open && r->windows[j].surface &&
                   r->windows[j].z_order<candidate->z_order)before++;
            if(before==pass)w=candidate;
        }
        if(!w)continue;
        for(int y=0;y<w->height;y++){
            int sy=w->y+y;
            if(sy<0||sy>=(int)QRX68_FB_HEIGHT)continue;
            for(int x=0;x<w->width;x++){
                int sx=w->x+x;
                if(sx<0||sx>=(int)QRX68_FB_WIDTH)continue;
                r->framebuffer[(size_t)sy*QRX68_FB_WIDTH+(unsigned)sx]=
                    w->surface[(size_t)y*(unsigned)w->width+(unsigned)x];
            }
        }
        if(w->menu_strip)draw_menu_overlay(r,w);
    }
}
static void set_pixel(QRX68 *r,QRX68Window *w,int x,int y){
    if(!w->surface || x<0||y<0||x>=w->width||y>=w->height)return;
    size_t i=(size_t)y*(unsigned)w->width+(unsigned)x;
    if(w->surface[i]!=w->pen){w->surface[i]=w->pen;r->frame_generation++;}
}
static void draw_gadgets(QRX68 *r,QRX68Window *w){
    uint32_t ptr=w->first_gadget;
    for(unsigned n=0;n<QRX68_MAX_GADGETS && ptr;n++){
        if(!ram_region(r,ptr,44))break;
        int x=(int16_t)rd16(r->memory+ptr+4),y=(int16_t)rd16(r->memory+ptr+6);
        int width=(int16_t)rd16(r->memory+ptr+8),height=(int16_t)rd16(r->memory+ptr+10);
        uint8_t prev=w->pen;w->pen=(rd16(r->memory+ptr+12)&0x200u)?8:1;
        if(width>0&&height>0&&width<=640&&height<=256){
            for(int i=0;i<width;i++){
                set_pixel(r,w,x+i,y);set_pixel(r,w,x+i,y+height-1);
            }
            for(int i=0;i<height;i++){
                set_pixel(r,w,x,y+i);set_pixel(r,w,x+width-1,y+i);
            }
        }
        w->pen=prev;ptr=rd32(r->memory+ptr);
    }
    compose(r);
}
/* OpenWindow() and OpenWindowTagList() share exactly the same bounded
 * constructor; tag-based callers do not bypass GUI or safety validation. */
static uint32_t open_window_new(QRX68 *r,uint32_t p){
            if(!ram_region(r,p,48))return 0;
            uint32_t flags=rd32(r->memory+p+10);
            const uint32_t allowed=QRX68_IDCMP_CLOSEWINDOW|QRX68_IDCMP_MOUSEBUTTONS|
                QRX68_IDCMP_MOUSEMOVE|QRX68_IDCMP_RAWKEY|QRX68_IDCMP_VANILLAKEY|
                QRX68_IDCMP_GADGETDOWN|QRX68_IDCMP_GADGETUP|
                QRX68_IDCMP_MENUPICK|QRX68_IDCMP_REFRESHWINDOW;
            if(flags&~allowed)return 0;
            for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++){
                QRX68Window *w=&r->windows[i];if(w->open)continue;
                int x=(int16_t)rd16(r->memory+p),y=(int16_t)rd16(r->memory+p+2);
                int width=(int16_t)rd16(r->memory+p+4),height=(int16_t)rd16(r->memory+p+6);
                if(width<=0||height<=0||width>640||height>256)return 0;
                if(!r->experimental_gui)return 0;
                uint32_t win=mem_alloc(r,192,0x10000u,true);
                uint32_t rastport=mem_alloc(r,128,0x10000u,true);
                uint32_t userport=mem_alloc(r,48,0x10000u,true);
                if(!win||!rastport||!userport || !port_register(r,userport,true))return 0;
                uint32_t gadgets=rd32(r->memory+p+18);
                if(gadgets && !ram_region(r,gadgets,44)){
                    port_unregister(r,userport);return 0;
                }
                w->first_gadget=gadgets;
                w->window=win;w->rastport=rastport;w->x=x;w->y=y;
                w->width=width;w->height=height;w->cursor_x=0;w->cursor_y=0;
                w->pen=1;w->open=true;w->idcmp_flags=flags;w->userport=userport;
                w->z_order=++r->event_serial;
                w->active_menu=-1;
                w->surface=calloc((size_t)width*(unsigned)height,1);
                if(!w->surface){
                    w->open=false;port_unregister(r,userport);return 0;
                }
                /* AmigaOS 3.x Window->RPort +50, IDCMPFlags +82, UserPort +86. */
                wr16(r->memory+win+4,(uint16_t)x);
                wr16(r->memory+win+6,(uint16_t)y);
                wr16(r->memory+win+8,(uint16_t)width);
                wr16(r->memory+win+10,(uint16_t)height);
                wr32(r->memory+win+24,rd32(r->memory+p+14));
                wr32(r->memory+win+62,gadgets);
                wr32(r->memory+win+50,rastport);
                wr32(r->memory+win+82,flags);
                wr32(r->memory+win+86,userport);
                draw_gadgets(r,w);
                r->frame_generation++;
                return win;
            }
            return 0;
}

static uint32_t intuition_api(QRX68 *r,unsigned lvo,bool *known){
    QRXCPU *c=&r->vm.cpu;
    *known=true;
    switch(lvo){
        case 204: return open_window_new(r,c->a[0]);
        case 606: { /* OpenWindowTagList(A0=NewWindow or NULL,A1=TagItem*) */
            uint32_t tmp=mem_alloc(r,48,0x10000u,true);
            if(!tmp)return 0;
            if(c->a[0]){
                if(!ram_region(r,c->a[0],48)){
                    (void)mem_free(r,tmp,0,true);return 0;
                }
                memcpy(r->memory+tmp,r->memory+c->a[0],48);
            }
            uint32_t tagptr=c->a[1];
            for(unsigned i=0;i<128 && tagptr;i++){
                if(!ram_region(r,tagptr,8)){
                    (void)mem_free(r,tmp,0,true);return 0;
                }
                uint32_t tag=rd32(r->memory+tagptr);
                uint32_t value=rd32(r->memory+tagptr+4);
                tagptr+=8;
                if(tag==0)break; /* TAG_DONE */
                if(tag==1)continue; /* TAG_IGNORE */
                if(tag==2){tagptr=value;continue;} /* TAG_MORE */
                if(tag==3){
                    if(value>128u-i){(void)mem_free(r,tmp,0,true);return 0;}
                    tagptr+=value*8u;continue;
                }
                /* TAG_USER+99=WA_Dummy; fields follow the 32bit NDK. */
                if(tag<UINT32_C(0x80000064) || tag>UINT32_C(0x80000081)){
                    (void)mem_free(r,tmp,0,true);return 0;
                }
                switch(tag-UINT32_C(0x80000063)){
                    case 1:wr16(r->memory+tmp,(uint16_t)value);break; /* Left */
                    case 2:wr16(r->memory+tmp+2,(uint16_t)value);break;
                    case 3:wr16(r->memory+tmp+4,(uint16_t)value);break;
                    case 4:wr16(r->memory+tmp+6,(uint16_t)value);break;
                    case 5:r->memory[tmp+8]=(uint8_t)value;break;
                    case 6:r->memory[tmp+9]=(uint8_t)value;break;
                    case 7:wr32(r->memory+tmp+10,value);break; /* IDCMP */
                    case 8:wr32(r->memory+tmp+14,value);break; /* Flags */
                    case 9:wr32(r->memory+tmp+18,value);break; /* Gadgets */
                    case 11:wr32(r->memory+tmp+26,value);break; /* Title */
                    case 15:wr16(r->memory+tmp+38,(uint16_t)value);break;
                    case 16:wr16(r->memory+tmp+40,(uint16_t)value);break;
                    case 17:wr16(r->memory+tmp+42,(uint16_t)value);break;
                    case 18:wr16(r->memory+tmp+44,(uint16_t)value);break;
                    default:(void)mem_free(r,tmp,0,true);return 0;
                }
            }
            uint32_t win=open_window_new(r,tmp);
            (void)mem_free(r,tmp,0,true);
            return win;
        }
        case 150: { /* ModifyIDCMP(A0=window,D0=flags) */
            QRX68Window *w=find_window(r,c->a[0]);
            uint32_t allowed=QRX68_IDCMP_CLOSEWINDOW|QRX68_IDCMP_MOUSEBUTTONS|
                QRX68_IDCMP_MOUSEMOVE|QRX68_IDCMP_RAWKEY|QRX68_IDCMP_VANILLAKEY|
                QRX68_IDCMP_GADGETDOWN|QRX68_IDCMP_GADGETUP|
                QRX68_IDCMP_MENUPICK|QRX68_IDCMP_REFRESHWINDOW;
            if(!w || (c->d[0]&~allowed))return 0;
            w->idcmp_flags=c->d[0];wr32(r->memory+w->window+82,c->d[0]);return 1;
        }
        case 42: { /* AddGadget(A0=window,A1=gadget,D0=position) */
            QRX68Window *w=find_window(r,c->a[0]);
            uint32_t g=c->a[1];
            if(!w||!ram_region(r,g,44))return UINT32_MAX;
            uint32_t current=w->first_gadget;
            if(!current){w->first_gadget=g;wr32(r->memory+w->window+62,g);}
            else{
                unsigned n=0;
                while(n++<QRX68_MAX_GADGETS && ram_region(r,current,44)){
                    if(current==g)return UINT32_MAX;
                    uint32_t next=rd32(r->memory+current);
                    if(!next){wr32(r->memory+current,g);break;}
                    if(next==g)return UINT32_MAX;
                    current=next;
                }
                if(n>QRX68_MAX_GADGETS)return UINT32_MAX;
            }
            draw_gadgets(r,w);r->frame_generation++;return 0; /* first position */
        }
        case 174: case 186: { /* OffGadget / OnGadget */
            QRX68Window *w=find_window(r,c->a[1]);
            uint32_t g=c->a[0];
            if(!w||!ram_region(r,g,44)||c->a[2]){*known=false;return 0;}
            uint16_t flags=rd16(r->memory+g+12);
            flags=lvo==174?(uint16_t)(flags|0x200u):(uint16_t)(flags&~0x200u);
            wr16(r->memory+g+12,flags);
            draw_gadgets(r,w);r->frame_generation++;return c->d[0];
        }
        case 222: case 432: { /* RefreshGadgets/RefreshGList */
            QRX68Window *w=find_window(r,c->a[1]);
            if(!w || (c->a[0]&&!ram_region(r,c->a[0],44)) || c->a[2]){
                *known=false;return 0;
            }
            draw_gadgets(r,w);r->frame_generation++;return c->d[0];
        }
        case 264: { /* SetMenuStrip(A0=window,A1=Menu) */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w || !ram_region(r,c->a[1],22))return 0;
            w->menu_strip=c->a[1];wr32(r->memory+w->window+28,c->a[1]);
            w->active_menu=-1;compose(r);
            r->frame_generation++;return 1;
        }
        case 54: { /* ClearMenuStrip(A0=window) */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w)return 0;
            w->menu_strip=0;wr32(r->memory+w->window+28,0);
            w->active_menu=-1;compose(r);
            r->frame_generation++;return 1;
        }
        case 354: case 366: { /* BeginRefresh / EndRefresh */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w){*known=false;return 0;}
            if(lvo==366){compose(r);r->frame_generation++;}
            return c->d[0];
        }
        case 486: { /* ChangeWindowBox(A0,D0 left,D1 top,D2 w,D3 h) */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w){*known=false;return 0;}
            int width=(int16_t)c->d[2],height=(int16_t)c->d[3];
            if(width<=0||height<=0||width>640||height>256)return c->d[0];
            uint8_t *new_surface=calloc((size_t)width*(unsigned)height,1);
            if(!new_surface)return c->d[0];
            int copyw=width<w->width?width:w->width;
            int copyh=height<w->height?height:w->height;
            for(int y=0;y<copyh;y++)memcpy(new_surface+(size_t)y*(unsigned)width,
                w->surface+(size_t)y*(unsigned)w->width,(unsigned)copyw);
            free(w->surface);w->surface=new_surface;
            w->x=(int16_t)c->d[0];w->y=(int16_t)c->d[1];
            w->width=width;w->height=height;
            wr16(r->memory+w->window+4,(uint16_t)w->x);
            wr16(r->memory+w->window+6,(uint16_t)w->y);
            wr16(r->memory+w->window+8,(uint16_t)width);
            wr16(r->memory+w->window+10,(uint16_t)height);
            compose(r);r->frame_generation++;
            return c->d[0];
        }
        case 168: { /* MoveWindow(A0,D0=deltaX,D1=deltaY), clipped */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w){*known=false;return 0;}
            int dx=(int16_t)c->d[0],dy=(int16_t)c->d[1];
            if(abs(dx)>2048||abs(dy)>2048)return c->d[0];
            w->x+=dx;w->y+=dy;
            wr16(r->memory+w->window+4,(uint16_t)w->x);
            wr16(r->memory+w->window+6,(uint16_t)w->y);
            compose(r);r->frame_generation++;
            return c->d[0];
        }
        case 306: case 312: { /* WindowToBack / WindowToFront */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w){*known=false;return 0;}
            uint64_t z=w->z_order;
            if(lvo==312)w->z_order=++r->event_serial;
            else{
                for(unsigned i=0;i<QRX68_MAX_WINDOWS;i++){
                    QRX68Window *p=&r->windows[i];
                    if(p!=w && p->open && p->z_order<z)p->z_order++;
                }
                w->z_order=0;
            }
            compose(r);r->frame_generation++;return c->d[0];
        }
        case 450: { /* ActivateWindow(A0): basic frontmost selection */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w){*known=false;return 0;}
            w->z_order=++r->event_serial;
            compose(r);r->frame_generation++;return c->d[0];
        }
        case 72: { /* CloseWindow(A0) */
            QRX68Window *w=find_window(r,c->a[0]);
            if(!w){*known=false;return 0;}
            w->open=false;
            free(w->surface);w->surface=NULL;
            compose(r);
            port_unregister(r,w->userport);
            for(unsigned i=0;i<QRX68_MAX_EVENTS;i++){
                QRX68Message *m=&r->messages[i];
                if(m->window==(unsigned)(w-r->windows)){m->pending=false;m->owned=false;}
            }
            r->frame_generation++;
            return c->d[0];
        }
        default:*known=false;return 0;
    }
}
static uint32_t graphics_api(QRX68 *r,unsigned lvo,bool *known){
    QRXCPU *c=&r->vm.cpu;
    *known=true;
    QRX68Window *w=find_rp(r,c->a[1]);
    if(!w){*known=false;return 0;}
    r->gfx_calls++;
    switch(lvo){
        case 342:{uint8_t old=w->pen;w->pen=(uint8_t)(c->d[0]&15u);return old;} /* SetAPen */
        case 306:{ /* RectFill */
            int x0=(int16_t)c->d[0],y0=(int16_t)c->d[1];
            int x1=(int16_t)c->d[2],y1=(int16_t)c->d[3];
            if(x0>x1 || y0>y1)return c->d[0];
            if(x0<0)x0=0;
            if(y0<0)y0=0;
            if(x1>=w->width)x1=w->width-1;
            if(y1>=w->height)y1=w->height-1;
            for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)set_pixel(r,w,x,y);
            compose(r);return c->d[0];
        }
        case 240: w->cursor_x=(int16_t)c->d[0];w->cursor_y=(int16_t)c->d[1];return c->d[0];
        case 246: { /* Draw: Bresenham inclusive endpoints */
            int x=w->cursor_x,y=w->cursor_y,x1=(int16_t)c->d[0],y1=(int16_t)c->d[1];
            int dx=abs(x1-x),sx=x<x1?1:-1,dy=-abs(y1-y),sy=y<y1?1:-1;
            int e=dx+dy;
            for(unsigned i=0;i<2048;i++){
                set_pixel(r,w,x,y);if(x==x1&&y==y1)break;
                int e2=e*2;if(e2>=dy){e+=dy;x+=sx;}
                if(e2<=dx){e+=dx;y+=sy;}
            }
            w->cursor_x=x1;w->cursor_y=y1;compose(r);return c->d[0];
        }
        case 324:set_pixel(r,w,(int16_t)c->d[0],(int16_t)c->d[1]);compose(r);return 0;
        case 318:{ /* ReadPixel */
            int x=(int16_t)c->d[0],y=(int16_t)c->d[1];
            if(x<0||y<0||x>=w->width||y>=w->height)return UINT32_MAX;
            if(!w->surface)return UINT32_MAX;
            return w->surface[(size_t)y*(unsigned)w->width+(unsigned)x];
        }
        default:*known=false;return 0;
    }
}
bool qrx68_enable_gui(QRX68 *r){
    if(!r||!r->memory)return false;
    if(!r->framebuffer){
        r->framebuffer=calloc((size_t)QRX68_FB_WIDTH*QRX68_FB_HEIGHT,1);
        if(!r->framebuffer)return false;
    }
    r->experimental_gui=true;
    return true;
}
static const uint8_t qrx68_palette[16][3]={
    {0,0,0},{255,255,255},{238,44,44},{44,220,44},
    {64,96,244},{244,210,32},{32,210,210},{232,44,220},
    {105,105,105},{190,190,190},{150,32,32},{32,130,32},
    {40,60,160},{145,100,22},{20,130,130},{130,20,130}
};
bool qrx68_send_gpu_frame(const QRX68 *r,const char *socket_path){
    if(!r||!r->framebuffer||!socket_path||!*socket_path)return false;
    const size_t n=(size_t)QRX68_FB_WIDTH*QRX68_FB_HEIGHT;
    uint32_t *rgba=malloc(n*sizeof(*rgba));if(!rgba)return false;
    for(size_t i=0;i<n;i++){
        const uint8_t *c=qrx68_palette[r->framebuffer[i]&15u];
        rgba[i]=((uint32_t)c[0]<<16)|((uint32_t)c[1]<<8)|c[2];
    }
    bool ok=qrx86_ipc_send(socket_path,rgba,QRX68_FB_WIDTH,QRX68_FB_HEIGHT);
    free(rgba);return ok;
}
bool qrx68_save_ppm(const QRX68 *r,const char *path){
    if(!r || !r->framebuffer || !path || !*path)return false;
    FILE *f=fopen(path,"wb");if(!f)return false;
    if(fprintf(f,"P6\n%u %u\n255\n",QRX68_FB_WIDTH,QRX68_FB_HEIGHT)<0){fclose(f);return false;}
    bool ok=true;
    for(size_t i=0;i<(size_t)QRX68_FB_WIDTH*QRX68_FB_HEIGHT;i++)
        if(fwrite(qrx68_palette[r->framebuffer[i]&15u],1,3,f)!=3){ok=false;break;}
    if(fclose(f))ok=false;
    return ok;
}
/* Called by BOTH the portable subset interpreter and the full Musashi core.
 * Per-instruction state remains in QRXCPU for deterministic ABI handling. */
int qrx68_boundary(QRX68 *r){
    if(!r || !r->memory || r->complete || r->failed)return -1;
    r->yielded=false;
    QRXCPU *c=&r->vm.cpu;
    if(c->pc==QRX68_RETURN_PC){r->complete=1;return -1;}
    uint32_t pc=c->pc;
    if((pc&0xFFFF0000u)==QRX68_TRAP_BASE ||
       (pc&0xFFFF0000u)==(QRX68_TRAP_BASE|0x10000u)){
        unsigned lib=(pc>>12)&15u,lvo=(pc&0xfffu)*6u;
        if(!lvo || lvo>VECTOR_COUNT*6u ||
           (lib!=LIB_EXEC && lib!=LIB_DOS && lib!=LIB_GFX && lib!=LIB_INTUITION) ||
           (lib==LIB_DOS && !r->dos_open_count) ||
           (lib==LIB_GFX && !r->gfx_open_count) ||
           (lib==LIB_INTUITION && !r->intuition_open_count)){
            unsupported(r,lib,lvo);return -1;
        }
        /* WaitPort must not RET or consume a guest instruction until an
         * event is available. Host scheduler polls the bridge and resumes. */
        if(lib==LIB_EXEC && lvo==384 && port_find(r,c->a[0]) &&
           !next_message(r,c->a[0])){
            r->yielded=true;return 2;
        }
        if(lib==LIB_EXEC && lvo==318 && c->d[0] &&
           !(r->signal_pending&c->d[0])){
            r->signal_wait=c->d[0];sync_task(r);
            r->yielded=true;return 2;
        }
        bool known=false;uint32_t value=0;
        if(lib==LIB_EXEC)value=exec_api(r,lvo,&known);
        if(lib==LIB_DOS)value=dos_api(r,lvo,&known);
        if(lib==LIB_GFX)value=graphics_api(r,lvo,&known);
        if(lib==LIB_INTUITION)value=intuition_api(r,lvo,&known);
        if(!known){unsupported(r,lib,lvo);return -1;}
        c->d[0]=value;r->hle_calls++;r->steps++;
        if(lib==LIB_EXEC)sync_task(r);
        if(r->complete)return -1; /* dos.library/Exit does not RTS */
        if(!hle_ret(r)){r->failed=1;return -1;}
        return 1;
    }
    if((pc&0xffff0000u)==(QRX68_RETURN_PC&0xffff0000u)){
        r->failed=1;return -1;
    }
    return 0;
}
bool qrx68_step(QRX68 *r){
    int boundary=qrx68_boundary(r);
    if(boundary!=0)return boundary>0;
    if(!qrx_vm_step(&r->vm)){r->failed=1;return false;}
    r->steps++;return true;
}
int qrx68_run(QRX68 *r,uint64_t budget){
    if(!r || !budget)return -1;
    for(uint64_t i=0;i<budget;i++){
        if(r->complete)return 0;
        if(r->failed)return -1;
        (void)qrx68_step(r);
        if(r->yielded)return 2; /* waiting on real IDCMP event */
    }
    return r->complete?0:r->failed?-1:1;
}
