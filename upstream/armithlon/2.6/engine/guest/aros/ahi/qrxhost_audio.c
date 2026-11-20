/* qrxhost.audio: AHI sub-driver for the ARMithlon board. AHI mixes in the guest
 * and this driver hands each buffer to the host's audio ring by DMA; the host
 * plays it through CoreAudio at a fixed 48 kHz. Playback only.
 * Built with m68k-amigaos-gcc against the AHI headers, see build.sh, or with the AROS
 * m68k toolchain (build-aros.sh): AROS's gcc has no register parameters, so small
 * assembler stubs take the registers AHI passes and call the C functions with them on the
 * stack. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <devices/ahi.h>
#include <libraries/ahi_sub.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#ifdef __AROS__
/* launched from the shell by mistake: the first code returns at once */
__asm__(".text\n.even\nqrx_entry_guard:\n\tmoveq #-1,%d0\n\trts\n");
#endif
#define LIBNAME "qrxhost.audio"
#define VERSION 6
#define REVISION 0
#define HOST_RATE 48000u
/* Keep this much queued on the host: about 60 ms, the lowest level that stays
 * clean with AROS's 20 ms Delay() granularity. */
#define QUEUE_TARGET_FRAMES 2880u
#define DMA_MAX_FRAMES 4096u

/* QRX media board, 32-bit upper alias (see include/qrxmedia.h). */
#define MEDIA_AUDIO 0xFF000200u
#define REG_QUEUED (*(volatile ULONG*)(MEDIA_AUDIO+0x0Cu))
#define REG_DMA_ADDR (*(volatile ULONG*)(MEDIA_AUDIO+0x14u))
#define REG_DMA_FRAMES (*(volatile ULONG*)(MEDIA_AUDIO+0x18u))
#define REG_DMA_GO (*(volatile ULONG*)(MEDIA_AUDIO+0x1Cu))

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
#ifdef __AROS__
struct UtilityBase *UtilityBase;
#else
struct Library *UtilityBase;
#endif
static struct Library *DriverLibBase;
static BPTR DriverSegList;
struct QRXData {
    struct Process *master, *slave;
    BYTE master_signal, slave_signal;
    APTR mix;
};
static const char libname[]=LIBNAME;
static const char libid[]=LIBNAME " 1.0 (03.11.2026) ARMithlon host audio";

#ifdef __AROS__
#define R(reg)
#define USED __attribute__((used, noinline))
/* stub NAME_r: push the registers (last argument first), call NAME, drop them; d0 is the result */
#define STUB(name, pushes, bytes) __asm__(".text\n.even\n" #name "_r:\n" pushes "\tjsr " #name "\n\tlea " #bytes "(%sp),%sp\n\trts\n");
#define P(r) "\tmove.l %" #r ",-(%sp)\n"
#else
#define R(reg) __asm(#reg)
#define USED
#endif
static void submit(const UBYTE *buffer,ULONG frames){
    /* The board rejects a transfer beyond its ring, so wait for room first. */
    while(frames){
        ULONG n=frames>DMA_MAX_FRAMES?DMA_MAX_FRAMES:frames;
        REG_DMA_ADDR=(ULONG)buffer;
        REG_DMA_FRAMES=n;
        REG_DMA_GO=1;
        buffer+=n*4u;
        frames-=n;
    }
}
static void slave_entry(void){
    struct ExecBase *sys=*(struct ExecBase**)4;
    struct AHIAudioCtrlDrv *ctrl=(struct AHIAudioCtrlDrv*)((struct Task*)FindTask(NULL))->tc_UserData;
    struct QRXData *d=(struct QRXData*)ctrl->ahiac_DriverData;
    (void)sys;
    d->slave_signal=AllocSignal(-1);
    if(d->slave_signal!=-1){
        Signal((struct Task*)d->master,1UL<<d->master_signal);
        ULONG stop=(1UL<<d->slave_signal)|SIGBREAKF_CTRL_C;
        while(!(SetSignal(0,0)&stop)){
            if(REG_QUEUED>=QUEUE_TARGET_FRAMES){Delay(1);continue;}CallHookPkt(ctrl->ahiac_PlayerFunc,ctrl,NULL);CallHookPkt(ctrl->ahiac_MixerFunc,ctrl,d->mix);
            /* 16-bit stereo big-endian needs no conversion: AHIST_S16S */
            submit((const UBYTE*)d->mix,ctrl->ahiac_BuffSamples);
        }
        FreeSignal(d->slave_signal);
        d->slave_signal=-1;
    }
    Forbid();
    Signal((struct Task*)d->master,1UL<<d->master_signal);
    d->slave=NULL;
    /* Multitasking resumes when this process is gone. */
}

/* ---- AHIsub_* ---- */
static USED ULONG sub_alloc(struct TagItem *tags R(a1),struct AHIAudioCtrlDrv *ctrl R(a2)){
    (void)tags;
    struct QRXData *d=AllocVec(sizeof(*d),MEMF_PUBLIC|MEMF_CLEAR);
    ctrl->ahiac_DriverData=d;
    if(!d)return AHISF_ERROR;
    d->slave_signal=-1;
    d->master=(struct Process*)FindTask(NULL);
    d->master_signal=AllocSignal(-1);
    if(d->master_signal==-1)return AHISF_ERROR;
    return AHISF_KNOWSTEREO|AHISF_MIXING|AHISF_TIMING;
}
static USED void sub_free(struct AHIAudioCtrlDrv *ctrl R(a2)){
    struct QRXData *d=(struct QRXData*)ctrl->ahiac_DriverData;
    if(d){
        if(d->master_signal!=-1)FreeSignal(d->master_signal);
        FreeVec(d);
        ctrl->ahiac_DriverData=NULL;
    }
}
static USED void sub_disable(struct AHIAudioCtrlDrv *ctrl R(a2)){(void)ctrl;Forbid();}
static USED void sub_enable(struct AHIAudioCtrlDrv *ctrl R(a2)){(void)ctrl;Permit();}
static USED ULONG sub_stop(ULONG flags R(d0),struct AHIAudioCtrlDrv *ctrl R(a2));
static USED ULONG sub_start(ULONG flags R(d0),struct AHIAudioCtrlDrv *ctrl R(a2)){
    struct QRXData *d=(struct QRXData*)ctrl->ahiac_DriverData;sub_stop(flags,ctrl);
    if(flags&AHISF_RECORD)return AHIE_UNKNOWN;
    if(flags&AHISF_PLAY){
        d->mix=AllocVec(ctrl->ahiac_BuffSize,MEMF_PUBLIC);
        if(!d->mix)return AHIE_NOMEM;
        struct TagItem proc[]={{NP_Entry,(ULONG)slave_entry},{NP_Name,(ULONG)libname},
            {NP_Priority,10},{NP_StackSize,16384},{TAG_DONE,0}};
        Forbid();
        d->slave=CreateNewProc(proc);
        if(d->slave)d->slave->pr_Task.tc_UserData=ctrl;
        Permit();
        if(!d->slave)return AHIE_NOMEM;
        Wait(1UL<<d->master_signal);
        if(!d->slave)return AHIE_UNKNOWN;
    }
    return AHIE_OK;
}
static USED ULONG sub_update(ULONG flags R(d0),struct AHIAudioCtrlDrv *ctrl R(a2)){(void)flags;(void)ctrl;return 0;}
static USED ULONG sub_stop(ULONG flags R(d0),struct AHIAudioCtrlDrv *ctrl R(a2)){
    struct QRXData *d=(struct QRXData*)ctrl->ahiac_DriverData;
    if(flags&AHISF_PLAY){
        if(d->slave){
            if(d->slave_signal!=-1)Signal((struct Task*)d->slave,1UL<<d->slave_signal);
            Wait(1UL<<d->master_signal);
        }
        FreeVec(d->mix);
        d->mix=NULL;
    }
    return 0;
}
/* All channel work stays in AHI's own mixer. */
static ULONG sub_unknown(void){return AHIS_UNKNOWN;}
static USED LONG sub_hwcontrol(ULONG attr R(d0),LONG arg R(d1),struct AHIAudioCtrlDrv *ctrl R(a2)){
    (void)attr;(void)arg;(void)ctrl;return 0;
}
static USED ULONG sub_getattr(ULONG attr R(d0),LONG arg R(d1),ULONG defvalue R(d2),
                         struct TagItem *tags R(a1),struct AHIAudioCtrlDrv *ctrl R(a2)){
    (void)tags;(void)ctrl;switch(attr){
    case AHIDB_Bits:return 16;
    case AHIDB_Frequencies:return 1;
    case AHIDB_Frequency:return HOST_RATE;
    case AHIDB_Index:return arg==(LONG)HOST_RATE?0:(ULONG)defvalue;
    case AHIDB_Author:return (ULONG)"QRX OS One";
    case AHIDB_Copyright:return (ULONG)"(C) 2026 QRX";
    case AHIDB_Version:return (ULONG)libid;
    case AHIDB_Record:return FALSE;
    case AHIDB_Realtime:return TRUE;
    case AHIDB_MaxChannels:return 128;
    case AHIDB_Outputs:return 1;
#ifdef __AROS__
    case AHIDB_Output:return (ULONG)"Host";
#else
    case AHIDB_Output:return (ULONG)"Mac";
#endif
    case AHIDB_Inputs:return 0;
    default:return defvalue;
    }
}

/* ---- library plumbing ---- */
static BPTR lib_expunge_impl(struct Library *lib){
    BPTR seg=0;
    if(lib->lib_OpenCnt){lib->lib_Flags|=LIBF_DELEXP;return 0;}
    Remove(&lib->lib_Node);
    seg=DriverSegList;
    if(UtilityBase)CloseLibrary((struct Library*)UtilityBase);
    if(DOSBase)CloseLibrary((struct Library*)DOSBase);
    FreeMem((UBYTE*)lib-lib->lib_NegSize,lib->lib_NegSize+lib->lib_PosSize);
    return seg;
}
static USED struct Library *lib_open(struct Library *lib R(a6)){lib->lib_OpenCnt++;lib->lib_Flags&=~LIBF_DELEXP;return lib;
}
static USED BPTR lib_close(struct Library *lib R(a6)){
    if(--lib->lib_OpenCnt==0&&(lib->lib_Flags&LIBF_DELEXP))return lib_expunge_impl(lib);
    return 0;
}
static USED BPTR lib_expunge(struct Library *lib R(a6)){return lib_expunge_impl(lib);}
static ULONG lib_null(void){return 0;}
static USED struct Library *lib_init(struct Library *lib R(d0),BPTR seg R(a0),struct ExecBase *sys R(a6)){
    SysBase=sys;
    DriverSegList=seg;
    DriverLibBase=lib;
    lib->lib_Node.ln_Type=NT_LIBRARY;
    lib->lib_Node.ln_Name=(char*)libname;
    lib->lib_Flags=LIBF_SUMUSED|LIBF_CHANGED;
    lib->lib_Version=VERSION;
    lib->lib_Revision=REVISION;
    lib->lib_IdString=(APTR)libid;
    DOSBase=(struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library",37);
    UtilityBase=(void*)OpenLibrary((CONST_STRPTR)"utility.library",37);
    if(!DOSBase||!UtilityBase)return NULL;
    return lib;
}
#ifdef __AROS__
STUB(lib_open, P(a6), 4)
STUB(lib_close, P(a6), 4)
STUB(lib_expunge, P(a6), 4)
STUB(lib_init, P(a6) P(a0) P(d0), 12)
STUB(sub_alloc, P(a2) P(a1), 8)
STUB(sub_free, P(a2), 4)
STUB(sub_disable, P(a2), 4)
STUB(sub_enable, P(a2), 4)
STUB(sub_start, P(a2) P(d0), 8)
STUB(sub_update, P(a2) P(d0), 8)
STUB(sub_stop, P(a2) P(d0), 8)
STUB(sub_hwcontrol, P(a2) P(d1) P(d0), 12)
STUB(sub_getattr, P(a2) P(a1) P(d2) P(d1) P(d0), 20)
extern char lib_open_r[], lib_close_r[], lib_expunge_r[], lib_init_r[], sub_alloc_r[], sub_free_r[],
    sub_disable_r[], sub_enable_r[], sub_start_r[], sub_update_r[], sub_stop_r[], sub_hwcontrol_r[],
    sub_getattr_r[];
#define E(f) (APTR)f##_r
#else
#define E(f) (APTR)f
#endif
static const APTR functions[]={
    E(lib_open),E(lib_close),E(lib_expunge),lib_null,
    E(sub_alloc),E(sub_free),E(sub_disable),E(sub_enable),E(sub_start),E(sub_update),E(sub_stop),
    sub_unknown,sub_unknown,sub_unknown,sub_unknown,sub_unknown,sub_unknown,
    E(sub_getattr),E(sub_hwcontrol),(APTR)-1
};
static const ULONG auto_table[4]={sizeof(struct Library),(ULONG)functions,0,(ULONG)E(lib_init)};
const struct Resident romtag={
    RTC_MATCHWORD,(struct Resident*)&romtag,(APTR)(&romtag+1),RTF_AUTOINIT,VERSION,
    NT_LIBRARY,0,(char*)libname,(char*)libid,(APTR)auto_table
};
/* A stray launch from the shell must do nothing. */
int entry_guard(void){return -1;}
