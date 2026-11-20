#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* dladdr on Linux */
#endif
#include "qrx86_door.h"
#include "qrx_netpolicy.h"
#include "qrx_vault.h"
#include "qrx_hostmidi.h"
void qrx_host_set_pointer(uint32_t index);
#include "qrx_hostnet.h"
#include "qrx_gl68k.h"
/* The host build of the same interpreter the guest runs, with its internals in
 * reach: the door keeps a shadow page table of host pointers. */
#define QRX86_HOST_JIT 1
#include "qrx86core.c"
#include "qrx_hostapi.h"
#include "qrxmedia.h"
#include <math.h>
#define QW_THUNKS 4096u

/* struct door_req on the guest side; all fields big-endian. */
enum {
    REQ_R=0, REQ_EIP=32, REQ_EFLAGS=36, REQ_SREG=40, REQ_SBASE=64, REQ_TLS=88,
    REQ_ST=124, REQ_TOP=188, REQ_FSW=192, REQ_FCW=196, REQ_FTW=200,
    REQ_DIR=204, REQ_SERIAL=208, REQ_HLE_BASE=212, REQ_HLE_COUNT=216,
    REQ_STOP=220, REQ_LIMIT=224, REQ_EXECUTED=228, REQ_STATUS=232, REQ_ALLOC=236, REQ_ALLOC_ADDR=240, REQ_ALLOC_DONE=244, REQ_SIZE=248
};
static x86cpu host;
static uint32_t shadow_serial, shadow_dir;
static bool shadow_valid;
static uint64_t door_calls, door_x86;
#include "qrx86_hostjit.c"
#ifndef QRX86_HOST_JIT_ACTIVE
static void x86_host_code_write(x86cpu *c, uint32_t a) { (void)c; (void)a; }
#endif

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
            if(t){
#ifdef QRX86_HOST_JIT_ACTIVE
                for(unsigned j=0;j<1024;j++){
                    if(t[j].flags&X86_PG_CODE)xj_kill_page((i<<10)|j);
                    t[j].mem=NULL;t[j].flags=0;
                    xj_table_page((i<<10)|j,&t[j]);
                }
#endif
                free(t);host.dir[i]=NULL;
            }
            continue;
        }
        if((uint64_t)gt+1024u*8u>size)continue;
        if(!t){t=calloc(1024,sizeof(x86page));host.dir[i]=t;if(!t)continue;}
        for(unsigned j=0;j<1024;j++){
            uint32_t mem=rd32(ram+gt+j*8u),flags=rd32(ram+gt+j*8u+4u);
            uint8_t *host_mem=(mem&&(uint64_t)mem+X86_PAGE_SIZE<=size)?(uint8_t*)ram+mem:NULL;
#ifdef QRX86_HOST_JIT_ACTIVE
            /* Translations survive a remap only if the page stayed where it was. */
            if((t[j].flags&X86_PG_CODE)&&t[j].mem!=host_mem)xj_kill_page((i<<10)|j);
            flags|=t[j].flags&X86_PG_CODE;
#endif
            t[j].flags=flags;
            t[j].mem=host_mem;
#ifdef QRX86_HOST_JIT_ACTIVE
            xj_table_page((i<<10)|j,&t[j]);
#endif
        }
    }
    tlb_flush(&host);
    shadow_dir=dir;
    shadow_valid=true;
}
/* ---- Each Windows process in the guest has its own thunk numbers and sound voices. The
 * page directory tells them apart; only the process that ran last is heard. */
#define MIX_RATE 48000u
#define MIX_TARGET_FRAMES 2400u   /* 50 ms */
#define MIX_VOICES 128u
#define DOOR_PROCS 8u
typedef struct {
    bool used,playing,looping;
    uint32_t mem,size,rate,chans,bits;
    float gain_l,gain_r;
    double pos;                   /* in source frames */
} MixVoice;
typedef struct {
    uint32_t dir;
    const QRXHostApi *api_bound[QW_THUNKS];
    MixVoice voices[MIX_VOICES];
    unsigned voices_live;
} DoorProc;
static DoorProc door_procs[DOOR_PROCS];
static DoorProc *cur_proc=&door_procs[0];
static void proc_select(uint32_t dir){
    static unsigned victim;
    DoorProc *free_slot=NULL;
    if(cur_proc->dir==dir)return;
    for(unsigned i=0;i<DOOR_PROCS;i++){
        if(door_procs[i].dir==dir){cur_proc=&door_procs[i];return;}
        if(!door_procs[i].dir&&!free_slot)free_slot=&door_procs[i];
    }
    if(!free_slot){
        do{free_slot=&door_procs[victim];victim=(victim+1u)%DOOR_PROCS;}while(free_slot==cur_proc);
        memset(free_slot,0,sizeof(*free_slot));
    }
    free_slot->dir=dir;
    cur_proc=free_slot;
}
/* A new program: its page directory may sit where a finished one's did, and its code where
 * that one's code was, so nothing known about that address carries over. */
static void proc_begin(uint32_t dir){
    proc_select(dir);
    memset(cur_proc,0,sizeof(*cur_proc));
    cur_proc->dir=dir;
    shadow_valid=false;
#ifdef QRX86_HOST_JIT_ACTIVE
    if(xj.cpu)xj_flush_all();
#endif
    qrx_d3d_new_process();
}
/* Anything the guest must decide stops the run with eip on that instruction;
 * the guest interprets it, so its behaviour stays in exactly one place. */
static int door_hle_hook(x86cpu *h,uint32_t index){
    const QRXHostApi *f=index<QW_THUNKS?cur_proc->api_bound[index]:NULL;
    if(!f&&index<QW_THUNKS)f=qrx_d3d_thunks[index];
    if(!f){h->stop=1;return 1;}
    /* The guest told us this thunk is ours: stdcall (or cdecl with nargs<0). */
    uint32_t sp=h->r[X86_ESP],a[16];
    uint32_t ret=x86_rd32(h,sp);
    for(unsigned i=0;i<16;i++)a[i]=x86_rd32(h,sp+4u+i*4u);
    uint32_t result=f->fn(h,a);
    /* QRX_D3D_LOG=1: the Direct3D methods a game wants and the host lacks. */
    {extern uint32_t qrx_d3d_unimpl(x86cpu*,const uint32_t*);static int log_on=-1;
     if(log_on<0)log_on=getenv("QRX_D3D_LOG")!=NULL;
     if(log_on&&f->fn==qrx_d3d_unimpl)fprintf(stderr,"[D3D] %s is not implemented\n",f->name?f->name:"?");
     else if(log_on&&(result>>28)==8u)fprintf(stderr,"[D3D] %s failed: %08x\n",f->name?f->name:"?",(unsigned)result);}
    /* Needs guest memory: leave eip on the thunk, the door runs this again. */
    if(qrx_hostapi_alloc_request){h->stop=1;return 1;}
    h->r[X86_EAX]=result;
    h->r[X86_ESP]=sp+4u+(f->nargs>0?(uint32_t)f->nargs*4u:0u);
    h->eip=ret;
    return 1;
}
static int door_int_hook(x86cpu *h,int vector){(void)vector;h->eip=h->cur_eip;h->stop=1;return 1;}

/* ---- DirectSound mixer. Runs on the CPU thread between x86 quanta, so the shadow
 * page table never changes under it, and tops up the host ring to a fixed latency. */
static QRXFullCPU *door_cpu;
static inline const uint8_t *x86_page_ptr(uint32_t a){
    x86page *t=host.dir[a>>22];
    if(!t)return NULL;
    t+=(a>>12)&1023u;
    return t->mem;
}
static inline uint8_t x86_byte(uint32_t a){
    const uint8_t *p=x86_page_ptr(a);
    return p?p[a&X86_PAGE_MASK]:0;
}
static inline float voice_sample(const MixVoice *v,uint32_t frame,unsigned ch){
    uint32_t bytes=v->bits/8u;
    uint32_t a=v->mem+frame*v->chans*bytes+ch*bytes;
    if(bytes==1)return ((float)x86_byte(a)-128.f)/128.f;
    return (float)(int16_t)((uint16_t)x86_byte(a)|(uint16_t)x86_byte(a+1u)<<8)/32768.f;
}
static int stall_on=-1;
static void mix_tick(void){
    if(!cur_proc->voices_live||!door_cpu||!door_cpu->vm.media)return;
    QRXMediaDevice *m=door_cpu->vm.media;
    unsigned queued=qrx_media_audio_queued(m);
    if(stall_on>0){
        /* How empty the sound queue gets: 0 is an audible gap. */
        static unsigned low=MIX_TARGET_FRAMES,gaps,calls;
        if(queued<low)low=queued;
        if(!queued)gaps++;
        if(++calls>=20000){fprintf(stderr,"[AUDIO] lowest queue %u of %u frames, empty %u times in %u calls\n",low,MIX_TARGET_FRAMES,gaps,calls);low=MIX_TARGET_FRAMES;gaps=calls=0;}
    }
    if(queued>=MIX_TARGET_FRAMES)return;
    unsigned n=MIX_TARGET_FRAMES-queued;
    static float left[MIX_TARGET_FRAMES],right[MIX_TARGET_FRAMES];
    static uint32_t out[MIX_TARGET_FRAMES];
    memset(left,0,n*sizeof(float));memset(right,0,n*sizeof(float));
    for(unsigned vi=0;vi<MIX_VOICES;vi++){
        MixVoice *v=&cur_proc->voices[vi];
        if(!v->used||!v->playing||!v->size||!v->chans||!v->bits)continue;
        uint32_t frames=v->size/(v->chans*(v->bits/8u));
        if(!frames)continue;
        double step=(double)v->rate/MIX_RATE;
        for(unsigned i=0;i<n;i++){
            uint32_t f=(uint32_t)v->pos;
            float l=voice_sample(v,f,0),r=v->chans>1?voice_sample(v,f,1):l;
            left[i]+=l*v->gain_l;right[i]+=r*v->gain_r;
            v->pos+=step;
            if((uint32_t)v->pos>=frames){
                if(v->looping)v->pos-=frames;
                else{v->playing=false;v->pos=0;cur_proc->voices_live--;break;}
            }
        }
    }
    for(unsigned i=0;i<n;i++){
        float l=left[i],r=right[i];
        l=l>1.f?1.f:l<-1.f?-1.f:l;r=r>1.f?1.f:r<-1.f?-1.f:r;
        out[i]=(uint32_t)(uint16_t)(int16_t)(l*32767.f)<<16|(uint16_t)(int16_t)(r*32767.f);
    }
    qrx_media_push_audio(m,out,n);
}
static void voice_set(MixVoice *v,const uint8_t *q){
    v->mem=rd32(q+20);v->size=rd32(q+24);v->rate=rd32(q+28);v->chans=rd32(q+32);v->bits=rd32(q+36);
    if(!v->rate)v->rate=22050;
    if(v->bits!=8&&v->bits!=16)v->bits=16;
    if(v->chans<1||v->chans>2)v->chans=2;
    /* hundredths of a decibel; pan attenuates the opposite side */
    double vol=pow(10.0,(double)(int32_t)rd32(q+40)/2000.0);
    int32_t pan=(int32_t)rd32(q+44);
    v->gain_l=(float)(vol*(pan>0?pow(10.0,-(double)pan/2000.0):1.0));
    v->gain_r=(float)(vol*(pan<0?pow(10.0,(double)pan/2000.0):1.0));
    v->looping=rd32(q+48)!=0;
}
static void shadow_sync(QRXFullCPU *c,uint32_t dir,uint32_t serial);
static void sound_service(QRXFullCPU *c,uint8_t *q){
    uint32_t op=rd32(q+12),idx=rd32(q+16);
    if(idx>=MIX_VOICES)return;
    MixVoice *v=&cur_proc->voices[idx];
    door_cpu=c;
    switch(op){
    case 1:{ /* start */
        bool was=v->used&&v->playing;
        voice_set(v,q);
        v->used=true;v->playing=true;
        if(!was)cur_proc->voices_live++;
        mix_tick();
        break;
    }
    case 2: if(v->used&&v->playing){v->playing=false;cur_proc->voices_live--;} break;
    case 3:{ /* volume, pan, frequency, position+1 */
        uint32_t arg=rd32(q+52);
        voice_set(v,q);
        v->used=true;
        if(arg&&v->chans)v->pos=(double)(arg-1u)/(v->chans*(v->bits/8u));
        break;
    }
    case 4:{
        uint32_t pos=(uint32_t)v->pos*(v->used?v->chans*(v->bits/8u):0u);
        wr32(q+56,pos);wr32(q+60,v->used&&v->playing);
        break;
    }
    case 5: if(v->used&&v->playing)cur_proc->voices_live--; memset(v,0,sizeof(*v)); break;
    default: break;
    }
}
/* ---- host API binding */
static void api_service(QRXFullCPU *c,uint8_t *q){
    uint32_t op=rd32(q+12),idx=rd32(q+16);
    char name[64];
    memcpy(name,q+28,63);name[63]=0;
    const QRXHostApi *f=qrx_hostapi_find(name);
    qrx_hostapi_context(c,&host,rd32(q+20));
    if(f&&op==1&&idx<QW_THUNKS)cur_proc->api_bound[idx]=f;
    wr32(q+24,f?1u:0u);
}
/* ---- MIDI: played by the host's synthesizer */
#ifndef __APPLE__
void qrx_host_midi_short(uint32_t msg){(void)msg;}
void qrx_host_midi_sysex(const uint8_t *d,uint32_t n){(void)d;(void)n;}
void qrx_host_midi_reset(void){}
#ifndef QRX_SDL_FRONTEND   /* the SDL window sets the pointer itself */
void qrx_host_set_pointer(uint32_t i){(void)i;}
#endif
#endif
static void midi_service(QRXFullCPU *c,uint8_t *q){
    uint32_t op=rd32(q+12);
    door_cpu=c;
    if(op==1)qrx_host_midi_short(rd32(q+16));
    else if(op==2){
        static uint8_t buf[4096];
        uint32_t a=rd32(q+20),n=rd32(q+24);
        if(n>sizeof buf)n=sizeof buf;
        for(uint32_t i=0;i<n;i++)buf[i]=x86_byte(a+i);
        qrx_host_midi_sysex(buf,n);
    }
    else if(op==3)qrx_host_midi_reset();
}
/* ---- TCP/IP: the guest's buffers are in 68k RAM */
static void net_service(QRXFullCPU *c,uint8_t *q){
    uint32_t a[13]={0};
    for(unsigned i=0;i<12;i++)a[i]=rd32(q+16+i*4u);
    if((rd32(q+12)==QRX_NET_RANDOM||rd32(q+12)==QRX_NET_CONFIG)&&(uint64_t)a[1]+a[2]>c->vm.ram.size){wr32(q+16+8*4u,UINT32_MAX);return;}
    qrx_hostnet_call(rd32(q+12),a,c->ram_owned);
    /* The 13th word only for the clock: older guests have room for 12 */
    for(unsigned i=8;i<(rd32(q+12)==QRX_NET_TIME?13u:12u);i++)wr32(q+16+i*4u,a[i]);
}
/* ---- A whole frame from x86 memory into guest RAM. The 68k would need a load, a
 * table look-up and a store per pixel; the frame buffer the display reads is plain RAM. */
enum { FRAME_LUT8, FRAME_ARGB32, FRAME_BGRA32, FRAME_RGB16, FRAME_RGB16PC };
static void frame_service(QRXFullCPU *c,uint8_t *q){
    uint32_t src=rd32(q+12),pitch=rd32(q+16),w=rd32(q+20),h=rd32(q+24),bpp=rd32(q+28);
    uint32_t dst=rd32(q+32),bpr=rd32(q+36),fmt=rd32(q+40),pal_addr=rd32(q+44);
    size_t size=c->vm.ram.size;
    wr32(q+48,0);
    if(!w||!h||w>4096||h>4096||(bpp!=8&&bpp!=16&&bpp!=32)||fmt>FRAME_RGB16PC||bpr>0x10000u)return;
    if((uint64_t)dst+(uint64_t)bpr*(h-1u)+(uint64_t)w*4u>size)return;
    if(fmt==FRAME_LUT8&&bpp!=8)return;
    uint32_t pal[256];
    if(bpp==8&&fmt!=FRAME_LUT8){
        if(!pal_addr||(uint64_t)pal_addr+1024u>size)return;
        const uint8_t *p=c->ram_owned+pal_addr;
        for(unsigned i=0;i<256;i++)pal[i]=rd32(p+i*4u)&0xFFFFFFu;
    }
    uint8_t *row=c->ram_owned+dst;
    /* One row of source pixels, joined across x86 pages when they are not contiguous. */
    static uint8_t line[4096u*4u];
    uint32_t bytes=w*bpp/8u;
    for(uint32_t y=0;y<h;y++,row+=bpr){
        uint32_t a=src+y*pitch,done=0;
        const uint8_t *s=NULL;
        if((a&X86_PAGE_MASK)+bytes<=X86_PAGE_SIZE){
            const uint8_t *m=x86_page_ptr(a);
            if(m)s=m+(a&X86_PAGE_MASK);
        }
        if(!s){
            while(done<bytes){
                uint32_t at=a+done,k=X86_PAGE_SIZE-(at&X86_PAGE_MASK);
                const uint8_t *m=x86_page_ptr(at);
                if(k>bytes-done)k=bytes-done;
                if(m)memcpy(line+done,m+(at&X86_PAGE_MASK),k);else memset(line+done,0,k);
                done+=k;
            }
            s=line;
        }
        switch(fmt){
        case FRAME_LUT8: memcpy(row,s,w);break;
        case FRAME_ARGB32:
        case FRAME_BGRA32:{
            uint32_t *d=(uint32_t*)row;
            /* Stored bytes: A R G B, or B G R A. */
            if(bpp==8){
                if(fmt==FRAME_ARGB32)for(uint32_t x=0;x<w;x++)d[x]=__builtin_bswap32(pal[s[x]]|0xFF000000u);
                else for(uint32_t x=0;x<w;x++)d[x]=pal[s[x]]|0xFF000000u;
            }else if(bpp==16){
                const uint16_t *p=(const uint16_t*)s;
                for(uint32_t x=0;x<w;x++){
                    uint32_t v=p[x];
                    uint32_t r=(v>>11)&31u,g=(v>>5)&63u,b=v&31u;
                    uint32_t px=0xFF000000u|((r<<3|r>>2)<<16)|((g<<2|g>>4)<<8)|(b<<3|b>>2);
                    d[x]=fmt==FRAME_ARGB32?__builtin_bswap32(px):px;
                }
            }else{
                const uint32_t *p=(const uint32_t*)s;
                if(fmt==FRAME_ARGB32)for(uint32_t x=0;x<w;x++)d[x]=__builtin_bswap32(p[x]|0xFF000000u);
                else for(uint32_t x=0;x<w;x++)d[x]=p[x]|0xFF000000u;
            }
            break;
        }
        default:{
            uint16_t *d=(uint16_t*)row;
            bool swap=fmt==FRAME_RGB16;
            if(bpp==16){
                if(swap)for(uint32_t x=0;x<w;x++)d[x]=__builtin_bswap16(((const uint16_t*)s)[x]);
                else memcpy(row,s,w*2u);
            }else{
                for(uint32_t x=0;x<w;x++){
                    uint32_t v=bpp==8?pal[s[x]]:((const uint32_t*)s)[x];
                    uint16_t px=(uint16_t)(((v>>8)&0xF800u)|((v>>5)&0x07E0u)|((v>>3)&0x001Fu));
                    d[x]=swap?__builtin_bswap16(px):px;
                }
            }
            break;
        }
        }
    }
    wr32(q+48,1);
}
static void service_call(QRXFullCPU *c,uint32_t block){
    if((uint64_t)block+128u>c->vm.ram.size)return;
    uint8_t *q=c->ram_owned+block;
    uint32_t svc=rd32(q);
    /* Sent by the desktop without an x86 process around: no page table to look at. */
    if(svc==6){qrx_host_set_pointer(rd32(q+12));return;}
    if(svc==7){proc_begin(rd32(q+8));return;}
    /* TCP/IP for native 68k programs, which have no page directory to look at. */
    if(svc==4&&!rd32(q+8)){net_service(c,q);return;}
    /* OpenGL for native 68k programs. */
    if(svc==8){qrx_gl68k_service(c->ram_owned,c->vm.ram.size,q);return;}
    /* Users and their encrypted home volumes. */
    if(svc==QRX_VAULT_SERVICE){
        uint32_t a[13];
        for(unsigned i=0;i<13;i++)a[i]=rd32(q+16+i*4u);
        qrx_vault_call(rd32(q+12),a,c->ram_owned,c->vm.ram.size);
        for(unsigned i=8;i<13;i++)wr32(q+16+i*4u,a[i]);
        return;
    }
    shadow_sync(c,rd32(q+8),rd32(q+4));
    if(svc==1)sound_service(c,q);
    else if(svc==2)api_service(c,q);
    else if(svc==3)midi_service(c,q);
    else if(svc==4)net_service(c,q);
    else if(svc==5)frame_service(c,q);
}
static void shadow_sync(QRXFullCPU *c,uint32_t dir,uint32_t serial){
    proc_select(dir);
    if(!shadow_valid||dir!=shadow_dir||serial!=shadow_serial){
        shadow_serial=serial;
        shadow_rebuild(c,dir);
    }
}
/* QRX86_STALLS=1: every pause over 8 ms in x86 execution on stderr, with the guest task
 * that ran before it. Rhythm shows in the times. */
#if defined(__APPLE__)
#include <mach/mach_time.h>
static double stall_ms(uint64_t ticks){static mach_timebase_info_data_t tb;if(!tb.denom)mach_timebase_info(&tb);return (double)ticks*tb.numer/tb.denom/1e6;}
#else
/* Linux (Raspberry Pi): monotonic nanoseconds stand in for Mach absolute time */
static uint64_t mach_absolute_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static double stall_ms(uint64_t ticks){return (double)ticks/1e6;}
#endif
static uint64_t stall_exit;
static void stall_enter(QRXFullCPU *c){
    uint64_t now=mach_absolute_time();
    static uint64_t start;
    if(!start)start=now;
    if(stall_exit){
        double ms=stall_ms(now-stall_exit);
        if(ms>8.0)fprintf(stderr,"[STALL] t=%.3f guest %.1f ms\n",stall_ms(now-start)/1000.0,ms);
    }
    (void)c;
}
static void door_run(QRXFullCPU *c,uint32_t req){
    if(stall_on<0)stall_on=getenv("QRX86_STALLS")!=NULL;
    uint64_t stall_t0=0;
    if(stall_on){stall_enter(c);stall_t0=mach_absolute_time();}
    if((uint64_t)req+REQ_SIZE>c->vm.ram.size)return;
    uint8_t *q=c->ram_owned+req;
    uint32_t dir=rd32(q+REQ_DIR),serial=rd32(q+REQ_SERIAL);
    if((uint64_t)dir+4096u>c->vm.ram.size)return;
    shadow_sync(c,dir,serial);
    mix_tick();
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
    if(rd32(q+REQ_ALLOC_DONE))qrx_hostapi_alloc_result(rd32(q+REQ_ALLOC_ADDR),rd32(q+REQ_ALLOC_DONE));
    uint32_t limit=rd32(q+REQ_LIMIT);
    host.fault=X86_OK;host.stop=0;
    host.icount=0;host.left=limit;host.lbase=limit;host.budget=limit;
    uint32_t stop_eip=rd32(q+REQ_STOP);
#ifdef QRX86_HOST_JIT_ACTIVE
    /* Translated code assumes flat data segments and that the stop address, the
     * return sentinel of x86_call, is never inside it. */
    static int jit_off=-1;
    if(jit_off<0)jit_off=getenv("QRX86_NOJIT")!=NULL;
    if(!jit_off&&stop_eip>=0xFFFFFF00u&&!host.sbase[X86_DS]&&!host.sbase[X86_ES]&&!host.sbase[X86_SS]&&xj_init(c))
        (void)xj_run(&host,stop_eip);
    else
#endif
    (void)x86_run(&host,stop_eip);
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
    wr32(q+REQ_ALLOC,qrx_hostapi_alloc_request);
    qrx_hostapi_alloc_request=0;
    wr32(q+REQ_STATUS,(uint32_t)host.fault);
    door_calls++;door_x86+=executed;
    if(stall_on){
        stall_exit=mach_absolute_time();
        double ms=stall_ms(stall_exit-stall_t0);
        if(ms>8.0)fprintf(stderr,"[STALL] x86 quantum %.1f ms, %u instructions\n",ms,executed);
    }
}
volatile uint32_t qrx_host_view_size;
static bool door_read(QRXFullCPU *c,uint32_t addr,unsigned n,uint32_t *out){
    if(addr==QRX86_DOOR_BASE&&n==4){*out=QRX86_DOOR_MAGIC;return true;}
    if(addr==QRX86_DOOR_BASE+16u&&n==4){*out=QRX86_DOOR_FEATURE_NATIVE_NET|QRX86_DOOR_FEATURE_GL|(qrx_host_view_size?QRX86_DOOR_FEATURE_HOST_VIEW:0);return true;}
    if(addr==QRX86_DOOR_BASE+20u&&n==4){*out=qrx_host_view_size;return true;}
    /* Reading the size register opens the RAM above the boot-visible 32 MiB. */
    if(addr==QRX86_DOOR_BASE+8u&&n==4){*out=(uint32_t)qrx_full_show_high_ram(c);return true;}
    return false;
}
static bool door_write(QRXFullCPU *c,uint32_t addr,unsigned n,uint32_t value){
    if(addr==QRX86_DOOR_BASE+4u&&n==4){door_run(c,value);return true;}
    if(addr==QRX86_DOOR_BASE+12u&&n==4){service_call(c,value);return true;}
    return false;
}
void qrx86_door_attach(QRXFullCPU *c){
    x86_init(&host);
    (void)qrx_net_mode();          /* Tor or WireGuard start with the machine */
    c->door_read=door_read;c->door_write=door_write;
}
void qrx86_door_report(void){
#ifdef QRX86_HOST_JIT_ACTIVE
    xj_report();
#endif
    qrx_gl68k_report();
    if(door_calls)fprintf(stderr,"[QRX86-DOOR] calls=%llu x86=%llu\n",(unsigned long long)door_calls,(unsigned long long)door_x86);
}
