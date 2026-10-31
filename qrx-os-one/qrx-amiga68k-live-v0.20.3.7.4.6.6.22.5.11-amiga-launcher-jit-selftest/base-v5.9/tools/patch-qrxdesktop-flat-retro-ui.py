#!/usr/bin/env python3
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
marker='/* QRX v0.20.3.7.4.6.5.9 Flat Retro UI / ECS-OCS visual baseline */'
if marker in s:
 print('Flat Retro UI already installed'); raise SystemExit(0)
needle='static void draw_wallpaper(struct Screen *s) {'
helpers='''/* QRX v0.20.3.7.4.6.5.9 Flat Retro UI / ECS-OCS visual baseline */
static void qrx_flat_frame(struct RastPort *rp, WORD x1, WORD y1, WORD x2, WORD y2) {
    outline(rp, PEN_DARK, x1, y1, x2, y2);
    if (x2-x1 > 3 && y2-y1 > 3) outline(rp, PEN_LIGHT, x1+1, y1+1, x2-1, y2-1);
}
static void qrx_flat_button(struct RastPort *rp, WORD x1, WORD y1, WORD x2, WORD y2, UBYTE bg, int active) {
    fill(rp, bg, x1, y1, x2, y2); outline(rp, PEN_EDGE, x1, y1, x2, y2);
    SetAPen(rp, qrx_pen(active ? PEN_ACCENT : PEN_LIGHT));
    Move(rp,x1+1,y2-1); Draw(rp,x1+1,y1+1); Draw(rp,x2-1,y1+1);
    SetAPen(rp,qrx_pen(PEN_DARK)); Move(rp,x2-1,y1+1); Draw(rp,x2-1,y2-1); Draw(rp,x1+1,y2-1);
}
'''+needle
if needle not in s: raise SystemExit('wallpaper anchor missing')
s=s.replace(needle,helpers,1)
# Deliberate OCS 4-colour profile before existing low-depth profile.
anchor='    if (g_screen_depth < 8) {'
ix=s.index(anchor,s.index('static void apply_look_palette'))
ocs='''    if (g_screen_depth <= 2) {
        /* OCS 2-bit baseline: four roles, maximum separation, no gradients. */
        set_color(0, 8, 14, 24); set_color(1, 32, 78, 142);
        set_color(2, 142, 160, 174); set_color(3, 244, 248, 252);
        return;
    }
'''
s=s[:ix]+ocs+s[ix:]
# Stronger flat QRX palette on AGA/RTG.
repls={
'set_color(PEN_BAR,       8,  14,  24);':'set_color(PEN_BAR,      18,  48,  94);',
'set_color(PEN_ACCENT,   67, 219, 184);':'set_color(PEN_ACCENT,   55, 174, 211);',
'set_color(PEN_PANEL,    13,  22,  36);':'set_color(PEN_PANEL,    34,  54,  76);',
'set_color(PEN_PANEL_2,   6,  12,  22);':'set_color(PEN_PANEL_2,  12,  24,  38);',
'set_color(PEN_MUTED,   125, 161, 190);':'set_color(PEN_MUTED,   151, 174, 194);',
'set_color(PEN_EDGE,     44,  70,  91);':'set_color(PEN_EDGE,     74, 102, 126);',
'set_color(PEN_LIGHT,   114, 236, 211);':'set_color(PEN_LIGHT,   174, 211, 232);',
'set_color(PEN_BUTTON,   20,  37,  54);':'set_color(PEN_BUTTON,   48,  73,  96);'}
for a,b in repls.items():
 if a not in s: raise SystemExit('palette anchor missing: '+a)
 s=s.replace(a,b,1)
# Add depth frames to existing QRX dock/buttons without changing event geometry.
s=s.replace('''    outline(rp, PEN_EDGE, dock_x, dock_y, dock_x + 287, dock_y + 32);''','''    qrx_flat_frame(rp, dock_x, dock_y, dock_x + 287, dock_y + 32);''',1)
s=s.replace('''    fill(rp, PEN_ACCENT, dock_x + 16, dock_y + 8,
         dock_x + 62, dock_y + 24);''','''    qrx_flat_button(rp, dock_x + 16, dock_y + 8, dock_x + 62, dock_y + 24, PEN_ACCENT, 1);''',1)
s=s.replace('''    fill(rp, PEN_BUTTON, dock_x + 72, dock_y + 8,
         dock_x + 118, dock_y + 24);''','''    qrx_flat_button(rp, dock_x + 72, dock_y + 8, dock_x + 118, dock_y + 24, PEN_BUTTON, 0);''',1)
s=s.replace('''    fill(rp, PEN_BUTTON, dock_x + 128, dock_y + 8,
         dock_x + 174, dock_y + 24);''','''    qrx_flat_button(rp, dock_x + 128, dock_y + 8, dock_x + 174, dock_y + 24, PEN_BUTTON, 0);''',1)
s=s.replace('''    fill(rp, PEN_BUTTON, dock_x + 184, dock_y + 8,
         dock_x + 270, dock_y + 24);''','''    qrx_flat_button(rp, dock_x + 184, dock_y + 8, dock_x + 270, dock_y + 24, PEN_BUTTON, 0);''',1)
# Header gets a hard separator and selector buttons get depth cue.
s=s.replace('''    fill(rp, PEN_BAR, 0, 0, g_screen_width - 1, 25);''','''    fill(rp, PEN_BAR, 0, 0, g_screen_width - 1, 25);
    outline(rp, PEN_DARK, 0, 0, g_screen_width - 1, 25);''',1)
s=s.replace('''        fill(rp, bg, x, 4, x + 54, 18);
        outline(rp, PEN_EDGE, x, 4, x + 54, 18);''','''        qrx_flat_button(rp, x, 4, x + 54, 18, bg, item == g_look);''',1)
p.write_text(s)
print('QRXDesktop Flat Retro UI / ECS-OCS baseline patch OK:',p)
