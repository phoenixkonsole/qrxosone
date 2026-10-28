#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: patch-qrxdesktop-screen-reset-public-host.py qrx_desktop.c')

p=Path(sys.argv[1])
s=p.read_text(errors='replace')
marker='/* QRX v0.20.3.7.4.6.5.7 existing public screen + low-depth compatibility */'
if marker in s:
    print('QRXDesktop v0.20.3.7.4.6.5.7 hotfix already present:', p)
    raise SystemExit(0)
if '/* QRX v0.20.3.7.4.6.5.7 AROS display fallback */' not in s:
    raise SystemExit('v0.20.3.7.4.6.5.7 display fallback marker missing')

# 1) runtime ownership state
needle='static char g_display_label[64] = "AGA 640x512x8";\n'
rep=needle + '''\n''' + marker + '''\nstatic int g_screen_borrowed = 0;\n'''
if needle not in s: raise SystemExit('display label anchor missing')
s=s.replace(needle,rep,1)

# 2) low-depth logical pen mapping and palette policy
needle='''static ULONG rgb32(UBYTE v) {\n    return ((ULONG)v << 24) | ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;\n}\n\n'''
insert=r'''static ULONG rgb32(UBYTE v) {
    return ((ULONG)v << 24) | ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;
}

/*
 * On the classic Deadwood bootstrap screen the working public screen can be
 * shallower than 8-bit.  Keep the logical QRX chrome pens stable while mapping
 * them to a compact 16-colour set.  8-bit/RTG screens keep the original pens.
 */
static UBYTE qrx_pen(UBYTE pen) {
    UBYTE mapped;
    UBYTE mask;
    if (g_screen_depth >= 8) return pen;
    switch (pen) {
        case PEN_BAR:      mapped = 1;  break;
        case PEN_BAR_TEXT: mapped = 15; break;
        case PEN_ACCENT:   mapped = 10; break;
        case PEN_PANEL:    mapped = 8;  break;
        case PEN_PANEL_2:  mapped = 0;  break;
        case PEN_TEXT:     mapped = 15; break;
        case PEN_MUTED:    mapped = 7;  break;
        case PEN_EDGE:     mapped = 0;  break;
        case PEN_LIGHT:    mapped = 15; break;
        case PEN_DARK:     mapped = 0;  break;
        case PEN_RED:      mapped = 12; break;
        case PEN_YELLOW:   mapped = 14; break;
        case PEN_GREEN:    mapped = 10; break;
        case PEN_WHITE:    mapped = 15; break;
        case PEN_BLACK:    mapped = 0;  break;
        case PEN_BUTTON:   mapped = 7;  break;
        case PEN_BLUE:     mapped = 9;  break;
        case PEN_GREY:     mapped = 8;  break;
        case PEN_GREY_2:   mapped = 7;  break;
        case PEN_AMIGA:    mapped = 9;  break;
        case PEN_AMIGA_HI: mapped = 11; break;
        default:           mapped = (UBYTE)(pen & 15); break;
    }
    mask = (UBYTE)((1U << (g_screen_depth ? g_screen_depth : 1)) - 1U);
    return (UBYTE)(mapped & mask);
}

'''
if needle not in s: raise SystemExit('rgb32 anchor missing')
s=s.replace(needle,insert,1)

needle='''static void apply_look_palette(void) {\n    set_color(PEN_RED,       239,  83,  80);\n'''
rep=r'''static void apply_look_palette(void) {
    /* Never change the palette of a screen owned by Wanderer/another task. */
    if (g_screen_borrowed) return;

    if (g_screen_depth < 8) {
        static const UBYTE low[16][3] = {
            {  3,   5,   8}, { 18,  52, 110}, { 32,  72,  96}, { 58,  92, 122},
            {120,  40,  40}, {120,  70,  30}, { 50, 105,  70}, {150, 156, 164},
            { 82,  88,  98}, { 48, 110, 220}, { 67, 219, 184}, {140, 190, 245},
            {239,  83,  80}, {200, 100, 190}, {244, 190,  70}, {245, 248, 252}
        };
        ULONG i, count = 1UL << (g_screen_depth ? g_screen_depth : 1);
        if (count > 16) count = 16;
        for (i = 0; i < count; ++i)
            set_color(i, low[i][0], low[i][1], low[i][2]);
        return;
    }

    set_color(PEN_RED,       239,  83,  80);
'''
if needle not in s: raise SystemExit('apply palette anchor missing')
s=s.replace(needle,rep,1)

# Map symbolic pens at every direct drawing helper call.
s=s.replace('SetAPen(rp, pen);','SetAPen(rp, qrx_pen(pen));')
s=s.replace('SetAPen(rp, hi);','SetAPen(rp, qrx_pen(hi));')
s=s.replace('SetAPen(rp, lo);','SetAPen(rp, qrx_pen(lo));')

# Low-depth wallpaper path must happen before the 224-colour wallpaper palette loop.
needle='''    if (!s) return;\n    rp = &s->RastPort;\n    bm = rp->BitMap;\n    if (!bm) return;\n\n    for (i = 0; i < QRX_COLORS; ++i) {\n'''
rep=r'''    if (!s) return;
    rp = &s->RastPort;
    bm = rp->BitMap;
    if (!bm) return;

    if (g_screen_depth < 8) {
        /* Compact classic fallback: no 256-colour wallpaper assumptions. */
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_screen_width - 1, g_screen_height - 1);
        SetAPen(rp, qrx_pen(PEN_BAR));
        RectFill(rp, 0, 0, g_screen_width - 1, 28);
        SetAPen(rp, qrx_pen(PEN_ACCENT));
        RectFill(rp, 0, 29, g_screen_width - 1, 31);
        return;
    }

    for (i = 0; i < QRX_COLORS; ++i) {
'''
if needle not in s: raise SystemExit('wallpaper preamble anchor missing')
s=s.replace(needle,rep,1)

# 3) current-screen fallback: clone native depth, not forced 8-bit.
s=s.replace('ULONG requested_depth = active_depth < QRX_DEPTH ? QRX_DEPTH : active_depth;',
            'ULONG requested_depth = active_depth;',1)
old='''                    if (depth < QRX_DEPTH) {\n                        CloseScreen(g_screen);\n                        g_screen = NULL;\n                    }\n                    if (g_screen) {\n'''
# First occurrence is current-mode fallback; second is SA_LikeWorkbench. Remove both in this hotfix.
count=s.count(old)
if count < 1: raise SystemExit(f'expected current-mode 8-bit rejection block, found {count}')
s=s.replace(old,'                    if (g_screen) {\n',1)
old2='''            if (depth < QRX_DEPTH) {\n                CloseScreen(g_screen);\n                g_screen = NULL;\n            }\n            if (g_screen) {\n'''
if old2 not in s: raise SystemExit('SA_LikeWorkbench 8-bit rejection block missing')
s=s.replace(old2,'            if (g_screen) {\n',1)

# SA_LikeWorkbench should inherit Workbench depth rather than force 8-bit.
s=s.replace('            SA_LikeWorkbench, TRUE,\n            SA_Depth, QRX_DEPTH,\n',
            '            SA_LikeWorkbench, TRUE,\n',1)

# Add final borrowed-public-screen host before terminal failure.
needle='''    if (!g_screen) {\n        PutStr("QRX: no usable RTG, AGA, or current AROS display mode found.\\n");\n        return 0;\n    }\n\n    PubScreenStatus(g_screen, 0);\n    SetDefaultPubScreen((UBYTE *)"QRX");\n    draw_wallpaper(g_screen);\n    apply_look_palette();\n    ScreenToFront(g_screen);\n    return 1;\n'''
rep=r'''    if (!g_screen) {
        struct Screen *host = LockPubScreen(NULL);
        if (host) {
            ULONG host_depth = GetBitMapAttr(host->RastPort.BitMap, BMA_DEPTH);
            g_screen = host;
            g_screen_borrowed = 1;
            g_screen_mode = GetVPModeID(&host->ViewPort);
            g_screen_width = host->Width;
            g_screen_height = host->Height;
            g_screen_depth = (UBYTE)(host_depth ? host_depth : 1);
            g_display_is_rtg =
                (g_screen_width > QRX_W ||
                 g_screen_height > QRX_H ||
                 g_screen_depth > QRX_DEPTH);
            snprintf(g_display_label, sizeof(g_display_label),
                     "AROS public-screen host %dx%dx%d",
                     (int)g_screen_width, (int)g_screen_height,
                     (int)g_screen_depth);
            PutStr("QRX: using existing AROS public screen as compatibility host.\n");
        }
    }

    if (!g_screen) {
        PutStr("QRX: no usable RTG, AGA, Workbench-like, or public host screen found.\n");
        return 0;
    }

    if (!g_screen_borrowed) {
        PubScreenStatus(g_screen, 0);
        SetDefaultPubScreen((UBYTE *)"QRX");
        draw_wallpaper(g_screen);
        apply_look_palette();
    }
    ScreenToFront(g_screen);
    return 1;
'''
if needle not in s: raise SystemExit('final display failure/publication anchor missing')
s=s.replace(needle,rep,1)

# Opening windows on a borrowed public screen uses WA_PubScreen.
winneedle='WA_CustomScreen, (ULONG)g_screen,'
if s.count(winneedle) < 3: raise SystemExit('expected at least three QRX window screen tags')
s=s.replace(winneedle,'(g_screen_borrowed ? WA_PubScreen : WA_CustomScreen), (ULONG)g_screen,')

# Never repaint the owner's screen; only paint QRX's full-screen backdrop window.
needle='''static void redraw_desktop(void) {\n    struct RastPort *rp;\n    if (!g_screen || !g_desktop) return;\n\n    draw_wallpaper(g_screen);\n    apply_look_palette();\n    rp = g_desktop->RPort;\n'''
rep=r'''static void redraw_desktop(void) {
    struct RastPort *rp;
    if (!g_screen || !g_desktop) return;

    rp = g_desktop->RPort;
    if (!g_screen_borrowed) {
        draw_wallpaper(g_screen);
        apply_look_palette();
    } else {
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_desktop->Width - 1, g_desktop->Height - 1);
    }
'''
if needle not in s: raise SystemExit('redraw desktop anchor missing')
s=s.replace(needle,rep,1)

# Reset ownership state at each open attempt.
needle='''    g_screen = NULL;\n    g_screen_mode = INVALID_ID;\n'''
rep='''    g_screen = NULL;\n    g_screen_borrowed = 0;\n    g_screen_mode = INVALID_ID;\n'''
if needle not in s: raise SystemExit('screen reset anchor missing')
s=s.replace(needle,rep,1)

# Borrowed host must be unlocked, not privatized/closed.
needle='''    if (g_screen) {\n        SetDefaultPubScreen(NULL);\n        PubScreenStatus(g_screen, PSNF_PRIVATE);\n        CloseScreen(g_screen);\n        g_screen = NULL;\n    }\n'''
rep=r'''    if (g_screen) {
        if (g_screen_borrowed) {
            UnlockPubScreen(NULL, g_screen);
        } else {
            SetDefaultPubScreen(NULL);
            PubScreenStatus(g_screen, PSNF_PRIVATE);
            CloseScreen(g_screen);
        }
        g_screen = NULL;
        g_screen_borrowed = 0;
    }
'''
if needle not in s: raise SystemExit('close screen anchor missing')
s=s.replace(needle,rep,1)

# Existing owner windows are not QRX visitor windows in host mode.
s=s.replace('''static int qrx_foreign_window_count(void) {\n    struct Window *w;\n    int count = 0;\n\n    if (!g_screen) return 0;\n''',
'''static int qrx_foreign_window_count(void) {\n    struct Window *w;\n    int count = 0;\n\n    if (!g_screen || g_screen_borrowed) return 0;\n''',1)
s=s.replace('''    if (!g_screen || wanted < 0) return NULL;\n''','''    if (!g_screen || g_screen_borrowed || wanted < 0) return NULL;\n''',1)
s=s.replace('''    if (!g_screen || !rp) return;\n''','''    if (!g_screen || g_screen_borrowed || !rp) return;\n''',1)
needle='''static int has_foreign_windows(void) {\n    struct Window *w;\n\n    if (!g_screen) return 0;\n'''
rep='''static int has_foreign_windows(void) {\n    struct Window *w;\n\n    if (!g_screen || g_screen_borrowed) return 0;\n'''
if needle not in s: raise SystemExit('has_foreign_windows anchor missing')
s=s.replace(needle,rep,1)

# TAKEOVER/WHDLoad cannot safely seize hardware while hosted on Wanderer screen.
needle='''static int qrx_schedule_takeover(const char *command) {\n    if (!command || !*command) {\n'''
rep='''static int qrx_schedule_takeover(const char *command) {\n    if (g_screen_borrowed) {\n        add_line("TAKEOVER unavailable in existing-public-screen host mode.");\n        return 0;\n    }\n    if (!command || !*command) {\n'''
if needle not in s: raise SystemExit('takeover anchor missing')
s=s.replace(needle,rep,1)
needle='''static int qrx_schedule_whdload(const char *slave, const char *data_dir,\n                                const char *options) {\n    if (!slave || !*slave) {\n'''
rep='''static int qrx_schedule_whdload(const char *slave, const char *data_dir,\n                                const char *options) {\n    if (g_screen_borrowed) {\n        add_line("WHDLoad unavailable in existing-public-screen host mode.");\n        return 0;\n    }\n    if (!slave || !*slave) {\n'''
if needle not in s: raise SystemExit('whdload anchor missing')
s=s.replace(needle,rep,1)

p.write_text(s)

check=p.read_text(errors='replace')
required=[
 marker,
 'static int g_screen_borrowed = 0;',
 'static UBYTE qrx_pen(UBYTE pen)',
 'if (g_screen_depth < 8)',
 'ULONG requested_depth = active_depth;',
 'using existing AROS public screen as compatibility host',
 '(g_screen_borrowed ? WA_PubScreen : WA_CustomScreen)',
 'UnlockPubScreen(NULL, g_screen);',
 'TAKEOVER unavailable in existing-public-screen host mode',
]
for token in required:
    if token not in check: raise SystemExit('verification failed: '+token)
if 'active_depth < QRX_DEPTH ? QRX_DEPTH : active_depth' in check:
    raise SystemExit('forced 8-bit current-screen fallback remains')
# ensure LikeWorkbench no longer forces depth in the immediate block
ix=check.find('SA_LikeWorkbench, TRUE')
if ix<0: raise SystemExit('SA_LikeWorkbench missing')
if 'SA_Depth, QRX_DEPTH' in check[ix:ix+220]:
    raise SystemExit('SA_LikeWorkbench still forces 8-bit depth')
print('QRXDesktop v0.20.3.7.4.6.5.7 screen/public-host patch OK:',p)
