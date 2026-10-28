#!/usr/bin/env python3
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
marker='QRX v0.20.3.7.4.6.6.6 desktop interaction + flicker-free terminal + AI/date'
if marker in s:
    print('already patched'); raise SystemExit(0)
# marker
s=s.replace('/*\n * QRX OS One - Amiga 68K native desktop prototype v0.20', f'/* {marker} */\n/*\n * QRX OS One - Amiga 68K native desktop prototype v0.20',1)
# Use DOS SystemTags for real asynchronous app launch; Execute('Run ...') could
# report success even when no child became visible on this Deadwood build.
s=s.replace('#include <dos/dosextens.h>', '#include <dos/dosextens.h>\n#include <dos/dostags.h>', 1)
old_launch='''static LONG launch_native(const char *command) {\n    char shell[256];\n\n    if (!command || !*command) {\n        add_line("Usage: run <Amiga executable> [arguments]");\n        return 0;\n    }\n\n    snprintf(shell, sizeof(shell), "Run >NIL: %s", command);\n    if (execute_capture(shell)) {\n        add_line("Native 68K process launched.");\n        return 1;\n    }\n\n    add_line("Launch failed. Check path, executable and required libraries.");\n    return 0;\n}'''
new_launch='''static LONG launch_native(const char *command) {\n    LONG rc;\n    if (!command || !*command) { add_line("Usage: run <Amiga executable> [arguments]"); return 0; }\n    rc = SystemTags((STRPTR)command, SYS_Asynch, TRUE, SYS_Input, 0, SYS_Output, 0, TAG_DONE);\n    if (rc != -1) { add_line("Native 68K process launched."); return 1; }\n    add_line("Launch failed. Check path, executable and required libraries.");\n    return 0;\n}'''
if old_launch not in s: raise SystemExit('launch_native anchor missing')
s=s.replace(old_launch,new_launch,1)

# Add partial input renderer after redraw_terminal
anchor='''static void redraw_all(void) {\n    redraw_desktop();\n    redraw_terminal();\n}\n'''
insert='''static void redraw_terminal_input(void) {\n    struct RastPort *rp;\n    char prompt[INPUT_LEN + 12];\n    WORD y;\n    if (!g_terminal) return;\n    rp = g_terminal->RPort;\n    y = g_terminal->Height - 26;\n    /* Only repaint the command line. Full-window redraw on every key caused\n       visible flashes on OCS/ECS and under emulation. */\n    fill(rp, PEN_PANEL_2, 6, y, g_terminal->Width - 7, g_terminal->Height - 7);\n    snprintf(prompt, sizeof(prompt), "qrx:> %s_", input);\n    text_at(rp, PEN_ACCENT, 14, g_terminal->Height - 15, prompt);\n}\n\n'''+anchor
if anchor not in s: raise SystemExit('redraw_all anchor missing')
s=s.replace(anchor,insert,1)
# key redraws: only replace in handler block globally exact common lines for typed/backspace
s=s.replace('''            if (input_len > 0) input[--input_len] = 0;\n            redraw_terminal();''','''            if (input_len > 0) input[--input_len] = 0;\n            redraw_terminal_input();''',1)
s=s.replace('''            input[input_len++] = (char)code;\n            input[input_len] = 0;\n            redraw_terminal();''','''            input[input_len++] = (char)code;\n            input[input_len] = 0;\n            redraw_terminal_input();''',1)
# htop command after mem
needle='''    } else if (!strcmp(cmd, "clear") || !strcmp(cmd, "cls")) {'''
htop='''    } else if (!strcmp(cmd, "htop") || !strcmp(cmd, "top")) {\n        allmem  = AvailMem(MEMF_ANY);\n        chipmem = AvailMem(MEMF_CHIP);\n        fastmem = AvailMem(MEMF_FAST);\n        add_line("QRX HTOP 68K  TASKS / MEMORY");\n        snprintf(buf, sizeof(buf), "Free: %luK  Chip: %luK  Fast: %luK", allmem/1024UL, chipmem/1024UL, fastmem/1024UL);\n        add_line(buf);\n        execute_capture("Status");\n'''+needle
if needle not in s: raise SystemExit('clear anchor missing')
s=s.replace(needle,htop,1)
s=s.replace('help version info mem clear about looks look <name> prefs','help version info mem htop clear about looks look <name> prefs',1)
# date/time helper before draw productivity
needle='static void draw_productivity_launcher(struct RastPort *rp) {'
timehelper='''static void qrx_draw_datetime(struct RastPort *rp) {\n    struct DateStamp ds;\n    struct DateTime dt;\n    char d[LEN_DATSTRING], t[LEN_DATSTRING], out[64];\n    DateStamp(&ds);\n    memset(&dt, 0, sizeof(dt));\n    dt.dat_Stamp = ds;\n    dt.dat_Format = FORMAT_DOS;\n    dt.dat_Flags = 0;\n    dt.dat_StrDate = d;\n    dt.dat_StrTime = t;\n    if (DateToStr(&dt)) {\n        if (g_screen_width < 760) { snprintf(out, sizeof(out), "%s", t); text_at(rp, PEN_BAR_TEXT, g_screen_width - 62, 15, out); }
        else { snprintf(out, sizeof(out), "%s  %s", d, t); text_at(rp, PEN_BAR_TEXT, g_screen_width - 175, 15, out); }\n    }\n}\n\n'''+needle
if needle not in s: raise SystemExit('productivity anchor missing')
s=s.replace(needle,timehelper,1)
# ensure datetime after desktop skin drawing
needle='''    else if (g_look == LOOK_AMIGAOS) draw_amiga_desktop(rp);\n    else draw_qrx_desktop(rp);\n\n    draw_desktop_disk_icon'''
rep='''    else if (g_look == LOOK_AMIGAOS) draw_amiga_desktop(rp);\n    else draw_qrx_desktop(rp);\n    qrx_draw_datetime(rp);\n\n    draw_desktop_disk_icon'''
if needle not in s: raise SystemExit('desktop draw anchor missing')
s=s.replace(needle,rep,1)
# improve fallback desktop icon glyph
old='''        fill(rp, e->dir_type > 0 ? PEN_ACCENT : PEN_BUTTON,\n             x, y, x + 36, y + 30);\n        outline(rp, PEN_EDGE, x, y, x + 36, y + 30);\n        text_at(rp, e->dir_type > 0 ? PEN_BLACK : PEN_TEXT,\n                x + 11, y + 21, e->dir_type > 0 ? "D" : "A");'''
new='''        const char *glyph = e->dir_type > 0 ? "D" : "*";\n        if (qrx_ci_cmp(e->name, "Writer") == 0) glyph = "W";\n        else if (qrx_ci_cmp(e->name, "Spreadsheet") == 0) glyph = "S";\n        else if (qrx_ci_cmp(e->name, "Presenter") == 0) glyph = "P";\n        else if (qrx_ci_cmp(e->name, "Hardware") == 0) glyph = "H";\n        fill(rp, e->dir_type > 0 ? PEN_ACCENT : PEN_BUTTON, x, y, x + 36, y + 30);\n        outline(rp, PEN_EDGE, x, y, x + 36, y + 30);\n        fill(rp, PEN_PANEL_2, x + 5, y + 5, x + 31, y + 25);\n        outline(rp, PEN_WHITE, x + 5, y + 5, x + 31, y + 25);\n        text_at(rp, PEN_TEXT, x + 14, y + 20, glyph);'''
if old not in s: raise SystemExit('icon anchor missing')
s=s.replace(old,new,1)
# dock interaction function before productivity click
needle='static int desktop_click_productivity(WORD x, WORD y) {'
dock='''static void redraw_terminal(void);\n\nstatic int desktop_click_dock(WORD x, WORD y) {\n    WORD dx, dy;\n    if (g_look == LOOK_QRX) {\n        dx=(g_screen_width-288)/2; if(dx<8)dx=8; dy=g_screen_height-38;\n        if(y>=dy+8 && y<=dy+25) {\n            if(x>=dx+14 && x<=dx+62) { WindowToFront(g_terminal); ActivateWindow(g_terminal); return 1; }\n            if(x>=dx+72 && x<=dx+122) { open_filemanager(g_files_path); return 1; }\n            if(x>=dx+130 && x<=dx+180) { launch_native("QRX:C/QRXAI"); return 1; }\n            if(x>=dx+188 && x<=dx+258) { add_line("QRX OS One / native 68K"); return 1; }\n        }\n    } else if (g_look == LOOK_MACOS) {\n        dx=(g_screen_width-274)/2; if(dx<8)dx=8; dy=g_screen_height-41;\n        if(y>=dy+8 && y<=dy+27) {\n            if(x>=dx+14 && x<=dx+64) { WindowToFront(g_terminal); ActivateWindow(g_terminal); return 1; }\n            if(x>=dx+72 && x<=dx+122) { open_filemanager(g_files_path); return 1; }\n            if(x>=dx+130 && x<=dx+180) { launch_native("QRX:C/QRXAI"); return 1; }\n        }\n    }\n    return 0;\n}\n\n'''+needle
if needle not in s: raise SystemExit('click productivity anchor missing')
s=s.replace(needle,dock,1)
# call dock first
needle='''    } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {\n        if(g_context_open){ WORD x=g_context_x,y=g_context_y; int row=(my-y)/20; g_context_open=0;\n            if(mx>=x&&mx<=x+145&&my>=y&&my<=y+100){ if(row==0)open_filemanager(g_files_path); else if(row==1){WindowToFront(g_terminal);ActivateWindow(g_terminal);} else if(row==2)qrx_scan_desktop(); else if(row==3)launch_native("QRX:C/QRXSettings"); }\n            redraw_desktop(); return; }\n        if (desktop_click_productivity(mx, my)) {'''
rep='''    } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {\n        if(g_context_open){ WORD x=g_context_x,y=g_context_y; int row=(my-y)/20; g_context_open=0;\n            if(mx>=x&&mx<=x+145&&my>=y&&my<=y+100){ if(row==0)open_filemanager(g_files_path); else if(row==1){WindowToFront(g_terminal);ActivateWindow(g_terminal);} else if(row==2)qrx_scan_desktop(); else if(row==3)launch_native("QRX:C/QRXSettings"); }\n            redraw_desktop(); return; }\n        if (desktop_click_dock(mx, my)) { redraw_desktop(); return; }\n        if (desktop_click_productivity(mx, my)) {'''
if needle not in s: raise SystemExit('desktop message anchor missing')
s=s.replace(needle,rep,1)
# Add AI terminal command before color
needle='''    } else if (!strcmp(cmd, "color")) {'''
s=s.replace(needle,'''    } else if (!strcmp(cmd, "ai") || !strcmp(cmd, "chat")) {\n        launch_native("QRX:C/QRXAI");\n'''+needle,1)
s=s.replace('windows focus <n> desktop open legacy takeover runmode color quit','windows focus <n> desktop open legacy takeover runmode ai chat color quit',1)
# 6.6.9 right app menu and Windows Start wiring
s=s.replace('y + 132);\\n    outline(rp, PEN_EDGE, x, y, g_screen_width - 8, y + 132);','y + 202);\\n    outline(rp, PEN_EDGE, x, y, g_screen_width - 8, y + 202);',1)
s=s.replace('''    fill(rp, PEN_ACCENT, x + 6, y + 100, g_screen_width - 14, y + 124);
    text_at(rp, PEN_BLACK, x + 13, y + 116, "HARDWARE");''','''    fill(rp, PEN_ACCENT, x+6,y+88,g_screen_width-14,y+108); text_at(rp,PEN_BLACK,x+13,y+102,"HARDWARE");
    fill(rp,PEN_BUTTON,x+6,y+110,g_screen_width-14,y+130); text_at(rp,PEN_TEXT,x+13,y+124,"AI");
    fill(rp,PEN_BUTTON,x+6,y+132,g_screen_width-14,y+152); text_at(rp,PEN_TEXT,x+13,y+146,"FILES");
    fill(rp,PEN_BUTTON,x+6,y+154,g_screen_width-14,y+174); text_at(rp,PEN_TEXT,x+13,y+168,"TERMINAL");
    fill(rp,PEN_BUTTON,x+6,y+176,g_screen_width-14,y+196); text_at(rp,PEN_TEXT,x+13,y+190,"COMPAT");''',1)
old='''    if (y >= py + 100 && y <= py + 124) {
        launch_native("QRX:C/QRXHardware");
        return 1;
    }
    return 0;'''
new='''    if (y >= py+88 && y <= py+108) { launch_native("QRX:C/QRXHardware"); return 1; }
    if (y >= py+110 && y <= py+130) { launch_native("QRX:C/QRXAI"); return 1; }
    if (y >= py+132 && y <= py+152) { open_filemanager(g_files_path); return 1; }
    if (y >= py+154 && y <= py+174) { WindowToFront(g_terminal); ActivateWindow(g_terminal); return 1; }
    if (y >= py+176 && y <= py+196) { launch_native("QRX:C/QRXCompatCenter"); return 1; }
    return 0;'''
if old not in s: raise SystemExit('6.6.9 app click anchor missing')
s=s.replace(old,new,1)
needle='static void qrx_draw_datetime(struct RastPort *rp) {'
start='''static int qrx_windows_start_click(WORD x, WORD y) {
    WORD ty=g_screen_height-28;
    if(g_look!=LOOK_WINDOWS)return 0;
    if(x>=4&&x<=72&&y>=ty+4&&y<=ty+23){ add_line("START: Writer | Sheet | Presenter | Hardware | AI | Files | Compat"); return 1; }
    if(x>=80&&x<=242&&y>=ty+4&&y<=ty+23){ WindowToFront(g_terminal); ActivateWindow(g_terminal); return 1; }
    return 0;
}

'''+needle
s=s.replace(needle,start,1)
s=s.replace('        if (desktop_click_dock(mx, my)) { redraw_desktop(); return; }','        if (qrx_windows_start_click(mx, my)) { redraw_desktop(); return; }\n        if (desktop_click_dock(mx, my)) { redraw_desktop(); return; }',1)

# 6.6.10 final sanitation
s=s.replace('}\\\\n        if (desktop_click_dock', '}\\n        if (desktop_click_dock')
sa=s.find('static int qrx_windows_start_click')
sb=s.find('static void qrx_draw_datetime', sa)
if sa>=0 and sb>sa:
    frag=s[sa:sb].replace('; redraw_terminal(); return 1;', '; return 1;')
    s=s[:sa]+frag+s[sb:]
hm=s.find('static void handle_desktop_message')
he=s.find('\\nstatic ', hm+20) if hm>=0 else -1
if hm>=0:
    if he<0: he=len(s)
    frag=s[hm:he]
    anchor='if (qrx_windows_start_click(mx, my)) { redraw_desktop(); return; }'
    if anchor in frag:
        additions=[]
        if 'desktop_click_dock(mx, my)' not in frag: additions.append('        if (desktop_click_dock(mx, my)) { redraw_desktop(); return; }')
        if 'desktop_click_productivity(mx, my)' not in frag: additions.append('        if (desktop_click_productivity(mx, my)) { redraw_desktop(); return; }')
        if 'qrx_desktop_object_click(mx, my)' not in frag: additions.append('        if (qrx_desktop_object_click(mx, my)) { redraw_desktop(); return; }')
        if additions:
            frag=frag.replace(anchor,anchor+'\\n'+'\\n'.join(additions),1)
            s=s[:hm]+frag+s[he:]
if '}\\\\n        if (' in s:
    raise SystemExit('6.6.10 gate: literal escaped newline remains')
p.write_text(s)
print('QRXDesktop interaction/terminal/AI/date patch OK:',p)
