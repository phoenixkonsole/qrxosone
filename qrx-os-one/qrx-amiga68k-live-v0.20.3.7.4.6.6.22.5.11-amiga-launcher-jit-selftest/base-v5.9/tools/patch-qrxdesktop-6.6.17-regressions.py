#!/usr/bin/env python3
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text(); mark='QRX 6.6.17 regression hardening'
if mark in s: raise SystemExit(0)
s='/* '+mark+' */\n'+s
# Terminal RAWKEY: productive patch accidentally amended first matching IDCMP mask (Files), enforce terminal specifically.
old='''WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW |\n                  IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE,\n        TAG_DONE);\n    if (!g_terminal)'''
new='''WA_IDCMP, IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW |\n                  IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE | IDCMP_RAWKEY,\n        TAG_DONE);\n    if (!g_terminal)'''
if old not in s: raise SystemExit('terminal IDCMP anchor missing')
s=s.replace(old,new,1)
# Stable screen-coordinate dragging/resizing: local coordinates change as the window itself moves.
s=s.replace('''    g_files_gesture_abs_x = g_files->LeftEdge + x;\n    g_files_gesture_abs_y = g_files->TopEdge + y;''','''    g_files_gesture_abs_x = g_screen ? g_screen->MouseX : g_files->LeftEdge + x;\n    g_files_gesture_abs_y = g_screen ? g_screen->MouseY : g_files->TopEdge + y;''',1)
s=s.replace('''    abs_x = g_files->LeftEdge + x;\n    abs_y = g_files->TopEdge + y;''','''    abs_x = g_screen ? g_screen->MouseX : g_files->LeftEdge + x;\n    abs_y = g_screen ? g_screen->MouseY : g_files->TopEdge + y;''',1)
# Add browser/antivirus commands and actual AmigaDOS cd plus direct fallback.
needle='''    } else if (!strcmp(cmd, "hardware")) {\n        launch_native("QRX:C/QRXHardware");'''
rep='''    } else if (!strcmp(cmd, "web") || !strcmp(cmd, "browser")) {\n        launch_native("QRX:C/QRXWeb");\n    } else if (!strcmp(cmd, "antivirus") || !strcmp(cmd, "ares")) {\n        launch_native("QRX:C/ARESAntivirus");\n    } else if (!strcmp(cmd, "hardware")) {\n        launch_native("QRX:C/QRXHardware");'''
if needle not in s: raise SystemExit('hardware command anchor missing')
s=s.replace(needle,rep,1)
needle='''    } else if (!strcmp(cmd, "pwd")) {\n        execute_capture("QRX:C/QRXUnix pwd");'''
rep='''    } else if (!strcmp(cmd, "cd")) {\n        execute_capture("CD");\n    } else if (!strncmp(cmd, "cd ", 3)) {\n        BPTR lk=Lock((STRPTR)(cmd+3),ACCESS_READ);\n        if(!lk) add_line("cd: drawer not found.");\n        else { BPTR old=CurrentDir(lk); if(old) UnLock(old); execute_capture("CD"); }\n    } else if (!strcmp(cmd, "pwd")) {\n        execute_capture("CD");'''
if needle not in s: raise SystemExit('pwd anchor missing')
s=s.replace(needle,rep,1)
s=s.replace('add_line("Unknown QRX command. Use \'dos <cmd>\' for AmigaDOS.");','execute_capture(cmd);',1)
# advertise commands
s=s.replace('add_line(" writer       -> QRX Writer 68K");','add_line(" writer       -> QRX Writer 68K");\n        add_line(" web/browser  -> QRX Web 68K");\n        add_line(" ares/antivirus -> ARES Antivirus");',1)
p.write_text(s)
print('6.6.17 regression hardening patch OK')
