#!/usr/bin/env python3
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
mark='QRX v0.20.3.7.4.6.6.11 unified desktop UX'
if mark in s: raise SystemExit(0)
s='/* '+mark+' */\n'+s

def replace_func(name,new):
 global s
 a=s.find(name); b=s.find('\nstatic ',a+20)
 if a<0 or b<a: raise SystemExit('missing '+name)
 s=s[:a]+new+s[b:]

replace_func('static void qrx_draw_datetime(', '''static void qrx_draw_datetime(struct RastPort *rp) {
    struct DateStamp ds; struct DateTime dt; char d[LEN_DATSTRING],t[LEN_DATSTRING],out[64]; WORD x;
    DateStamp(&ds); memset(&dt,0,sizeof(dt)); dt.dat_Stamp=ds; dt.dat_Format=FORMAT_DOS;
    dt.dat_StrDate=(STRPTR)d; dt.dat_StrTime=(STRPTR)t;
    if(DateToStr(&dt)){snprintf(out,sizeof(out),"%.5s %.5s",d,t);x=(g_screen_width-(WORD)(strlen(out)*8))/2;if(x<260)x=260;if(x+(WORD)(strlen(out)*8)>382)x=382-(WORD)(strlen(out)*8);text_at(rp,PEN_BAR_TEXT,x,15,out);}
}''')

replace_func('static void draw_productivity_launcher(', '''static void draw_productivity_launcher(struct RastPort *rp) {
    WORD x=g_screen_width-112,y=58;if(x<520)x=520;
    fill(rp,PEN_PANEL,x,y,g_screen_width-7,y+268);outline(rp,PEN_EDGE,x,y,g_screen_width-7,y+268);text_at(rp,PEN_MUTED,x+10,y+17,"QRX APPS");
#define ROW(yy,label) do{fill(rp,PEN_BUTTON,x+7,y+(yy),g_screen_width-14,y+(yy)+22);outline(rp,PEN_EDGE,x+7,y+(yy),g_screen_width-14,y+(yy)+22);text_at(rp,PEN_TEXT,x+16,y+(yy)+16,label);}while(0)
    ROW(26,"WRITER");ROW(52,"SHEET");ROW(78,"PRESENTER");ROW(104,"HARDWARE");ROW(130,"SETTINGS");ROW(156,"WELCOME");ROW(182,"WEB");ROW(208,"ARES AV");ROW(234,"FILES");
#undef ROW
}''')

replace_func('static int desktop_click_productivity(', '''static int desktop_click_productivity(WORD x,WORD y){
    WORD px=g_screen_width-112,py=58;if(px<520)px=520;if(x<px+7||x>g_screen_width-14)return 0;
    if(y>=py+26&&y<=py+48){launch_native("QRX:C/QRXWriter");return 1;} if(y>=py+52&&y<=py+74){launch_native("QRX:C/QRXSpreadsheet");return 1;}
    if(y>=py+78&&y<=py+100){launch_native("QRX:C/QRXPresenter");return 1;} if(y>=py+104&&y<=py+126){launch_native("QRX:C/QRXHardware");return 1;}
    if(y>=py+130&&y<=py+152){launch_native("QRX:C/QRXSettings");return 1;} if(y>=py+156&&y<=py+178){launch_native("QRX:C/QRXWelcome");return 1;}
    if(y>=py+182&&y<=py+204){launch_native("QRX:C/QRXWeb");return 1;} if(y>=py+208&&y<=py+230){launch_native("QRX:C/ARESAntivirus");return 1;}
    if(y>=py+234&&y<=py+256){open_filemanager(g_files_path);return 1;} return 0;
}''')

needle='static void handle_desktop_message(struct IntuiMessage *msg) {'
helper='''static int g_ai_hover=0;
static void qrx_draw_ai_float(struct RastPort *rp){WORD x=g_screen_width-54,y=g_screen_height-64;fill(rp,PEN_RED,x,y,x+38,y+38);outline(rp,g_ai_hover?PEN_WHITE:PEN_EDGE,x,y,x+38,y+38);SetAPen(rp,PEN_WHITE);Move(rp,x+9,y+15);Draw(rp,x+15,y+15);Move(rp,x+23,y+15);Draw(rp,x+29,y+15);text_at(rp,PEN_WHITE,x+12,y+31,"AI");if(g_ai_hover){fill(rp,PEN_PANEL,x-68,y+8,x-4,y+31);outline(rp,PEN_EDGE,x-68,y+8,x-4,y+31);text_at(rp,PEN_TEXT,x-59,y+24,"QRXAI");}}
static int qrx_ai_float_hit(WORD x,WORD y){WORD ax=g_screen_width-54,ay=g_screen_height-64;return x>=ax&&x<=ax+38&&y>=ay&&y<=ay+38;}
static int qrx_top_menu_click(WORD x,WORD y){if(y>22)return 0;if(x>=8&&x<=58){launch_native("QRX:C/QRXWelcome");return 1;}if(x>=62&&x<=118){open_filemanager(g_files_path);return 1;}if(x>=122&&x<=194){WindowToFront(g_terminal);ActivateWindow(g_terminal);return 1;}if(x>=198&&x<=250){launch_native("QRX:C/QRXWelcome");return 1;}return desktop_click_look(x,y);}

'''+needle
if needle not in s: raise SystemExit('handler anchor missing')
s=s.replace(needle,helper,1)

# floating AI gets painted with the desktop
s=s.replace('    draw_productivity_launcher(rp);','    draw_productivity_launcher(rp);\n    qrx_draw_ai_float(rp);',1)

replace_func('static void handle_desktop_message(', '''static void handle_desktop_message(struct IntuiMessage *msg) {
    ULONG cls;UWORD code;WORD mx,my;int nh;if(!msg)return;cls=msg->Class;code=msg->Code;mx=msg->MouseX;my=msg->MouseY;ReplyMsg((struct Message*)msg);
    if(cls==IDCMP_REFRESHWINDOW){BeginRefresh(g_desktop);redraw_desktop();EndRefresh(g_desktop,TRUE);return;}
    if(cls==IDCMP_MOUSEMOVE){nh=qrx_ai_float_hit(mx,my);if(nh!=g_ai_hover){g_ai_hover=nh;redraw_desktop();}return;}
    if(cls!=IDCMP_MOUSEBUTTONS||code!=SELECTDOWN)return;
    if(qrx_ai_float_hit(mx,my)){launch_native("QRX:C/QRXAI");return;} if(qrx_top_menu_click(mx,my))return; if(qrx_windows_start_click(mx,my))return;
    if(desktop_click_dock(mx,my))return; if(desktop_click_productivity(mx,my))return; if(qrx_desktop_object_click(mx,my))return; if(desktop_click_foreign_window(mx,my))return;
    if(desktop_click_files_launcher(mx,my)){open_filemanager(g_files_path);return;}
}''')

s=s.replace('WFLG_BORDERLESS | WFLG_SMART_REFRESH | WFLG_RMBTRAP,','WFLG_BORDERLESS | WFLG_SMART_REFRESH | WFLG_RMBTRAP | WFLG_ACTIVATE | WFLG_REPORTMOUSE,',1)
s=s.replace('WA_IDCMP, IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS,','WA_IDCMP, IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE,',1)

# common bottom dock hit fallback
start=s.find('static int desktop_click_dock('); end=s.find('\nstatic ',start+20)
if start>=0 and end>start:
 frag=s[start:end]
 frag=frag.replace('    return 0;\n}', '''    if(y>=g_screen_height-48){WORD cx=g_screen_width/2;if(x>=cx-150&&x<cx-80){WindowToFront(g_terminal);ActivateWindow(g_terminal);return 1;}if(x>=cx-80&&x<cx-10){open_filemanager(g_files_path);return 1;}if(x>=cx-10&&x<cx+60){launch_native("QRX:C/QRXAI");return 1;}if(x>=cx+60&&x<=cx+160){launch_native("QRX:C/QRXSettings");return 1;}}\n    return 0;\n}''',1)
 s=s[:start]+frag+s[end:]

# Welcome once per QRXDesktop session.
s=s.replace('    redraw_all();\n\n    term_sig','    redraw_all();\n    launch_native("QRX:C/QRXWelcome");\n\n    term_sig',1)
s=s.replace('static void redraw_desktop(void);','static void qrx_draw_ai_float(struct RastPort *rp);\nstatic void redraw_desktop(void);',1)
# Real Windows START menu.
a=s.find('static int qrx_windows_start_click('); b=s.find('\nstatic ',a+20)
if a>=0 and b>a:
    fn='static int g_win_start_open=0;\nstatic void qrx_draw_win_start_menu(struct RastPort *rp){WORD y=g_screen_height-28;if(g_look!=LOOK_WINDOWS||!g_win_start_open)return;fill(rp,PEN_PANEL,4,y-174,164,y-2);outline(rp,PEN_EDGE,4,y-174,164,y-2);text_at(rp,PEN_TEXT,16,y-150,\"WRITER\");text_at(rp,PEN_TEXT,16,y-124,\"SPREADSHEET\");text_at(rp,PEN_TEXT,16,y-98,\"PRESENTER\");text_at(rp,PEN_TEXT,16,y-72,\"SETTINGS\");text_at(rp,PEN_TEXT,16,y-46,\"QRXAI\");text_at(rp,PEN_TEXT,16,y-20,\"WELCOME\");}\nstatic int qrx_windows_start_click(WORD x,WORD y){WORD ty=g_screen_height-28;if(g_look!=LOOK_WINDOWS)return 0;if(x>=4&&x<=72&&y>=ty+4&&y<=ty+23){g_win_start_open=!g_win_start_open;qrx_draw_win_start_menu(g_desktop->RPort);return 1;}if(g_win_start_open&&x>=4&&x<=164&&y>=ty-174&&y<ty){WORD r=(y-(ty-174))/26;if(r==0)launch_native(\"QRX:C/QRXWriter\");else if(r==1)launch_native(\"QRX:C/QRXSpreadsheet\");else if(r==2)launch_native(\"QRX:C/QRXPresenter\");else if(r==3)launch_native(\"QRX:C/QRXSettings\");else if(r==4)launch_native(\"QRX:C/QRXAI\");else launch_native(\"QRX:C/QRXWelcome\");g_win_start_open=0;return 1;}if(x>=80&&x<=242&&y>=ty+4&&y<=ty+23){WindowToFront(g_terminal);ActivateWindow(g_terminal);return 1;}return 0;}'
    s=s[:a]+fn+s[b:]
# launch_native is used by the Windows START helper before its original definition.
# Keep an early static prototype so GCC never creates an implicit external declaration.
qs=s.find('static int g_win_start_open=0;')
if qs>=0 and 'static LONG launch_native(const char *command);' not in s[:qs]:
    s=s[:qs]+'static LONG launch_native(const char *command);\n'+s[qs:]

s=s.replace('    qrx_draw_ai_float(rp);','    qrx_draw_ai_float(rp);\n    qrx_draw_win_start_menu(rp);',1)
s=s.replace('if(nh!=g_ai_hover){g_ai_hover=nh;redraw_desktop();}','if(nh!=g_ai_hover){g_ai_hover=nh;qrx_draw_ai_float(g_desktop->RPort);}')
s=s.replace('add_line(\"QRX OS One / native 68K\"); return 1;','launch_native(\"QRX:C/QRXSettings\"); return 1;',1)
p.write_text(s)
print('QRXDesktop 6.6.11 unified UX patch OK')
