#ifndef QRX_THEME_H
#define QRX_THEME_H
#define QRX_THEME_QRX 0
#define QRX_THEME_WIN 1
#define QRX_THEME_MAC 2
#define QRX_THEME_AMIGA 3
/* Shared QRX screen palette. These MUST match QRXDesktop. Never use pens 0..3 for app chrome. */
#define QP_BAR 224
#define QP_BAR_TEXT 225
#define QP_ACCENT 226
#define QP_PANEL 227
#define QP_PANEL2 228
#define QP_TEXT 229
#define QP_MUTED 230
#define QP_EDGE 231
#define QP_LIGHT 232
#define QP_DARK 233
#define QP_RED 234
#define QP_YELLOW 235
#define QP_GREEN 236
#define QP_WHITE 237
#define QP_BLACK 238
#define QP_BUTTON 239
#define QP_BLUE 240
#define QP_GREY 241
#define QP_GREY2 242
#define QP_AMIGA 243
#define QP_AMIGA_HI 244
static int qrx_theme_mode=QRX_THEME_QRX;
static UBYTE qrx_theme_legacy_pen(UBYTE p){if(p==0)return QP_PANEL2;if(p==1)return QP_PANEL;if(p==2)return QP_ACCENT;if(p==3)return QP_TEXT;return p;}
static void qrx_theme_load(void){BPTR f=Open((STRPTR)"SYS:QRX/Prefs/desktop.cfg",MODE_OLDFILE);char b[128];qrx_theme_mode=QRX_THEME_QRX;if(!f)f=Open((STRPTR)"QRX:Prefs/desktop.cfg",MODE_OLDFILE);if(!f)return;while(FGets(f,(STRPTR)b,sizeof(b))){if(!strncmp(b,"look=mac",8))qrx_theme_mode=QRX_THEME_MAC;else if(!strncmp(b,"look=windows",12))qrx_theme_mode=QRX_THEME_WIN;else if(!strncmp(b,"look=amiga",10))qrx_theme_mode=QRX_THEME_AMIGA;else if(!strncmp(b,"look=qrx",8))qrx_theme_mode=QRX_THEME_QRX;}Close(f);}
static void qrx_theme_text(struct RastPort*r,UBYTE p,WORD x,WORD y,const char*s){SetAPen(r,qrx_theme_legacy_pen(p));Move(r,x,y);Text(r,(STRPTR)s,strlen(s));}
static void qrx_theme_box(struct RastPort*r,UBYTE p,WORD x1,WORD y1,WORD x2,WORD y2){SetAPen(r,qrx_theme_legacy_pen(p));RectFill(r,x1,y1,x2,y2);}
static void qrx_theme_outline(struct RastPort*r,UBYTE p,WORD x1,WORD y1,WORD x2,WORD y2){SetAPen(r,qrx_theme_legacy_pen(p));Move(r,x1,y1);Draw(r,x2,y1);Draw(r,x2,y2);Draw(r,x1,y2);Draw(r,x1,y1);}
static void qrx_theme_frame(struct Window*w,const char*title){struct RastPort*r=w->RPort;WORD ww=w->Width,hh=w->Height;SetAPen(r,QP_PANEL);RectFill(r,0,0,ww-1,hh-1);SetAPen(r,QP_EDGE);Move(r,0,0);Draw(r,ww-1,0);Draw(r,ww-1,hh-1);Draw(r,0,hh-1);Draw(r,0,0);
 if(qrx_theme_mode==QRX_THEME_MAC){SetAPen(r,QP_BAR);RectFill(r,1,1,ww-2,27);SetAPen(r,QP_RED);RectFill(r,9,9,17,17);SetAPen(r,QP_YELLOW);RectFill(r,21,9,29,17);SetAPen(r,QP_GREEN);RectFill(r,33,9,41,17);SetAPen(r,QP_BAR_TEXT);Move(r,(ww/2)-(strlen(title)*4),19);Text(r,(STRPTR)title,strlen(title));}
 else if(qrx_theme_mode==QRX_THEME_WIN){SetAPen(r,QP_BAR);RectFill(r,2,2,ww-3,27);SetAPen(r,QP_BAR_TEXT);Move(r,8,19);Text(r,(STRPTR)title,strlen(title));SetAPen(r,QP_BUTTON);RectFill(r,ww-72,4,ww-50,24);RectFill(r,ww-48,4,ww-26,24);SetAPen(r,QP_RED);RectFill(r,ww-24,4,ww-4,24);SetAPen(r,QP_BLACK);Move(r,ww-66,18);Text(r,(STRPTR)"_",1);Move(r,ww-43,18);Text(r,(STRPTR)"[]",2);SetAPen(r,QP_WHITE);Move(r,ww-18,18);Text(r,(STRPTR)"X",1);}
 else if(qrx_theme_mode==QRX_THEME_AMIGA){SetAPen(r,QP_AMIGA);RectFill(r,2,2,ww-3,25);SetAPen(r,QP_PANEL);RectFill(r,5,5,21,21);RectFill(r,ww-22,5,ww-6,21);SetAPen(r,QP_WHITE);Move(r,31,18);Text(r,(STRPTR)title,strlen(title));}
 else {SetAPen(r,QP_BAR);RectFill(r,1,1,ww-2,28);SetAPen(r,QP_ACCENT);RectFill(r,1,27,ww-2,29);Move(r,8,19);Text(r,(STRPTR)"QRX",3);SetAPen(r,QP_BAR_TEXT);Move(r,43,19);Text(r,(STRPTR)title,strlen(title));SetAPen(r,QP_BUTTON);RectFill(r,ww-82,4,ww-62,24);RectFill(r,ww-58,4,ww-38,24);SetAPen(r,QP_RED);RectFill(r,ww-34,4,ww-14,24);SetAPen(r,QP_TEXT);Move(r,ww-76,18);Text(r,(STRPTR)"_",1);Move(r,ww-53,18);Text(r,(STRPTR)"+",1);SetAPen(r,QP_WHITE);Move(r,ww-29,18);Text(r,(STRPTR)"X",1);}}
static void qrx_theme_button(struct RastPort*r,WORD x,WORD y,WORD w,const char*s){UBYTE bg=(qrx_theme_mode==QRX_THEME_QRX)?QP_ACCENT:QP_BUTTON,fg=(qrx_theme_mode==QRX_THEME_QRX)?QP_WHITE:QP_BLACK;SetAPen(r,bg);RectFill(r,x,y,x+w,y+21);SetAPen(r,QP_EDGE);Move(r,x,y);Draw(r,x+w,y);Draw(r,x+w,y+21);Draw(r,x,y+21);Draw(r,x,y);SetAPen(r,fg);Move(r,x+6,y+15);Text(r,(STRPTR)s,strlen(s));}
static void qrx_theme_scrollbar(struct RastPort*r,WORD x,WORD top,WORD bot,int pos,int total){WORD h=bot-top,thumb=total>0?(h*8/(total+8)):h;if(thumb<18)thumb=18;SetAPen(r,QP_PANEL);RectFill(r,x,top,x+8,bot);SetAPen(r,QP_EDGE);Move(r,x,top);Draw(r,x+8,top);Draw(r,x+8,bot);Draw(r,x,bot);Draw(r,x,top);SetAPen(r,QP_BUTTON);RectFill(r,x+1,top+(h-thumb)*pos/(total?total:1),x+7,top+(h-thumb)*pos/(total?total:1)+thumb);}
static void qrx_theme_tooltip(struct Window*w,WORD x,WORD y,const char*s){struct RastPort*r=w->RPort;WORD ww=(WORD)(strlen(s)*8+10);if(x+ww>w->Width)x=w->Width-ww-2;if(y+20>w->Height)y=w->Height-22;SetAPen(r,QP_LIGHT);RectFill(r,x,y,x+ww,y+18);SetAPen(r,QP_EDGE);Move(r,x,y);Draw(r,x+ww,y);Draw(r,x+ww,y+18);Draw(r,x,y+18);Draw(r,x,y);SetAPen(r,QP_BLACK);Move(r,x+5,y+13);Text(r,(STRPTR)s,strlen(s));}
static int qrx_theme_close_hit(struct Window*w,WORD x,WORD y){if(y>29)return 0;if(qrx_theme_mode==QRX_THEME_MAC)return x>=6&&x<=20;if(qrx_theme_mode==QRX_THEME_AMIGA)return x<=24;return x>=w->Width-36;}
static int qrx_theme_back_hit(struct Window*w,WORD x,WORD y){if(y>29)return 0;if(qrx_theme_mode==QRX_THEME_MAC)return 0;if(qrx_theme_mode==QRX_THEME_AMIGA)return 0;return x>=w->Width-84&&x<w->Width-60;}
static int qrx_theme_max_hit(struct Window*w,WORD x,WORD y){if(y>29)return 0;if(qrx_theme_mode==QRX_THEME_MAC)return x>=40&&x<=55;if(qrx_theme_mode==QRX_THEME_AMIGA)return x>=w->Width-24;return x>=w->Width-60&&x<w->Width-36;}
static int qrx_theme_drag_hit(struct Window*w,WORD x,WORD y){if(y>29)return 0;return !qrx_theme_close_hit(w,x,y)&&!qrx_theme_back_hit(w,x,y)&&!qrx_theme_max_hit(w,x,y);}
#endif
