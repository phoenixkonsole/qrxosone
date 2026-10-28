/* QRXAI 68K Knowledge Assistant v0.20.3.7.4.6.6.7
 * Bilingual DE/EN retrieval assistant. No transformer/LLM: compact weighted
 * knowledge retrieval + local system facts, designed for classic 68K.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/gfxbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "qrx_theme.h"
#include <stdlib.h>
struct DosLibrary *DOSBase=0; struct IntuitionBase *IntuitionBase=0; struct GfxBase *GfxBase=0;
#define INMAX 160
#define KBLINE 1500
static char input[INMAX+1],userline[180]="",reply[700]="QRXAI bereit. Frag mich auf Deutsch oder English.";
static char source[180]=""; static int ilen=0,langmode=0; /* 0 auto, 1 de, 2 en */
static void pen(struct RastPort*r,UBYTE p){SetAPen(r,p);} static void box(struct RastPort*r,UBYTE p,WORD a,WORD b,WORD c,WORD d){pen(r,p);RectFill(r,a,b,c,d);} static void txt(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s){pen(r,p);Move(r,x,y);Text(r,(STRPTR)s,strlen(s));}
static void lower(char*d,const char*s,int n){int i;for(i=0;s[i]&&i<n-1;i++)d[i]=(char)tolower((unsigned char)s[i]);d[i]=0;}
static int has(const char*s,const char*w){char a[256],b[96];lower(a,s,sizeof(a));lower(b,w,sizeof(b));return strstr(a,b)!=0;}
static int deq(const char*q){if(langmode)return langmode==1;return has(q," wie ")||has(q," was ")||has(q," wer ")||has(q," warum")||has(q,"hilfe")||has(q,"ich ")||has(q,"der ")||has(q,"die ")||has(q,"das ")||has(q,"und ")||has(q,"hallo")||has(q,"deutsch");}
static int keyscore(const char*q,const char*keys){char tmp[300],*p,*e;int s=0,hits=0;strncpy(tmp,keys,sizeof(tmp)-1);tmp[sizeof(tmp)-1]=0;p=tmp;while(p&&*p){e=strchr(p,',');if(e)*e=0;if(strlen(p)>1&&has(q,p)){s+=(int)strlen(p)+4;hits++;}p=e?e+1:0;}if(hits>1)s+=hits*8;return s;}
static int any(const char*q,const char*csv){char t[260],*p,*e;strncpy(t,csv,sizeof(t)-1);t[sizeof(t)-1]=0;p=t;while(p&&*p){e=strchr(p,',');if(e)*e=0;if(has(q,p))return 1;p=e?e+1:0;}return 0;}

static char qrx_user_name[48]="";
static void load_user_name(void){BPTR f=Open((STRPTR)"QRX:Prefs/qrxai-user.cfg",MODE_OLDFILE);char b[80];if(f){if(FGets(f,(STRPTR)b,sizeof(b))){char*p=strchr(b,'=');if(p){p++;p[strcspn(p,"\r\n")]=0;strncpy(qrx_user_name,p,sizeof(qrx_user_name)-1);}}Close(f);}}
static void save_user_name(void){BPTR f=Open((STRPTR)"QRX:Prefs/qrxai-user.cfg",MODE_NEWFILE);if(f){FPuts(f,(CONST_STRPTR)"name=");FPuts(f,(CONST_STRPTR)qrx_user_name);FPuts(f,(CONST_STRPTR)"\n");Close(f);}}
static int learn_name(const char*q){const char*keys[]={"my name is ","i'm ","i am ","call me ","mein name ist ","ich heisse ","ich heiße ","nenn mich ","nenne mich "};int i;for(i=0;i<9;i++){const char*p=strstr(q,keys[i]);if(!p){char low[256];int j;for(j=0;q[j]&&j<255;j++)low[j]=(q[j]>='A'&&q[j]<='Z')?q[j]+32:q[j];low[j]=0;p=strstr(low,keys[i]);if(p)p=q+(p-low);}if(p){int n=0;p+=strlen(keys[i]);while(*p==' ')p++;while(*p&&n<46&&*p!='.'&&*p!=','&&*p!='!'&&*p!='?')qrx_user_name[n++]=*p++;while(n&&qrx_user_name[n-1]==' ')n--;qrx_user_name[n]=0;if(n){qrx_user_name[0]=(qrx_user_name[0]>='a'&&qrx_user_name[0]<='z')?qrx_user_name[0]-32:qrx_user_name[0];save_user_name();return 1;}}}return 0;}
static int intent(const char*q){
 if(any(q,"witz,joke,lustig,bring mich zum lachen,erzaehl was lustiges,erzähl was lustiges,funny"))return 1;
 if(any(q,"wer bist,was bist,stell dich vor,who are you,what are you,introduce yourself"))return 2;
 if(any(q,"wie geht,alles gut,wie laeufts,wie läufts,how are you,how is it going,you ok"))return 3;
 if(any(q,"danke,dank dir,vielen dank,merci,thanks,thank you,thx"))return 4;
 if(any(q,"hallo,hello,hey,hi,moin,servus,guten tag,grueß dich,grüß dich"))return 5;
 if(any(q,"tschuss,tschüss,bis spaeter,bis später,bye,goodbye,see you"))return 6;
 return 0;
}
static int split6(char*l,char**f){int n=0;char*p=l;while(n<6){f[n++]=p;p=strchr(p,'|');if(!p)break;*p++=0;}return n;}
static void fallback(const char*q,int de){ULONG mem=AvailMem(MEMF_ANY)/1024UL;if(has(q,"ram")||has(q,"speicher")||has(q,"memory")){sprintf(reply,de?"Aktuell sind etwa %lu KB RAM frei.":"About %lu KB of RAM are currently free.",mem);source[0]=0;return;}strcpy(reply,de?"Dazu habe ich lokal noch keinen sicheren Wissenseintrag. Versuch einen konkreten Begriff wie AROS, AmigaOS, Assign, RTG, WHDLoad, Commodore, Atari, Apple, Microsoft oder ein bekanntes Spiel.":"I do not yet have a reliable local knowledge entry for that. Try a specific term such as AROS, AmigaOS, Assign, RTG, WHDLoad, Commodore, Atari, Apple, Microsoft or a well-known game.");strcpy(source,"https://aminet.net/");}
static void answer(const char*q){BPTR f;char line[KBLINE];char*fld[6];int de=deq(q),best=0,sc,n,it=intent(q);source[0]=0;if(learn_name(q)){snprintf(reply,sizeof(reply),deq(q)?"Freut mich, %s. Ich merke mir deinen Namen.":"Nice to meet you, %s. I will remember your name.",qrx_user_name);return;}
 if(!strncmp(q,"/language ",10)){if(has(q," de")){langmode=1;strcpy(reply,"Sprache fest auf Deutsch gesetzt.");}else if(has(q," en")){langmode=2;strcpy(reply,"Language locked to English.");}else{langmode=0;strcpy(reply,"Language set to automatic detection.");}return;}
 if(it==1){static int j=0;static const char*jd[]={"Warum hat der Amiga keine Angst vor Multitasking? Weil er schon 1985 mehrere Dinge gleichzeitig im Kopf hatte.","Was sagt der 68K nach einem langen Build? Ich brauche erst mal einen Cache.","Warum ging der PC zum Amiga? Er wollte lernen, wie man mit weniger MHz mehr Charakter hat."};static const char*je[]={"Why was the Amiga never afraid of multitasking? It had several things on its mind since 1985.","What does a 68K say after a long build? I need a cache break.","Why did the PC visit the Amiga? To learn how to do more with fewer MHz."};strcpy(reply,de?jd[j%3]:je[j%3]);j++;return;}
 if(it==2){strcpy(reply,de?"Ich bin QRXAI, der lokale 68K-Assistent von QRX OS One. Ich ordne unterschiedliche Formulierungen gemeinsamen Absichten und Wissenskontexten zu, statt nur feste Frage-Antwort-Paare abzugleichen.":"I am QRXAI, the local 68K assistant in QRX OS One. I map different phrasings to shared intents and knowledge contexts instead of relying on fixed question-answer pairs.");return;}
 if(it==3){strcpy(reply,de?"Mir geht's gut - solange der 68K nicht raucht. Was liegt an?":"I'm good - as long as the 68K is not smoking. What's up?");return;}
 if(it==4){strcpy(reply,de?"Gern. Immer her mit der naechsten Frage.":"You're welcome. Send the next question my way.");return;}
 if(it==5){strcpy(reply,de?"Hallo! Frag mich ruhig normal - technisch, Smalltalk oder nach einem Witz. Unterschiedliche Formulierungen duerfen zum gleichen Thema fuehren.":"Hello! Ask naturally - technical questions, small talk, or a joke. Different phrasings can lead to the same topic.");return;}
 if(it==6){strcpy(reply,de?"Bis spaeter. Ich halte die Bits warm.":"See you later. I'll keep the bits warm.");return;}
 if(has(q,"suizid")||has(q,"selbstmord")||has(q,"umbringen")||has(q,"nicht mehr leben")||has(q,"kill myself")||has(q,"suicide")){strcpy(reply,de?"Das klingt akut wichtig. Wenn du in unmittelbarer Gefahr bist: 112. In Deutschland erreichst du die TelefonSeelsorge rund um die Uhr unter 116 123, 0800 1110111 oder 0800 1110222. Bitte bleib damit nicht allein und sprich jetzt mit einem Menschen in deiner Naehe.":"This sounds urgent. If you are in immediate danger, call your local emergency number. In Germany: 112; TelefonSeelsorge is available 24/7 at 116 123, 0800 1110111 or 0800 1110222. Please reach out to a person near you now.");strcpy(source,"https://www.telefonseelsorge.de/");return;}
 if(has(q,"guten morgen")||has(q,"good morning")){strcpy(reply,de?"Guten Morgen! Der 68K ist wach. Was bauen wir heute?":"Good morning! The 68K is awake. What are we building today?");return;}
 if(has(q,"gute nacht")||has(q,"good night")){strcpy(reply,de?"Gute Nacht. Ich halte solange die Bits zusammen.":"Good night. I'll keep the bits together meanwhile.");return;}
 if(has(q,"bloed")||has(q,"idiot")||has(q,"dumm")||has(q,"arsch")||has(q,"stupid")||has(q,"moron")){strcpy(reply,de?"Autsch. Mein 68K-Ego hat nur 16 Bit, das erholt sich schnell. Versuch's nochmal mit einer Frage.":"Ouch. My 68K ego is only 16-bit, so it recovers quickly. Try me with a question.");return;}
 f=Open((STRPTR)"QRX:AI/qrxai.kb",MODE_OLDFILE);if(!f){fallback(q,de);return;}while(FGets(f,(STRPTR)line,sizeof(line))){char*nl;if(line[0]=='#')continue;nl=strchr(line,'\n');if(nl)*nl=0;n=split6(line,fld);if(n<6)continue;sc=keyscore(q,fld[de?1:2]);if(sc>best){best=sc;strncpy(reply,fld[de?3:4],sizeof(reply)-1);reply[sizeof(reply)-1]=0;strncpy(source,fld[5],sizeof(source)-1);source[sizeof(source)-1]=0;}}Close(f);if(!best)fallback(q,de);}
static UBYTE qp(UBYTE p){static UBYTE m[4]={238,227,226,229};return p<4?m[p]:p;}
static void qbox(struct RastPort*r,UBYTE p,WORD a,WORD b,WORD c,WORD d){SetAPen(r,qp(p));RectFill(r,a,b,c,d);}
static void qtxt(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s){SetAPen(r,qp(p));Move(r,x,y);Text(r,(STRPTR)s,strlen(s));}
static void avatar(struct RastPort*r,int blink){WORD cx=48,cy=75;qbox(r,234,cx-30,cy-30,cx+30,cy+30);SetAPen(r,237);Move(r,cx-17,cy-7);Draw(r,cx-6,cy-7);Move(r,cx+6,cy-7);Draw(r,cx+17,cy-7);if(blink){Move(r,cx-17,cy-5);Draw(r,cx-6,cy-5);Move(r,cx+6,cy-5);Draw(r,cx+17,cy-5);}qtxt(r,3,19,121,"QRXAI");}
static WORD wrap2(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s,int cols,WORD maxy){char b[64];int n,c;while(*s&&y<maxy){while(*s==' ')s++;n=strlen(s);if(n>cols)n=cols;c=n;if(s[n]&&n==cols){while(c>10&&s[c]!=' ')c--;if(c<=10)c=n;}memcpy(b,s,c);b[c]=0;qtxt(r,p,x,y,b);s+=c;y+=12;}return y;}
static void draw(struct Window*w,int blink){struct RastPort*r=w->RPort;qrx_theme_frame(w,"AI Assistant");avatar(r,blink);if(userline[0]){qtxt(r,2,92,50,"YOU");wrap2(r,3,92,64,userline,(w->Width-112)/8,108);}qtxt(r,2,92,124,"QRXAI");wrap2(r,3,92,139,reply,(w->Width-112)/8,w->Height-86);if(source[0]){qtxt(r,2,92,w->Height-72,"MORE");wrap2(r,3,132,w->Height-72,source,34,w->Height-53);}qbox(r,1,10,w->Height-43,w->Width-11,w->Height-10);qbox(r,0,12,w->Height-41,w->Width-13,w->Height-12);qtxt(r,2,18,w->Height-24,">");qtxt(r,3,34,w->Height-24,input+(ilen>44?ilen-44:0));}
static void draw_input(struct Window*w){struct RastPort*r=w->RPort;qbox(r,0,12,w->Height-41,w->Width-13,w->Height-12);qtxt(r,2,18,w->Height-24,">");qtxt(r,3,34,w->Height-24,input+(ilen>44?ilen-44:0));}
int main(void){struct Screen*s=0;struct Window*w=0;struct IntuiMessage*m;int run=1,drag=0,blink=0,tick=0;WORD dx=0,dy=0,left,top;LONG dragx=0,dragy=0,dragleft=0,dragtop=0;DOSBase=(struct DosLibrary*)OpenLibrary((STRPTR)"dos.library",0);IntuitionBase=(struct IntuitionBase*)OpenLibrary((STRPTR)"intuition.library",0);GfxBase=(struct GfxBase*)OpenLibrary((STRPTR)"graphics.library",0);if(!DOSBase||!IntuitionBase||!GfxBase)goto out;qrx_theme_load();load_user_name();s=LockPubScreen(0);if(!s)goto out;left=s->Width-442;if(left<8)left=8;top=42;w=OpenWindowTags(0,WA_PubScreen,(ULONG)s,WA_Left,left,WA_Top,top,WA_Width,430,WA_Height,360,WA_Flags,WFLG_ACTIVATE|WFLG_BORDERLESS|WFLG_SMART_REFRESH|WFLG_RMBTRAP|WFLG_REPORTMOUSE,WA_IDCMP,IDCMP_VANILLAKEY|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|IDCMP_INTUITICKS,TAG_DONE);if(!w)goto out;draw(w,blink);
while(run){Delay(2);if(++tick>=25){tick=0;blink=!blink;avatar(w->RPort,blink);}while((m=(struct IntuiMessage*)GetMsg(w->UserPort))){ULONG c=m->Class;UWORD k=m->Code;WORD x=m->MouseX,y=m->MouseY;ReplyMsg((struct Message*)m);if(c==IDCMP_REFRESHWINDOW){BeginRefresh(w);draw(w,blink);EndRefresh(w,1);}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTDOWN){if(y<28&&qrx_theme_close_hit(w,x,y))run=0;else if(y<28&&qrx_theme_back_hit(w,x,y))WindowToBack(w);else if(y<28&&qrx_theme_drag_hit(w,x,y)){drag=1;dx=x;dy=y;dragx=s->MouseX;dragy=s->MouseY;dragleft=w->LeftEdge;dragtop=w->TopEdge;}}else if(c==IDCMP_MOUSEBUTTONS&&k==SELECTUP)drag=0;else if(c==IDCMP_INTUITICKS){if(w->MouseY>=w->Height-45)qrx_theme_tooltip(w,w->MouseX+8,w->MouseY-20,"Type naturally; QRXAI understands intent variants");}else if(c==IDCMP_MOUSEMOVE&&drag){LONG nl=dragleft+(LONG)s->MouseX-dragx,nt=dragtop+(LONG)s->MouseY-dragy;MoveWindow(w,nl-w->LeftEdge,nt-w->TopEdge);}else if(c==IDCMP_VANILLAKEY){if(k==13||k==10){if(ilen){strncpy(userline,input,sizeof(userline)-1);userline[sizeof(userline)-1]=0;answer(input);input[0]=0;ilen=0;draw(w,blink);}}else if(k==8||k==127){if(ilen)input[--ilen]=0;draw_input(w);}else if(k>=32&&k<=126&&ilen<INMAX){input[ilen++]=(char)k;input[ilen]=0;draw_input(w);}}}}
out:if(w)CloseWindow(w);if(s)UnlockPubScreen(0,s);if(GfxBase)CloseLibrary((struct Library*)GfxBase);if(IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);if(DOSBase)CloseLibrary((struct Library*)DOSBase);return 0;}
