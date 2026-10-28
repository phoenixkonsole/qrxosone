#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <graphics/rastport.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "qrx_theme.h"
struct IntuitionBase *IntuitionBase=0; struct GfxBase *GfxBase=0; struct DosLibrary *DOSBase=0; struct Library *AslBase=0;
static int dirty=0,maxed=0,backed=0,scroll=0; static WORD ox,oy,ow,oh; static char status[96]="Ready";
static void box(struct RastPort*r,UBYTE p,WORD a,WORD b,WORD c,WORD d){SetAPen(r,qrx_theme_legacy_pen(p));RectFill(r,a,b,c,d);} static void txt(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s){SetAPen(r,qrx_theme_legacy_pen(p));Move(r,x,y);Text(r,(STRPTR)s,strlen(s));}
static void btn(struct RastPort*r,WORD x,WORD y,WORD w,const char*s){qrx_theme_button(r,x,y,w,s);}
static WORD wrap(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s,int cols,WORD maxy){char b[96];int n,c;while(*s&&y<maxy){while(*s==' ')s++;n=strlen(s);if(n>cols)n=cols;c=n;if(s[n]&&n==cols){while(c>10&&s[c]!=' ')c--;if(c<=10)c=n;}memcpy(b,s,c);b[c]=0;txt(r,p,x,y,b);s+=c;y+=12;}return y;}
static void frame(struct Window*w,const char*title){qrx_theme_frame(w,title);if(qrx_theme_mode==QRX_THEME_QRX)txt(w->RPort,2,150,18,"File   Edit   View   Insert   Format   Help");}
static int qrx_file_request(struct Window*w,int save,char*dest,int cap){
 struct FileRequester*fr;if(!AslBase)return 0;
 fr=(struct FileRequester*)AllocAslRequestTags(ASL_FileRequest,
   ASLFR_Window,w,ASLFR_TitleText,save?"QRX Save File":"QRX Open File",
   ASLFR_InitialDrawer,"QRX:Documents",ASLFR_DoSaveMode,save?TRUE:FALSE,TAG_DONE);
 if(!fr)return 0;
 if(AslRequestTags(fr,TAG_DONE)){
   const char*d=fr->fr_Drawer?fr->fr_Drawer:"QRX:Documents";
   const char*f=fr->fr_File?fr->fr_File:"";
   int dl=strlen(d);snprintf(dest,cap,"%s%s%s",d,(dl&&d[dl-1]!=':'&&d[dl-1]!='/')?"/":"",f);
   FreeAslRequest(fr);return 1;
 }
 FreeAslRequest(fr);return 0;
}
static int savebuf(const char*path,const char*b,LONG n){BPTR f=Open((STRPTR)path,MODE_NEWFILE);if(!f){strcpy(status,"Save failed");return 0;}Write(f,(APTR)b,n);Close(f);dirty=0;snprintf(status,sizeof(status),"Saved: %s",path);return 1;}
static LONG loadbuf(const char*path,char*b,LONG cap){BPTR f=Open((STRPTR)path,MODE_OLDFILE);LONG n;if(!f){strcpy(status,"Open failed / file not found");return -1;}n=Read(f,b,cap-1);Close(f);if(n<0)n=0;b[n]=0;dirty=0;snprintf(status,sizeof(status),"Opened: %s",path);return n;}
static void chrome_click(struct Window*w,WORD x,WORD y,int*run){if(y>28)return;if(qrx_theme_close_hit(w,x,y)){*run=0;return;}if(qrx_theme_max_hit(w,x,y)){if(!maxed){ox=w->LeftEdge;oy=w->TopEdge;ow=w->Width;oh=w->Height;MoveWindow(w,-w->LeftEdge,30-w->TopEdge);SizeWindow(w,w->WScreen->Width-w->Width,w->WScreen->Height-30-w->Height);maxed=1;}else{MoveWindow(w,ox-w->LeftEdge,oy-w->TopEdge);SizeWindow(w,ow-w->Width,oh-w->Height);maxed=0;}return;}if(qrx_theme_back_hit(w,x,y)){WindowToBack(w);backed=1;return;}WindowToFront(w);ActivateWindow(w);}
static void scrollbar(struct Window*w,int pos,int total){qrx_theme_scrollbar(w->RPort,w->Width-12,82,w->Height-31,pos,total);}
#if defined(APP_WRITER)
#define TITLE "Writer"
static char doc[8192]="QRX Writer\n\nStart typing here. This is a native 68K document.";static int len=59;static char path[256]="QRX:Documents/Writer.txt";
static void draw(struct Window*w){struct RastPort*r=w->RPort;char line[72];int i=0,j,y=91,row=0;frame(w,TITLE);btn(r,8,34,44,"OPEN");btn(r,56,34,44,"SAVE");btn(r,104,34,34,"B");btn(r,142,34,34,"I");btn(r,180,34,50,"ALIGN");btn(r,234,34,54,"SIZE-");btn(r,292,34,54,"SIZE+");txt(r,2,8,72,"WYSIWYG PAGE");box(r,1,8,78,w->Width-16,w->Height-31);box(r,0,12,82,w->Width-20,w->Height-35);while(i<len&&y<w->Height-45){j=0;while(i<len&&doc[i]!='\n'&&j<60){line[j++]=doc[i++];}if(i<len&&doc[i]=='\n')i++;if(row++>=scroll){line[j]=0;txt(r,3,20,y,line);y+=13;}}scrollbar(w,scroll,row);txt(r,2,8,w->Height-13,status);}
static void key(UWORD k){if(k==8||k==127){if(len)doc[--len]=0;}else if((k==10||k==13)&&len<8190){doc[len++]='\n';doc[len]=0;}else if(k>=32&&k<=126&&len<8190){doc[len++]=(char)k;doc[len]=0;}dirty=1;}
static void click(struct Window*w,WORD x,WORD y){if(y>=34&&y<=55&&x<54){if(qrx_file_request(w,0,path,sizeof(path))){LONG n=loadbuf(path,doc,sizeof(doc));if(n>=0)len=n;}}else if(y>=34&&y<=55&&x>=56&&x<102){if(qrx_file_request(w,1,path,sizeof(path)))savebuf(path,doc,len);}else{WindowToFront(w);ActivateWindow(w);}}
#elif defined(APP_SHEET)
#define TITLE "Spreadsheet"
#define R 20
#define C 8
static char cells[R][C][32];static int sx=0,sy=0,clen=0;static char path[256]="QRX:Documents/Sheet.qrxsheet";
static double val(int r,int c){char*p=cells[r][c];double a,b;if(!*p)return 0;if(*p!='=')return atof(p);p++;if(!strncmp(p,"SUM(",4)){int c1=p[4]-'A',r1=atoi(p+5)-1;char*q=strchr(p,':');if(q){int c2=q[1]-'A',r2=atoi(q+2)-1;double z=0;int x,y;for(y=r1;y<=r2;y++)for(x=c1;x<=c2;x++)if(y>=0&&y<R&&x>=0&&x<C)z+=atof(cells[y][x]);return z;}}a=atof(p);while(*p&&((*p>='0'&&*p<='9')||*p=='.'))p++;if(!*p)return a;b=atof(p+1);if(*p=='+')return a+b;if(*p=='-')return a-b;if(*p=='*')return a*b;if(*p=='/'&&b!=0)return a/b;return a;}
static void sheet_save(void){savebuf(path,(char*)cells,sizeof(cells));}static void sheet_open(void){BPTR f=Open((STRPTR)path,MODE_OLDFILE);if(f){Read(f,cells,sizeof(cells));Close(f);strcpy(status,"Sheet opened");}else strcpy(status,"Open failed");}
static void draw(struct Window*w){struct RastPort*r=w->RPort;int x,y;char b[40];frame(w,TITLE);btn(r,8,34,44,"OPEN");btn(r,56,34,44,"SAVE");btn(r,104,34,54,"SUM");btn(r,162,34,54,"FORMAT");txt(r,2,8,72,"fx");txt(r,3,30,72,cells[sy][sx]);for(x=0;x<C;x++){sprintf(b,"%c",'A'+x);txt(r,2,62+x*58,91,b);}for(y=0;y<12;y++){sprintf(b,"%d",y+1+scroll);txt(r,2,8,111+y*23,b);for(x=0;x<C;x++){int rr=y+scroll;box(r,(x==sx&&rr==sy)?2:1,34+x*58,96+y*23,90+x*58,117+y*23);if(cells[rr][x][0]){if(cells[rr][x][0]=='=')snprintf(b,sizeof(b),"%.2f",val(rr,x));else strncpy(b,cells[rr][x],sizeof(b)-1),b[sizeof(b)-1]=0;txt(r,3,38+x*58,111+y*23,b);}}}scrollbar(w,scroll,R-12);txt(r,2,8,w->Height-13,status);}
static void key(UWORD k){char*p=cells[sy][sx];clen=strlen(p);if(k==8||k==127){if(clen)p[--clen]=0;}else if(k==13){if(sy<R-1)sy++;}else if(k>=32&&k<=126&&clen<30){p[clen++]=(char)k;p[clen]=0;}dirty=1;}
static void click(struct Window*w,WORD x,WORD y){if(y>=34&&y<=55&&x<54){if(qrx_file_request(w,0,path,sizeof(path)))sheet_open();}else if(y>=34&&y<=55&&x>=56&&x<102){if(qrx_file_request(w,1,path,sizeof(path)))sheet_save();}else if(y>=96&&x>=34){int cx=(x-34)/58,cy=(y-96)/23+scroll;if(cx>=0&&cx<C&&cy>=0&&cy<R){sx=cx;sy=cy;}}WindowToFront(w);ActivateWindow(w);}
#elif defined(APP_PRESENTER)
#define TITLE "Presenter"
#define SLIDES 16
static char slides[SLIDES][512];static int nslides=1,cur=0,plen=0;static char path[256]="QRX:Documents/Presentation.qrxpres";
static void pres_save(void){char all[SLIDES*512+32];int i;all[0]=0;for(i=0;i<nslides;i++){strcat(all,"---SLIDE---\n");strcat(all,slides[i]);strcat(all,"\n");}savebuf(path,all,strlen(all));}
static void pres_open(void){char all[SLIDES*512+32],*p,*q;LONG n=loadbuf(path,all,sizeof(all));if(n<0)return;nslides=0;p=all;while((p=strstr(p,"---SLIDE---\n"))&&nslides<SLIDES){p+=12;q=strstr(p,"\n---SLIDE---\n");if(q)*q=0;strncpy(slides[nslides++],p,511);if(!q)break;p=q+1;}if(nslides<1)nslides=1;cur=0;}
static void draw(struct Window*w){struct RastPort*r=w->RPort;int i;char b[32];frame(w,TITLE);btn(r,8,34,44,"OPEN");btn(r,56,34,44,"SAVE");btn(r,104,34,72,"+ SLIDE");btn(r,180,34,72,"- SLIDE");btn(r,256,34,72,"PRESENT");txt(r,2,8,72,"SLIDES");box(r,1,8,78,112,w->Height-31);for(i=0;i<nslides&&i<10;i++){box(r,i==cur?2:0,16,88+i*27,102,110+i*27);sprintf(b,"Slide %d",i+1);txt(r,3,25,104+i*27,b);}box(r,1,122,78,w->Width-16,w->Height-31);box(r,0,130,86,w->Width-24,w->Height-39);txt(r,2,150,112,"QRX PRESENTATION");wrap(r,3,150,140,slides[cur][0]?slides[cur]:"Type slide content here...",48,w->Height-55);scrollbar(w,0,0);txt(r,2,8,w->Height-13,status);}
static void key(UWORD k){char*p=slides[cur];plen=strlen(p);if(k==8||k==127){if(plen)p[--plen]=0;}else if((k==10||k==13)&&plen<510){p[plen++]='\n';p[plen]=0;}else if(k>=32&&k<=126&&plen<510){p[plen++]=(char)k;p[plen]=0;}dirty=1;}
static void click(struct Window*w,WORD x,WORD y){if(y>=34&&y<=55){if(x<54){if(qrx_file_request(w,0,path,sizeof(path)))pres_open();}else if(x<102){if(qrx_file_request(w,1,path,sizeof(path)))pres_save();}else if(x<178&&nslides<SLIDES){nslides++;cur=nslides-1;slides[cur][0]=0;strcpy(status,"Slide added");}else if(x<254&&nslides>1){int i;for(i=cur;i<nslides-1;i++)strcpy(slides[i],slides[i+1]);nslides--;if(cur>=nslides)cur=nslides-1;strcpy(status,"Slide removed");}else if(x<330)strcpy(status,"Presentation mode: use full-screen preview next milestone");}else if(x>=16&&x<=102&&y>=88){int i=(y-88)/27;if(i<nslides)cur=i;}WindowToFront(w);ActivateWindow(w);}
#elif defined(APP_HARDWARE)
#define TITLE "Hardware"
static void draw(struct Window*w){struct RastPort*r=w->RPort;char b[96];frame(w,TITLE);txt(r,2,16,54,"SYSTEM OVERVIEW");sprintf(b,"Free RAM: %lu KB",AvailMem(MEMF_ANY)/1024UL);txt(r,3,24,82,b);sprintf(b,"Chip RAM: %lu KB",AvailMem(MEMF_CHIP)/1024UL);txt(r,3,24,104,b);sprintf(b,"Fast RAM: %lu KB",AvailMem(MEMF_FAST)/1024UL);txt(r,3,24,126,b);txt(r,3,24,158,"AROS m68k / QRX native runtime");btn(r,24,190,150,"DIAGNOSTICS");}static void key(UWORD k){(void)k;}static void click(struct Window*w,WORD x,WORD y){if(y>=190&&y<=212)SystemTags((STRPTR)"QRX:C/QRXDiagnostics",SYS_Asynch,TRUE,TAG_DONE);WindowToFront(w);ActivateWindow(w);}
#elif defined(APP_SETTINGS)
#define TITLE "Settings"
static void draw(struct Window*w){struct RastPort*r=w->RPort;frame(w,TITLE);txt(r,2,18,54,"QRX CONTROL CENTER");txt(r,3,18,75,"AROS settings, grouped like a modern system panel");btn(r,20,96,205,"DISPLAY & GRAPHICS");btn(r,242,96,205,"SOUND / AHI");btn(r,20,132,205,"NETWORK");btn(r,242,132,205,"INPUT");btn(r,20,168,205,"LOCALE");btn(r,242,168,205,"SYSTEM / BOOT");wrap(r,3,20,225,"QRX uses the native AROS preference tools as its hardware configuration layer. Changes remain compatible with AROS applications.",58,w->Height-30);}static void key(UWORD k){(void)k;}static void click(struct Window*w,WORD x,WORD y){const char*c=0;if(y>=96&&y<=117)c=x<230?"SYS:Prefs/ScreenMode":"SYS:Prefs/AHI";else if(y>=132&&y<=153)c=x<230?"SYS:Prefs/Network":"SYS:Prefs/Input";else if(y>=168&&y<=189)c=x<230?"SYS:Prefs/Locale":"SYS:Prefs/Boot";if(c)SystemTags((STRPTR)c,SYS_Asynch,TRUE,TAG_DONE);WindowToFront(w);ActivateWindow(w);}
#elif defined(APP_WELCOME)
#define TITLE "Welcome to QRX OS One"
static void draw(struct Window*w){struct RastPort*r=w->RPort;WORD y;frame(w,TITLE);txt(r,2,18,54,"QRX OS ONE v0.20.3.7.4.6.6.13");y=82;y=wrap(r,3,20,y,"QRX OS One is a native 68K desktop built on AROS. It combines classic Amiga execution with a modern QRX environment, local AI, POSIX/Linux compatibility, QRX86 and the path toward Win32/Wine applications.",60,w->Height-100);y+=8;wrap(r,3,20,y,"Goal: a fast open operating system for real Amigas, accelerators and emulation without losing the character and efficiency of the platform.",60,w->Height-70);txt(r,2,20,w->Height-58,"Developer: Pascal Papara   Copyright 2026 QRX");txt(r,3,20,w->Height-39,"Thanks to the AROS Team, ReactOS, Wine and");txt(r,3,20,w->Height-23,"all Amiga developers around the globe.");}static void key(UWORD k){(void)k;}static void click(struct Window*w,WORD x,WORD y){(void)x;(void)y;WindowToFront(w);ActivateWindow(w);}
#else
#define TITLE "QRX System"
static void draw(struct Window*w){struct RastPort*r=w->RPort;char b[96];frame(w,TITLE);txt(r,2,16,54,"QRX SYSTEM PANEL");sprintf(b,"Free RAM: %lu KB",AvailMem(MEMF_ANY)/1024UL);txt(r,3,24,82,b);txt(r,3,24,108,"Use QRX Settings for AROS display, sound, network and input.");wrap(r,3,24,136,"QRX OS One is a native 68K environment built on AROS with classic Amiga execution and compatibility layers for POSIX, QRX86 and Win32/Wine.",58,w->Height-45);txt(r,2,16,w->Height-15,status);}static void key(UWORD k){(void)k;}static void click(struct Window*w,WORD x,WORD y){(void)x;(void)y;WindowToFront(w);ActivateWindow(w);}
#endif
int main(void){struct Screen*s=0;struct Window*w=0;struct IntuiMessage*m;int run=1,drag=0;WORD dx=0,dy=0,left,top,width=520,height=350;LONG dragx=0,dragy=0,dragleft=0,dragtop=0;IntuitionBase=(struct IntuitionBase*)OpenLibrary((STRPTR)"intuition.library",0);GfxBase=(struct GfxBase*)OpenLibrary((STRPTR)"graphics.library",0);DOSBase=(struct DosLibrary*)OpenLibrary((STRPTR)"dos.library",0);AslBase=OpenLibrary((STRPTR)"asl.library",0);if(!IntuitionBase||!GfxBase||!DOSBase)goto out;qrx_theme_load();s=LockPubScreen(0);if(!s)goto out;left=(s->Width-width)/2;top=(s->Height-height)/2;if(left<4)left=4;if(top<30)top=30;w=OpenWindowTags(0,WA_PubScreen,(ULONG)s,WA_Left,left,WA_Top,top,WA_Width,width,WA_Height,height,WA_MinWidth,400,WA_MinHeight,260,WA_MaxWidth,s->Width,WA_MaxHeight,s->Height-28,WA_Flags,WFLG_ACTIVATE|WFLG_BORDERLESS|WFLG_SMART_REFRESH|WFLG_RMBTRAP|WFLG_REPORTMOUSE,WA_IDCMP,IDCMP_REFRESHWINDOW|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_VANILLAKEY|IDCMP_NEWSIZE|IDCMP_INTUITICKS,TAG_DONE);if(!w)goto out;draw(w);while(run){WaitPort(w->UserPort);while((m=(struct IntuiMessage*)GetMsg(w->UserPort))){ULONG c=m->Class;UWORD k=m->Code;WORD x=m->MouseX,y=m->MouseY;ReplyMsg((struct Message*)m);if(c==IDCMP_REFRESHWINDOW){BeginRefresh(w);draw(w);EndRefresh(w,1);}else if(c==IDCMP_NEWSIZE)draw(w);else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTDOWN){WindowToFront(w);ActivateWindow(w);if(y<28){chrome_click(w,x,y,&run);if(qrx_theme_drag_hit(w,x,y)){drag=1;dx=x;dy=y;dragx=s->MouseX;dragy=s->MouseY;dragleft=w->LeftEdge;dragtop=w->TopEdge;}}else click(w,x,y);draw(w);}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTUP)drag=0;else if(c==IDCMP_INTUITICKS){WORD mx=w->MouseX,my=w->MouseY;if(my>=34&&my<=56)qrx_theme_tooltip(w,mx+10,my+12,"Toolbar action - click to activate");}
else if(c==IDCMP_MOUSEMOVE&&drag){LONG nl=dragleft+(LONG)s->MouseX-dragx,nt=dragtop+(LONG)s->MouseY-dragy;MoveWindow(w,nl-w->LeftEdge,nt-w->TopEdge);}else if(c==IDCMP_VANILLAKEY){key(k);draw(w);}}}out:if(w)CloseWindow(w);if(s)UnlockPubScreen(0,s);if(AslBase)CloseLibrary(AslBase);if(DOSBase)CloseLibrary((struct Library*)DOSBase);if(GfxBase)CloseLibrary((struct Library*)GfxBase);if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);return 0;}
