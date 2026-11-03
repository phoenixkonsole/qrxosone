/* AmigaDOS packet handler for QRXHostFS tree ABI. Runs inside AROS/m68k. */
#define __aros_getbase_SysBase() SysBase
#define __aros_getbase_DOSBase() DOSBase
#include <exec/execbase.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "qrxhostfs_guest.h"
#define PATHMAX 1024
#ifndef QRX_VOLUME_NAME
#define QRX_VOLUME_NAME "HOSTSYS"
#endif
struct Handle {struct FileLock lock; ULONG position; char path[PATHMAX];};
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
static struct MsgPort *port;
static struct DosList *volume;
static char pathbuf[PATHMAX],otherbuf[PATHMAX],entrybuf[256];
static ULONG host_size,host_result;
static LONG host(unsigned op,const char *path,void *data,ULONG offset,ULONG length){
    Forbid();
    ULONG status=qrxf_request(0x100+op,path,data,offset,length);
    host_size=qrxf_rd(QRXV_HFS_SIZE);host_result=qrxf_rd(QRXV_HFS_RESULT);
    Permit();
    if(!status)return 0;
    if(status==3)return ERROR_DISK_NOT_VALIDATED;
    if(status==2)return ERROR_OBJECT_NOT_FOUND;
    if(status==4)return ERROR_DISK_WRITE_PROTECTED;
    if(status==5)return ERROR_OBJECT_IN_USE;
    return ERROR_OBJECT_WRONG_TYPE;
}
static struct Handle *lockptr(BPTR lock){return lock?(struct Handle *)BADDR(lock):NULL;}
static int pathjoin(BPTR lock,BPTR name,char out[PATHMAX]){
    struct Handle *h=lockptr(lock);strcpy(out,h?h->path:"");
    UBYTE *b=BADDR(name);if(!b)return 1;
    char tmp[256];unsigned len=b[0];memcpy(tmp,b+1,len);tmp[len]=0;
    char *p=tmp,*colon=strchr(tmp,':');if(colon){out[0]=0;p=colon+1;}
    while(*p){
        if(*p=='/'){char *slash=strrchr(out,'/');if(slash)*slash=0;else out[0]=0;p++;continue;}
        char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p),at=strlen(out);
        if(at+n+2>=PATHMAX)return 0;
        if(at)out[at++]='/';memcpy(out+at,p,n);out[at+n]=0;
        if(!end)break;p=end+1;
    }
    return 1;
}
static struct Handle *newhandle(const char *path,LONG access){
    struct Handle *h=AllocVec(sizeof(*h),MEMF_PUBLIC|MEMF_CLEAR);
    if(h){strcpy(h->path,path);h->lock.fl_Key=(IPTR)h;h->lock.fl_Access=access;
        h->lock.fl_Task=port;h->lock.fl_Volume=MKBADDR(volume);}
    return h;
}
static LONG examine(const char *path,struct FileInfoBlock *fib,int next){
    ULONG index=next?(ULONG)fib->fib_DiskKey:0;LONG error;
    if(next){error=host(1,path,entrybuf,index,sizeof entrybuf);if(error==ERROR_OBJECT_NOT_FOUND)return ERROR_NO_MORE_ENTRIES;}
    else error=host(2,path,NULL,0,0);
    if(error)return error;
    int dir=next?(host_result&0x80000000u)!=0:host_result!=0;
    ULONG size=host_size;memset(fib,0,sizeof(*fib));fib->fib_DiskKey=next?index+1:0;
    fib->fib_DirEntryType=fib->fib_EntryType=dir?2:-3;fib->fib_Size=size;fib->fib_NumBlocks=(size+511)/512;
    const char *name=next?entrybuf:strrchr(path,'/');if(!next)name=name?name+1:path;
    /* DOS converts the packet-level BCPL name to a C string. */
    if(!*name)name=QRX_VOLUME_NAME;
    size_t len=strlen(name);if(len>sizeof(fib->fib_FileName)-2)len=sizeof(fib->fib_FileName)-2;
    fib->fib_FileName[0]=len;memcpy(fib->fib_FileName+1,name,len);
    return 0;
}
static void tracehex(ULONG n){const char *digits="0123456789abcdef";for(int shift=28;shift>=0;shift-=4)*(volatile UWORD*)0xdff030=0x100|digits[(n>>shift)&15];}
static void reply(struct DosPacket *p,LONG result,LONG error){
    if(error && error!=ERROR_OBJECT_NOT_FOUND && error!=ERROR_NO_MORE_ENTRIES){const char *s="[HFS-P] reply type=";while(*s)*(volatile UWORD*)0xdff030=0x100|*s++;tracehex(p->dp_Type);*(volatile UWORD*)0xdff030=0x120;tracehex(error);*(volatile UWORD*)0xdff030=0x120;tracehex(result);*(volatile UWORD*)0xdff030=0x10a;}
    struct MsgPort *dest=p->dp_Port;p->dp_Res1=result;p->dp_Res2=error;p->dp_Port=port;
    p->dp_Link->mn_Node.ln_Name=(char*)p;PutMsg(dest,p->dp_Link);
}
int qrx_handler(void){
    struct Process *me=(struct Process*)FindTask(NULL);port=&me->pr_MsgPort;
    WaitPort(port);struct Message *m=GetMsg(port);if(!m)return 20;
    struct DosPacket *p=(struct DosPacket*)m->mn_Node.ln_Name;
    if(qrxf_rd(0)!=QRXV_HFS_MAGIC || !(qrxf_rd(QRXV_HFS_FEATURES)&0x10)){
        reply(p,DOSFALSE,ERROR_DEVICE_NOT_MOUNTED);return 20;
    }
    struct DeviceNode *device=BADDR(p->dp_Arg3);if(device)device->dn_Task=port;
    volume=MakeDosEntry((STRPTR)QRX_VOLUME_NAME,DLT_VOLUME);
    if(!volume){reply(p,DOSFALSE,ERROR_NO_FREE_STORE);return 20;}
    volume->dol_Task=port;volume->dol_misc.dol_volume.dol_DiskType=ID_DOS_DISK;
    AddDosEntry(volume);reply(p,DOSTRUE,0);
    for(;;){
        WaitPort(port);
        while((m=GetMsg(port))){
            p=(struct DosPacket*)m->mn_Node.ln_Name;
            LONG result=DOSTRUE,error=0;struct Handle *h=NULL;BPTR lock=(BPTR)p->dp_Arg1;
            switch(p->dp_Type){
            case ACTION_IS_FILESYSTEM:break;
            case ACTION_LOCATE_OBJECT:
            case ACTION_CREATE_DIR:
                if(!pathjoin(lock,(BPTR)p->dp_Arg2,pathbuf)){error=ERROR_LINE_TOO_LONG;break;}
                error=host(p->dp_Type==ACTION_CREATE_DIR?7:2,pathbuf,NULL,0,0);
                if(!error){h=newhandle(pathbuf,p->dp_Arg3);if(h)result=MKBADDR(h);else error=ERROR_NO_FREE_STORE;}
                break;
            case ACTION_COPY_DIR:
            case ACTION_PARENT:
            case ACTION_COPY_DIR_FH:
            case ACTION_PARENT_FH:
                h=(p->dp_Type==ACTION_COPY_DIR_FH||p->dp_Type==ACTION_PARENT_FH)?(struct Handle*)p->dp_Arg1:lockptr(lock);strcpy(pathbuf,h?h->path:"");
                if(p->dp_Type==ACTION_PARENT||p->dp_Type==ACTION_PARENT_FH){if(!*pathbuf){result=0;break;}char *s=strrchr(pathbuf,'/');if(s)*s=0;else *pathbuf=0;}
                h=newhandle(pathbuf,SHARED_LOCK);if(h)result=MKBADDR(h);else error=ERROR_NO_FREE_STORE;break;
            case ACTION_FREE_LOCK:h=lockptr(lock);if(h)FreeVec(h);break;
            case ACTION_FH_FROM_LOCK:{
                struct FileHandle *fh=BADDR(p->dp_Arg1);h=lockptr(p->dp_Arg2);
                if(!h){error=ERROR_OBJECT_WRONG_TYPE;break;}
                fh->fh_Arg1=(IPTR)h;fh->fh_Type=port;break;}
            case ACTION_EXAMINE_FH:
                h=(struct Handle*)p->dp_Arg1;error=examine(h?h->path:"",BADDR(p->dp_Arg2),0);break;
            case ACTION_CHANGE_MODE:break;
            case ACTION_EXAMINE_OBJECT:
            case ACTION_EXAMINE_NEXT:
                h=lockptr(lock);error=examine(h?h->path:"",BADDR(p->dp_Arg2),p->dp_Type==ACTION_EXAMINE_NEXT);break;
            case ACTION_FINDINPUT:
            case ACTION_FINDOUTPUT:
            case ACTION_FINDUPDATE:{
                struct FileHandle *fh=BADDR(p->dp_Arg1);
                if(!pathjoin((BPTR)p->dp_Arg2,(BPTR)p->dp_Arg3,pathbuf)){error=ERROR_LINE_TOO_LONG;break;}
                error=host(p->dp_Type==ACTION_FINDOUTPUT?9:2,pathbuf,NULL,0,0);
                if(error==ERROR_OBJECT_NOT_FOUND&&p->dp_Type==ACTION_FINDUPDATE)error=host(9,pathbuf,NULL,0,0);
                if(!error && p->dp_Type!=ACTION_FINDOUTPUT && host_result)error=ERROR_OBJECT_WRONG_TYPE;
                if(!error){h=newhandle(pathbuf,SHARED_LOCK);if(!h)error=ERROR_NO_FREE_STORE;
                    else{fh->fh_Arg1=(IPTR)h;fh->fh_Type=port;}}
                break;}
            case ACTION_READ:
            case ACTION_WRITE:{
                h=(struct Handle*)p->dp_Arg1;UBYTE *data=(UBYTE*)p->dp_Arg2;ULONG length=p->dp_Arg3,done=0;
                while(done<length){ULONG chunk=length-done;if(chunk>4096)chunk=4096;
                    error=host(p->dp_Type==ACTION_READ?3:4,h->path,data+done,h->position,chunk);
                    if(error)break;ULONG n=host_result;done+=n;h->position+=n;if(n<chunk)break;}
                result=error?-1:(LONG)done;break;}
            case ACTION_SEEK:{
                h=(struct Handle*)p->dp_Arg1;error=host(2,h->path,NULL,0,0);if(error){result=-1;break;}
                LONG base=p->dp_Arg3==OFFSET_BEGINNING?0:p->dp_Arg3==OFFSET_CURRENT?h->position:host_size;
                LONG pos=base+p->dp_Arg2;if(pos<0){error=ERROR_SEEK_ERROR;result=-1;break;}
                result=h->position;h->position=pos;break;}
            case ACTION_END:h=(struct Handle*)p->dp_Arg1;if(h)FreeVec(h);break;
            case ACTION_DELETE_OBJECT:
                if(!pathjoin(lock,(BPTR)p->dp_Arg2,pathbuf)){error=ERROR_LINE_TOO_LONG;break;}
                error=host(5,pathbuf,NULL,0,0);break;
            case ACTION_RENAME_OBJECT:
                if(!pathjoin(lock,(BPTR)p->dp_Arg2,pathbuf)||!pathjoin((BPTR)p->dp_Arg3,(BPTR)p->dp_Arg4,otherbuf)){error=ERROR_LINE_TOO_LONG;break;}
                error=host(8,pathbuf,otherbuf,0,0);break;
            case ACTION_DISK_INFO:
            case ACTION_INFO:{
                struct InfoData *info=BADDR(p->dp_Type==ACTION_INFO?p->dp_Arg2:p->dp_Arg1);
                error=host(10,"",NULL,0,0);if(error)break;
                memset(info,0,sizeof(*info));info->id_DiskState=ID_VALIDATED;info->id_NumBlocks=host_size;
                info->id_NumBlocksUsed=host_result;info->id_BytesPerBlock=65536;info->id_DiskType=ID_DOS_DISK;
                info->id_VolumeNode=MKBADDR(volume);break;}
            case ACTION_CURRENT_VOLUME:result=MKBADDR(volume);break;
            case ACTION_SAME_LOCK:{struct Handle *a=lockptr(lock),*b=lockptr(p->dp_Arg2);
                result=!strcmp(a?a->path:"",b?b->path:"")?LOCK_SAME:LOCK_SAME_VOLUME;break;}
            case ACTION_FLUSH:break;
            default:error=ERROR_ACTION_NOT_KNOWN;break;
            }
            if(error)result=(p->dp_Type==ACTION_READ || p->dp_Type==ACTION_WRITE || p->dp_Type==ACTION_SEEK)?-1:DOSFALSE;
            reply(p,result,error);
        }
    }
}

/* Handler entry has no Workbench/CLI C-runtime startup: the first message is
 * an ACTION_STARTUP packet and must remain owned by the handler. */
__attribute__((section(".text.entry"))) int _start(void){
    SysBase=*(struct ExecBase * volatile *)4;
    DOSBase=(struct DosLibrary*)OpenLibrary((STRPTR)"dos.library",0);
    if(!DOSBase)return 20;
    int result=qrx_handler();CloseLibrary((struct Library*)DOSBase);return result;
}
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,size_t n){unsigned char *p=d;while(n--)*p++=c;return d;}
size_t strlen(const char *s){size_t n=0;while(s[n])n++;return n;}
char *strcpy(char *d,const char *s){char *p=d;while((*p++=*s++));return d;}
char *strncpy(char *d,const char *s,size_t n){char *p=d;while(n&&*s){*p++=*s++;n--;}while(n--)*p++=0;return d;}
char *strchr(const char *s,int c){do{if(*s==c)return (char*)s;}while(*s++);return NULL;}
char *strrchr(const char *s,int c){const char *p=NULL;do{if(*s==c)p=s;}while(*s++);return (char*)p;}
int strcmp(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
