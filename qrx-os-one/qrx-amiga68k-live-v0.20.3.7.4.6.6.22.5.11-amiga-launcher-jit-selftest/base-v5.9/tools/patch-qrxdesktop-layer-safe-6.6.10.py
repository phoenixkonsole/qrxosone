#!/usr/bin/env python3
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
marker='QRX v0.20.3.7.4.6.6.10 layer-safe desktop'
if marker in s:
    print('6.6.10 layer-safe patch already present'); raise SystemExit(0)
s='/* '+marker+' */\n'+s
s=s.replace('if (qrx_windows_start_click(mx, my)) { redraw_desktop(); return; }','if (qrx_windows_start_click(mx, my)) { return; }')
s=s.replace('if (desktop_click_dock(mx, my)) { redraw_desktop(); return; }','if (desktop_click_dock(mx, my)) { return; }')
s=s.replace('if (desktop_click_productivity(mx, my)) { redraw_desktop(); return; }','if (desktop_click_productivity(mx, my)) { return; }')
s=s.replace('if (qrx_desktop_object_click(mx, my)) { redraw_desktop(); return; }','if (qrx_desktop_object_click(mx, my)) { return; }')
a=s.find('static void redraw_desktop(')
if a>=0:
    b=s.find('\nstatic ',a+20)
    if b<0:b=len(s)
    frag=s[a:b]
    frag=frag.replace('draw_wallpaper(g_screen);','if (g_visitor_count == 0) draw_wallpaper(g_screen);')
    frag=frag.replace('draw_wallpaper();','if (g_visitor_count == 0) draw_wallpaper();')
    s=s[:a]+frag+s[b:]
p.write_text(s)
print('QRXDesktop 6.6.10 layer-safe patch OK:',p)
