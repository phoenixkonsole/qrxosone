#include <exec/types.h>
#include <exec/libraries.h>
#include <intuition/intuition.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
struct IntuitionBase *IntuitionBase;struct GfxBase *GfxBase;struct DosLibrary *DOSBase;
static const char*tests[]={"QRX:C/QRXDynarecTest","QRX:C/QRX86Run QRX:Tests/hello-i386","QRX:C/QRXLinuxRun QRX:Tests/hello-linux","QRX:C/QRXWinRuntimeTest","QRX:C/QRXWineHostTest","QRX:C/QRXWineServicesTest","QRX:C/QRXWineDisplayTest","Execute QRX:S/TestWin32","Execute QRX:S/TestWine",0};static LONG result[12];
static void b(struct RastPort*r,UBYTE p,WORD a,WORD y,WORD c,WORD d){SetAPen(r,p);RectFill(r,a,y,c,d);}static void t(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s){SetAPen(r,p);Move(r,x,y);Text(r,(STRPTR)s,strlen(s));}
static void draw(struct Window*w){int i;char q[100];struct RastPort*r=w->RPort;b(r,238,0,0,w->Width-1,w->Height-1);b(r,227,0,0,w->Width-1,28);b(r,226,0,27,w->Width-1,29);t(r,229,12,18,"QRX   Compatibility Center");b(r,226,w->Width-29,4,w->Width-6,23);t(r,238,w->Width-22,18,"X");t(r,226,14,52,"QRX86 / Linux ABI / Win32 / Wine");for(i=0;tests[i];i++){snprintf(q,sizeof(q),"%s   %s",result[i]==-1?"FAIL":"PASS",tests[i]);t(r,result[i]==-1?234:236,14,78+i*24,q);}}
int main(void){struct Screen*s=0;struct Window*w=0;struct IntuiMessage*m;int run=1,drag=0,i;WORD dx=0,dy=0;IntuitionBase=(struct IntuitionBase*)OpenLibrary((STRPTR)"intuition.library",0);GfxBase=(struct GfxBase*)OpenLibrary((STRPTR)"graphics.library",0);DOSBase=(struct DosLibrary*)OpenLibrary((STRPTR)"dos.library",0);if(!IntuitionBase||!GfxBase||!DOSBase)goto out;for(i=0;tests[i];i++)result[i]=SystemTags((STRPTR)tests[i],SYS_Asynch,FALSE,TAG_DONE);s=LockPubScreen(0);if(!s)goto out;w=OpenWindowTags(0,WA_PubScreen,(ULONG)s,WA_Left,45,WA_Top,42,WA_Width,550,WA_Height,280,WA_Flags,WFLG_ACTIVATE|WFLG_BORDERLESS|WFLG_SMART_REFRESH|WFLG_REPORTMOUSE,WA_IDCMP,IDCMP_REFRESHWINDOW|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE,TAG_DONE);if(!w)goto out;draw(w);while(run){WaitPort(w->UserPort);while((m=(struct IntuiMessage*)GetMsg(w->UserPort))){ULONG c=m->Class;UWORD k=m->Code;WORD x=m->MouseX,y=m->MouseY;ReplyMsg((struct Message*)m);if(c==IDCMP_REFRESHWINDOW){BeginRefresh(w);draw(w);EndRefresh(w,1);}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTDOWN){if(x>w->Width-34&&y<28)run=0;else if(y<28){drag=1;dx=x;dy=y;}}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTUP)drag=0;else if(c==IDCMP_MOUSEMOVE&&drag)MoveWindow(w,x-dx,y-dy);}}out:if(w)CloseWindow(w);if(s)UnlockPubScreen(0,s);if(DOSBase)CloseLibrary((struct Library*)DOSBase);if(GfxBase)CloseLibrary((struct Library*)GfxBase);if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);return 0;}
