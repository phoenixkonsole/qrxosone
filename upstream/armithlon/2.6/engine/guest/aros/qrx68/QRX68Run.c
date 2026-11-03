/* QRX68Run: AROS/68k CLI bridge, invoked by the existing QRXDesktop.
 * Requires FS-UAE host-directory volume QRXBRIDGE: and a running QRX68 host
 * bridge service. Guest launches no host shell and receives no host pointers.
 * This is a compatibility viewer, NOT a native AROS exec.library replacement.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COPY_BUF_SIZE 8192u
#define MAX_HUNK_BYTES (64UL*1024UL*1024UL)
#define FB_W 640
#define FB_H 256
#define FRAME_BYTES (FB_W*FB_H*3u)

struct Library *CyberGfxBase = NULL;

static int file_prefix(const char *p) {
    BPTR f=Open((STRPTR)p,MODE_OLDFILE);
    UBYTE h[4]; LONG got;
    if(!f) return 0;
    got=Read(f,h,4);Close(f);
    return got==4&&h[0]==0&&h[1]==0&&h[2]==3&&h[3]==0xf3;
}
static int copy_hunk(const char *source,const char *target){
    BPTR in=0,out=0; UBYTE *buf=NULL;
    LONG n; ULONG total=0; int ok=0;
    in=Open((STRPTR)source,MODE_OLDFILE);
    if(!in) goto done;
    out=Open((STRPTR)target,MODE_NEWFILE);
    if(!out) goto done;
    buf=(UBYTE*)AllocVec(COPY_BUF_SIZE,MEMF_PUBLIC);
    if(!buf) goto done;
    while((n=Read(in,buf,COPY_BUF_SIZE))>0){
        total+=(ULONG)n;
        if(total>MAX_HUNK_BYTES || Write(out,buf,n)!=n) goto done;
    }
    if(n<0||total<20)goto done;
    ok=1;
done:
    if(buf)FreeVec(buf);
    if(in)Close(in);
    if(out)Close(out);
    if(!ok)DeleteFile((STRPTR)target);
    return ok;
}
static int publish_req(const char *id){
    char tmp[160],fin[160];BPTR f;
    const char *marker="QRX68/1\n";
    sprintf(tmp,"QRXBRIDGE:inbox/%s.req.part",id);
    sprintf(fin,"QRXBRIDGE:inbox/%s.req",id);
    f=Open((STRPTR)tmp,MODE_NEWFILE);
    if(!f)return 0;
    if(Write(f,(APTR)marker,8)!=8){Close(f);DeleteFile((STRPTR)tmp);return 0;}
    Close(f);
    if(!Rename((STRPTR)tmp,(STRPTR)fin)){DeleteFile((STRPTR)tmp);return 0;}
    return 1;
}
static LONG read_text(const char *p,char *buf,LONG cap){
    BPTR f=Open((STRPTR)p,MODE_OLDFILE);
    LONG n=0;
    if(!f)return -1;
    n=Read(f,buf,cap-1);
    Close(f);
    if(n<0)return -1;
    buf[n]=0;
    return n;
}
static void show_lines(struct RastPort *rp,const char *text,int *line,int start_y,int max_lines){
    const char *s=text;char buf[110];
    while(*s&&*line<max_lines){
        int n=0;
        while(*s&&*s!='\n'&&n<105){
            char c=*s++;
            buf[n++]=(c>=32 && c<127)?c:' ';
        }
        while(*s && *s!='\n')s++;
        if(*s=='\n')s++;
        if(n){
            SetAPen(rp,1);
            Move(rp,12,start_y+(*line)*15);
            Text(rp,(STRPTR)buf,(ULONG)n);
            (*line)++;
        }
    }
}
/* QRX68/2 interactive AROS Intuition viewer: reacts while guest is RUNNING.
 * Input events are copied through atomic files; guest HLE provides actual
 * IntuiMessage buffers at the Exec GetMsg/ReplyMsg ABI boundary.
 * This is NOT a replacement for AROS' own native window/layers subsystem. */
static void send_input(const char *id,ULONG seq,ULONG cls,UWORD code,
                       UWORD qual,WORD x,WORD y){
    char path[176],tmp[176],msg[120];BPTR f;LONG n;
    snprintf(path,sizeof(path),"QRXBRIDGE:inbox/%s.%010lu.evt",id,(unsigned long)seq);
    snprintf(tmp,sizeof(tmp),"%s.part",path);
    n=snprintf(msg,sizeof(msg),"QRX68/2 %lu %lu %u %u %d %d\n",
        0ul,(unsigned long)cls,(unsigned)code,(unsigned)qual,(int)x,(int)y);
    if(n<=0||n>=(LONG)sizeof(msg))return;
    f=Open((STRPTR)tmp,MODE_NEWFILE);if(!f)return;
    if(Write(f,msg,n)!=n){Close(f);DeleteFile((STRPTR)tmp);return;}
    Close(f);
    if(!Rename((STRPTR)tmp,(STRPTR)path))DeleteFile((STRPTR)tmp);
}
static void stop_guest(const char *id){
    char path[168],tmp[180];BPTR f;
    snprintf(path,sizeof(path),"QRXBRIDGE:inbox/%s.stop",id);
    snprintf(tmp,sizeof(tmp),"%s.part",path);
    f=Open((STRPTR)tmp,MODE_NEWFILE);if(!f)return;
    if(Write(f,(APTR)"QRX68/2 STOP\n",13)!=13){Close(f);DeleteFile((STRPTR)tmp);return;}
    Close(f);if(!Rename((STRPTR)tmp,(STRPTR)path))DeleteFile((STRPTR)tmp);
}
static void draw_frame(const char *id,struct Window *win,UBYTE *raw,UBYTE *argb){
    char path[160];BPTR f;LONG got;
    const char header[]="P6\n640 256\n255\n";
    char head[sizeof(header)-1];
    snprintf(path,sizeof(path),"QRXBRIDGE:results/%s.ppm",id);
    f=Open((STRPTR)path,MODE_OLDFILE);if(!f)return;
    got=Read(f,head,sizeof(head));
    if(got==(LONG)sizeof(head)&&memcmp(head,header,sizeof(head))==0)
        got=Read(f,raw,FRAME_BYTES);
    else got=-1;
    Close(f);
    if(got!=(LONG)FRAME_BYTES)return;
    if(!CyberGfxBase)return;
    for(ULONG i=0;i<(ULONG)FB_W*FB_H;i++){
        argb[i*4]=255;argb[i*4+1]=raw[i*3];
        argb[i*4+2]=raw[i*3+1];argb[i*4+3]=raw[i*3+2];
    }
    WritePixelArray(argb,0,0,FB_W*4u,win->RPort,12,20,FB_W,FB_H,RECTFMT_ARGB);
}
static int guest_view_live(const char *id){
    struct NewWindow w;struct Window *win;
    char path[160],line[256],shown[80]="",status[256];
    UBYTE *raw=NULL,*argb=NULL;
    ULONG seq=0,cycles=0;
    int finished=0,success=0,last_frame=-1,stop_sent=0;
    memset(&w,0,sizeof(w));
    w.LeftEdge=80;w.TopEdge=45;w.Width=FB_W+24;w.Height=FB_H+83;
    w.DetailPen=1;w.BlockPen=0;
    w.IDCMPFlags=IDCMP_CLOSEWINDOW|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|
                 IDCMP_RAWKEY|IDCMP_VANILLAKEY|IDCMP_REFRESHWINDOW;
    w.Flags=WFLG_CLOSEGADGET|WFLG_DRAGBAR|WFLG_DEPTHGADGET|
            WFLG_ACTIVATE|WFLG_SMART_REFRESH;
    w.Title=(STRPTR)"QRX68 - AmigaOS application (live)";
    w.Type=WBENCHSCREEN;
    win=OpenWindow(&w);
    if(!win){Printf("QRX68: could not open live Intuition window\n");return 0;}
    CyberGfxBase=OpenLibrary("cybergraphics.library",40);
    raw=(UBYTE*)AllocVec(FRAME_BYTES,MEMF_PUBLIC);
    argb=(UBYTE*)AllocVec(FB_W*FB_H*4u,MEMF_PUBLIC);
    SetAPen(win->RPort,1);Move(win->RPort,12,FB_H+39);
    Text(win->RPort,(STRPTR)"Starting QRX68/2...",20);
    for(;cycles<9000;cycles++){
        int refresh=0;
        struct IntuiMessage *m;
        while((m=(struct IntuiMessage*)GetMsg(win->UserPort))!=NULL){
            ULONG cls=m->Class;UWORD code=m->Code,qual=m->Qualifier;
            WORD x=m->MouseX,y=m->MouseY;
            ReplyMsg((struct Message*)m);
            if(cls==IDCMP_REFRESHWINDOW){refresh=1;continue;}
            if(cls==IDCMP_CLOSEWINDOW){
                if(!finished){send_input(id,++seq,IDCMP_CLOSEWINDOW,0,0,0,0);
                    /* Give the guest its normal chance to close itself. */
                    for(int n=0;n<20;n++){
                        char stpath[160],stbody[160];
                        snprintf(stpath,sizeof(stpath),"QRXBRIDGE:results/%s.status",id);
                        if(read_text(stpath,stbody,sizeof(stbody))>0){finished=1;break;}
                        Delay(2);
                    }
                    if(!finished){stop_guest(id);stop_sent=1;}
                }
                goto done;
            }
            if(!finished){
                if(cls==IDCMP_MOUSEBUTTONS||cls==IDCMP_MOUSEMOVE||
                   cls==IDCMP_RAWKEY||cls==IDCMP_VANILLAKEY){
                    if(seq<999999999ul)send_input(id,++seq,cls,code,qual,x-12,y-20);
                }
            }
        }
        snprintf(path,sizeof(path),"QRXBRIDGE:results/%s.frame",id);
        if(read_text(path,line,sizeof(line))>0){
            int frame=0;
            if(sscanf(line,"QRX68/2 FRAME %d",&frame)==1 &&
                frame>0 && (frame!=last_frame||refresh)){
                if(raw&&argb)draw_frame(id,win,raw,argb);
                last_frame=frame;
            }
        }
        snprintf(path,sizeof(path),"QRXBRIDGE:results/%s.status",id);
        if(!finished && read_text(path,status,sizeof(status))>0){
            finished=1;
            success=status[0]=='O'&&status[1]=='K'&&status[2]=='\n';
            snprintf(shown,sizeof(shown),"QRX68: %s - Close window to return",
                     success?"finished":"guest exited/error");
            SetAPen(win->RPort,1);Move(win->RPort,12,FB_H+39);
            Text(win->RPort,(STRPTR)shown,strlen(shown));
        }
        Delay(2); /* cooperative event polling; no busy-spin */
    }
    if(!finished){stop_guest(id);stop_sent=1;}
done:
    if(!finished&&!stop_sent)stop_guest(id);
    if(raw)FreeVec(raw);
    if(argb)FreeVec(argb);
    if(CyberGfxBase){CloseLibrary(CyberGfxBase);CyberGfxBase=NULL;}
    CloseWindow(win);
    return success;
}
static int reserve_request_id(char *id,size_t cap,const char *input){
    struct DateStamp ds;
    DateStamp(&ds);
    for(ULONG attempt=0;attempt<256;attempt++){
        char claim[160];BPTR reserved;
        unsigned long unique=(unsigned long)ds.ds_Tick ^ (unsigned long)(ULONG)input ^
                             (unsigned long)(attempt*7919u);
        snprintf(id,cap,"Q%08lX%08lX%08lX",(unsigned long)ds.ds_Days,
                 (unsigned long)ds.ds_Minute,unique);
        snprintf(claim,sizeof(claim),"QRXBRIDGE:inbox/%s.used",id);
        reserved=CreateDir((STRPTR)claim);
        if(reserved){UnLock(reserved);return 1;}
    }
    return 0;
}
int main(int argc,char **argv){
    char id[48],inpart[160],inf[160];
    int success=0;
    if(argc!=2 || !file_prefix(argv[1])){
        Printf("Usage: QRX68Run <Amiga HUNK program>\n");return 20;
    }
    if(!reserve_request_id(id,sizeof(id),argv[1])){
        Printf("QRX68: cannot reserve unique request ID in QRXBRIDGE:\n");
        return 20;
    }
    sprintf(inpart,"QRXBRIDGE:inbox/%s.hunk.part",id);
    sprintf(inf,"QRXBRIDGE:inbox/%s.hunk",id);
    sprintf(status,"QRXBRIDGE:results/%s.status",id);
    if(!copy_hunk(argv[1],inpart)||!Rename((STRPTR)inpart,(STRPTR)inf)){
        Printf("QRX68: QRXBRIDGE: not mounted or cannot send HUNK\n");
        DeleteFile((STRPTR)inpart);return 20;
    }
    if(!publish_req(id)){DeleteFile((STRPTR)inf);return 20;}
    /* Display opens immediately, while the real 68k guest is still running.
       The host writes .status last; quitting sends IDCMP_CLOSEWINDOW + STOP. */
    success=guest_view_live(id);
    return success?0:10;
}
