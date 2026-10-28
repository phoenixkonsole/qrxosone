#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
struct IntuitionBase *IntuitionBase;struct GfxBase *GfxBase;struct DosLibrary *DOSBase;
static void b(struct RastPort*r,UBYTE p,WORD a,WORD y,WORD c,WORD d){SetAPen(r,p);RectFill(r,a,y,c,d);}static void t(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s){SetAPen(r,p);Move(r,x,y);Text(r,(STRPTR)s,strlen(s));}
static void draw(struct Window*w){char q[96];struct RastPort*r=w->RPort;b(r,238,0,0,w->Width-1,w->Height-1);b(r,227,0,0,w->Width-1,28);b(r,226,0,27,w->Width-1,29);t(r,229,12,18,"QRX   htop 68K");b(r,226,w->Width-29,4,w->Width-6,23);t(r,238,w->Width-22,18,"X");t(r,226,14,52,"CPU  MEM  TASKS");sprintf(q,"Mem free %luK   Chip %luK   Fast %luK",AvailMem(MEMF_ANY)/1024UL,AvailMem(MEMF_CHIP)/1024UL,AvailMem(MEMF_FAST)/1024UL);t(r,229,14,76,q);t(r,230,14,104,"STATE    CPU%    MEM%    TASK");t(r,229,14,128,"RUN       --      --     QRXDesktop");t(r,229,14,148,"WAIT      --      --     Intuition / DOS tasks");t(r,230,14,w->Height-18,"F1 Help   F5 Tree   F6 Sort   F10 Quit");}
int main(void){struct Screen*s=0;struct Window*w=0;struct IntuiMessage*m;int run=1,drag=0;WORD dx=0,dy=0;IntuitionBase=(struct IntuitionBase*)OpenLibrary((STRPTR)"intuition.library",0);GfxBase=(struct GfxBase*)OpenLibrary((STRPTR)"graphics.library",0);DOSBase=(struct DosLibrary*)OpenLibrary((STRPTR)"dos.library",0);if(!IntuitionBase||!GfxBase||!DOSBase)goto out;s=LockPubScreen(0);if(!s)goto out;w=OpenWindowTags(0,WA_PubScreen,(ULONG)s,WA_Left,40,WA_Top,38,WA_Width,540,WA_Height,280,WA_Flags,WFLG_ACTIVATE|WFLG_BORDERLESS|WFLG_SMART_REFRESH|WFLG_REPORTMOUSE,WA_IDCMP,IDCMP_REFRESHWINDOW|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE,TAG_DONE);if(!w)goto out;draw(w);while(run){WaitPort(w->UserPort);while((m=(struct IntuiMessage*)GetMsg(w->UserPort))){ULONG c=m->Class;UWORD k=m->Code;WORD x=m->MouseX,y=m->MouseY;ReplyMsg((struct Message*)m);if(c==IDCMP_REFRESHWINDOW){BeginRefresh(w);draw(w);EndRefresh(w,1);}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTDOWN){if(x>w->Width-34&&y<28)run=0;else if(y<28){drag=1;dx=x;dy=y;}}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTUP)drag=0;else if(c==IDCMP_MOUSEMOVE&&drag)MoveWindow(w,x-dx,y-dy);}}out:if(w)CloseWindow(w);if(s)UnlockPubScreen(0,s);if(DOSBase)CloseLibrary((struct Library*)DOSBase);if(GfxBase)CloseLibrary((struct Library*)GfxBase);if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);return 0;}
