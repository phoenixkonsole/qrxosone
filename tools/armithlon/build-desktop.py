#!/usr/bin/env python3
"""Build canonical QRXDesktop with local patches, leaving the canonical source and
its hash manifests untouched: a layer-safe RTG wallpaper and icon support for
Windows, AmigaOS and AROS (tools/armithlon/desktop).
A synthetic wallpaper is explicitly used only when the original generated
wallpaper is unavailable.
"""
from pathlib import Path
import os,shutil,subprocess,sys
root=Path(__file__).resolve().parents[2]
source=next((root/'qrx-os-one').glob('*/base-v5.9/tools/qrx_desktop.canonical-6.6.20.c'))
extra=root/'tools/armithlon/desktop'
out=root/'.build/qrx-desktop';out.mkdir(parents=True,exist_ok=True)
# The Pack/Unpack menu is part of the shipped desktop but not of the canonical source.
work=out/'qrx_desktop.base.c'
shutil.copy(source,work)
subprocess.run(['patch','-s','--no-backup-if-mismatch',str(work),str(extra/'archive-actions.patch')],check=True)
s=work.read_text()
def patch(old,new):
 global s
 assert s.count(old)==1,old
 s=s.replace(old,new)
patch('rp = &s->RastPort;','/* Keep wallpaper within desktop refresh/layer clipping. */\n    rp = g_desktop ? g_desktop->RPort : &s->RastPort;')
# Game folders hold more than the 64 files the file manager listed.
patch('#define FILE_MAX_ENTRIES 64','#define FILE_MAX_ENTRIES 256')
patch('#include <proto/icon.h>','#include <proto/icon.h>\n#include <graphics/scale.h>')
patch('static void draw_file_icon(struct RastPort *rp, int entry_index, WORD x, WORD y) {','#include "qrx_icons.c"\n\nstatic void draw_file_icon(struct RastPort *rp, int entry_index, WORD x, WORD y) {')
patch('''    struct DiskObject *dobj = NULL;
    struct Image *img = NULL;
    char full[FILE_PATH_LEN + 128];
    int drawn = 0;

    if (entry_index < 0 || entry_index >= g_file_count) return;
    e = &g_file_entries[entry_index];
''','''    char full[FILE_PATH_LEN + 128];
    int drawn = 0;

    if (entry_index < 0 || entry_index >= g_file_count) return;
    e = &g_file_entries[entry_index];
''')
patch('''    if (IconBase) {
        qrx_join_path(full, sizeof(full), g_files_path, e->name);
        dobj = e->has_icon
             ? GetDiskObject((STRPTR)full)
             : GetDiskObjectNew((STRPTR)full);
        if (dobj) {
            img = (struct Image *)dobj->do_Gadget.GadgetRender;
            if (img && img->Width <= FILE_ICON_W && img->Height <= FILE_ICON_H) {
                DrawImage(rp, img, x, y);
                drawn = 1;
            }
            FreeDiskObject(dobj);
        }
    }

    if (drawn) return;
''','''    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    drawn = g_file_icon_view ? qrx_icon_draw(rp, full, x, y, FILE_ICON_W, 46, PEN_PANEL)
                             : qrx_icon_draw(rp, full, x, y, 46, 27, PEN_PANEL);
    if (drawn) return;
''')
patch('''    struct DiskObject *dobj = NULL;
    struct Image *img = NULL;
    int drawn = 0;

    if (index < 0 || index >= g_desktop_count) return;
    e = &g_desktop_entries[index];

    if (IconBase) {
        dobj = e->has_icon
             ? GetDiskObject((STRPTR)e->full_path)
             : GetDiskObjectNew((STRPTR)e->full_path);
        if (dobj) {
            img = (struct Image *)dobj->do_Gadget.GadgetRender;
            if (img && img->Width <= 50 && img->Height <= 34) {
                DrawImage(rp, img, x, y);
                drawn = 1;
            }
            FreeDiskObject(dobj);
        }
    }
''','''    int drawn = 0;

    if (index < 0 || index >= g_desktop_count) return;
    e = &g_desktop_entries[index];
    drawn = qrx_icon_draw(rp, e->full_path, x, y, 50, 34, PEN_PANEL);
''')
# The designs have an icon for everything: no more "auto icon" caption.
patch('''            if (g_file_entries[idx].generated_icon)
                text_at(rp, PEN_MUTED, x + 4, y + 67, "auto icon");
''','')
patch('IconBase = OpenLibrary("icon.library", 37);','IconBase = OpenLibrary("icon.library", 37);\n    CyberGfxBase = OpenLibrary("cybergraphics.library", 41);')
patch('    if (IconBase) CloseLibrary(IconBase);','    if (IconBase) { qrx_icon_cache_free(); CloseLibrary(IconBase); }\n    if (CyberGfxBase) CloseLibrary(CyberGfxBase);')
patch('snprintf(buf, sizeof(buf), "%.12s%s",\n                     g_file_entries[idx].name,','snprintf(buf, sizeof(buf), "%.11s%s",\n                     g_file_entries[idx].name,')
# Windows programs run on QRX86, without waiting: Wine, behind QRXWinRun, is not shipped.
patch('''snprintf(command, sizeof(command), "QRX:C/QRXWinRun \\"%s\\"", full);
        execute_capture(command);
        return 1;''','''snprintf(command, sizeof(command), "C:qrx-win32 --chdir \\"%s\\"", full);
        return launch_native(command);''')
# Dragging: MoveWindow is asynchronous and relative. Another one before the last has
# happened moves too far and queues up, so a drag waits for it and then goes straight
# to the newest position. Refreshes during a move repaint only, without a palette reload.
patch('''        MoveWindow(g_terminal,
                   target_left - g_terminal->LeftEdge,
                   target_top - g_terminal->TopEdge);''','''        qrx_drag_move(g_terminal, &g_term_drag, target_left, target_top);''')
patch('''static int g_files_dragging = 0;''','''struct QRXDragState { LONG sent_left, sent_top; int pending; };
static struct QRXDragState g_term_drag, g_files_drag;
static void qrx_drag_move(struct Window *win, struct QRXDragState *d, LONG left, LONG top) {
    if (d->pending && (win->LeftEdge != d->sent_left || win->TopEdge != d->sent_top)) return;
    if (left == win->LeftEdge && top == win->TopEdge) { d->pending = 0; return; }
    d->sent_left = left;
    d->sent_top = top;
    d->pending = 1;
    MoveWindow(win, left - win->LeftEdge, top - win->TopEdge);
}
static int g_files_dragging = 0;''')
patch('''static void finish_window_gesture(void) {
    if (g_dragging || g_resizing) {''','''static void finish_window_gesture(void) {
    if (g_dragging) {
        /* The last position may have been skipped while a move was on its way. */
        g_term_drag.pending = 0;
        update_window_gesture(0, 0);
        g_term_drag.pending = 0;
    }
    if (g_dragging || g_resizing) {''')
patch('''    if(cls==IDCMP_REFRESHWINDOW){BeginRefresh(g_desktop);redraw_desktop();EndRefresh(g_desktop,TRUE);return;}''','''    if(cls==IDCMP_REFRESHWINDOW){BeginRefresh(g_desktop);g_refresh_only=1;redraw_desktop();g_refresh_only=0;EndRefresh(g_desktop,TRUE);return;}''')
patch('''static void redraw_desktop(void) {
    struct RastPort *rp;
    if (!g_screen || !g_desktop) return;

    rp = g_desktop->RPort;
    if (!g_screen_borrowed) {
        draw_wallpaper(g_screen);
        apply_look_palette();
    } else {
        apply_look_palette();''','''static void redraw_desktop(void) {
    struct RastPort *rp;
    if (!g_screen || !g_desktop) return;

    rp = g_desktop->RPort;
    if (!g_screen_borrowed) {
        draw_wallpaper(g_screen);
        if (!g_refresh_only) apply_look_palette();
    } else {
        if (!g_refresh_only) apply_look_palette();''')
patch('''    for (i = 0; i < QRX_COLORS; ++i) {
        SetRGB32(&s->ViewPort, (ULONG)i,''','''    if (!g_refresh_only) for (i = 0; i < QRX_COLORS; ++i) {
        SetRGB32(&s->ViewPort, (ULONG)i,''')
patch('''static void draw_wallpaper(struct Screen *s) {''','''static int g_refresh_only;
static void draw_wallpaper(struct Screen *s) {''')
patch('''        MoveWindow(g_files,
                   left - g_files->LeftEdge,
                   top - g_files->TopEdge);''','''        qrx_drag_move(g_files, &g_files_drag, left, top);''')
patch('''    g_files_dragging = 0;
    g_files_resizing = 0;
    if (g_files) {
        g_files_left = g_files->LeftEdge;''','''    if (g_files_dragging) {
        /* The last position may have been skipped while a move was on its way. */
        g_files_drag.pending = 0;
        update_file_gesture(0, 0);
        g_files_drag.pending = 0;
    }
    g_files_dragging = 0;
    g_files_resizing = 0;
    if (g_files) {
        g_files_left = g_files->LeftEdge;''')
# ---- wallpaper, design icons, drives, pointer
patch('''static void load_preferences(void) {''','''static char g_wallpaper[64] = "Convergence";
static int g_pointer = 1;
static void load_preferences(void) {''')
patch('''        if (!strncmp(buf, "look=", 5)) {''','''        if (!strncmp(buf, "wallpaper=", 10)) {
            strncpy(g_wallpaper, buf + 10, sizeof(g_wallpaper) - 1);
        } else if (!strncmp(buf, "pointer=", 8)) {
            g_pointer = atoi(buf + 8);
        } else if (!strncmp(buf, "look=", 5)) {''')
patch('''    FPuts(fh, buf);
    Close(fh);
}

static void text_at(''','''    FPuts(fh, buf);
    snprintf(buf, sizeof(buf), "wallpaper=%s\\npointer=%d\\n", g_wallpaper, g_pointer);
    FPuts(fh, buf);
    Close(fh);
}

static void text_at(''')
patch('''static int g_refresh_only;
static void draw_wallpaper(struct Screen *s) {''','''static int g_refresh_only;
static int qrx_wallpaper_draw(struct RastPort *rp);
static void draw_wallpaper(struct Screen *s) {''')
patch('''    SetAPen(rp, 0);
    RectFill(rp, 0, 0, g_screen_width - 1, g_screen_height - 1);

    {
        WORD xoff''','''    if (qrx_wallpaper_draw(rp)) return;
    SetAPen(rp, 0);
    RectFill(rp, 0, 0, g_screen_width - 1, g_screen_height - 1);

    {
        WORD xoff''')
patch('''        if (!g_refresh_only) apply_look_palette();
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_desktop->Width - 1, g_desktop->Height - 1);''','''        if (!g_refresh_only) apply_look_palette();
        if (!qrx_wallpaper_draw(rp)) {
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_desktop->Width - 1, g_desktop->Height - 1);
        }''')
patch('''    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    drawn = g_file_icon_view ? qrx_icon_draw(rp, full, x, y, FILE_ICON_W, 46, PEN_PANEL)
                             : qrx_icon_draw(rp, full, x, y, 46, 27, PEN_PANEL);''','''    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    drawn = g_file_icon_view ? qrx_object_icon_draw(rp, full, e->dir_type > 0, x, y, 48, 48, PEN_PANEL)
                             : qrx_object_icon_draw(rp, full, e->dir_type > 0, x, y, 46, 27, PEN_PANEL);''')
patch('''    drawn = qrx_icon_draw(rp, e->full_path, x, y, 50, 34, PEN_PANEL);''','''    drawn = qrx_object_icon_draw(rp, e->full_path, e->dir_type > 0, x, y, 48, 40, PEN_PANEL);''')
# drives
patch('''static void draw_desktop_disk_icon(struct RastPort *rp, WORD x, WORD y,
                                   const char *label, UBYTE pen) {''','''static void draw_desktop_disk_icon(struct RastPort *rp, WORD x, WORD y,
                                   const char *label, UBYTE pen) {
    int kind = !qrx_ci_cmp(label, "RAM:") ? QK_RAM : !qrx_ci_cmp(label, "APPS:") ? QK_APPS : QK_DISK;
    if (qrx_theme_icon_draw(rp, kind, x - 2, y - 6, 48, 48)) {
        text_at(rp, PEN_TEXT, x - 2, y + 53, label);
        return;
    }''')
patch('''    draw_desktop_disk_icon(rp, 24, 72, "SYS:", PEN_BLUE);
    draw_desktop_disk_icon(rp, 24, 132, "QRX:", PEN_ACCENT);
    draw_desktop_disk_icon(rp, 24, 192, "RAM:", PEN_GREEN);
''','''    if (!g_refresh_only) qrx_refresh_mounted_volumes();
    draw_desktop_disk_icon(rp, 24, 72, "SYS:", PEN_BLUE);
    draw_desktop_disk_icon(rp, 24, 132, "QRX:", PEN_ACCENT);
    draw_desktop_disk_icon(rp, 24, 192, "RAM:", PEN_GREEN);
    {
        int v;
        for (v = 0; v < g_mounted_volume_count && 252 + v * 60 < g_screen_height - 70; v++)
            draw_desktop_disk_icon(rp, 24, (WORD)(252 + v * 60), g_mounted_volumes[v].path, PEN_ACCENT);
    }
''')
patch('''        if (y >= 186 && y <= 240) { open_filemanager("RAM:"); return 1; }
''','''        if (y >= 186 && y <= 240) { open_filemanager("RAM:"); return 1; }
        if (y >= 246 && (y - 246) % 60 <= 54 && (y - 246) / 60 < g_mounted_volume_count) {
            open_filemanager(g_mounted_volumes[(y - 246) / 60].path);
            return 1;
        }
''')
# a filesystem mounted as a device has no volume in the list, as APPS: on ARMithlon
patch('''static void qrx_refresh_mounted_volumes(void) {''','''static void qrx_add_device_volume(const char *dos, const char *name) {
    if (g_mounted_volume_count >= QRX_MOUNTED_VOLUME_MAX || qrx_volume_name_seen(name)) return;
    {
        struct DosList *d = LockDosList(LDF_READ | LDF_DEVICES);
        int found = d && FindDosEntry(d, dos, LDF_DEVICES) != NULL;
        UnLockDosList(LDF_READ | LDF_DEVICES);
        if (!found) return;
    }
    strncpy(g_mounted_volumes[g_mounted_volume_count].name, name, sizeof(g_mounted_volumes[0].name) - 1);
    snprintf(g_mounted_volumes[g_mounted_volume_count].path, sizeof(g_mounted_volumes[0].path), "%s:", name);
    ++g_mounted_volume_count;
}
static void qrx_refresh_mounted_volumes(void) {''')
# the look port and pointer in the main loop
patch('''        wait_mask = term_sig | desk_sig | files_sig;
        signals = Wait(wait_mask);
''','''        wait_mask = term_sig | desk_sig | files_sig | (g_look_port ? 1UL << g_look_port->mp_SigBit : 0);
        signals = Wait(wait_mask);
        if (qrx_look_port_handle()) {
            redraw_desktop();
            save_preferences();
        }
''')
patch('''    term_sig = 1UL << g_terminal->UserPort->mp_SigBit;
    desk_sig = 1UL << g_desktop->UserPort->mp_SigBit;
''','''    term_sig = 1UL << g_terminal->UserPort->mp_SigBit;
    desk_sig = 1UL << g_desktop->UserPort->mp_SigBit;
    qrx_look_port_open();
    qrx_pointer_apply();
''')
patch('''    save_preferences();

    if (g_files) {
        CloseWindow(g_files);
        g_files = NULL;
    }

    CloseWindow(g_terminal);''','''    save_preferences();
    qrx_look_port_close();
    qrx_wallpaper_forget();
    qrx_theme_icons_free();

    if (g_files) {
        CloseWindow(g_files);
        g_files = NULL;
    }

    CloseWindow(g_terminal);''')
if 'QRX_DRAG_STATS' in os.environ:
 patch('''        qrx_drag_move(g_terminal, &g_term_drag, target_left, target_top);''','''        {extern void qrx_stat_move(WORD,WORD,WORD,WORD);qrx_stat_move(target_left,target_top,g_terminal->LeftEdge,g_terminal->TopEdge);}
        qrx_drag_move(g_terminal, &g_term_drag, target_left, target_top);''')
 patch('''        g_dragging = 0;
        g_resizing = 0;
        sync_terminal_geometry();''','''        g_dragging = 0;
        g_resizing = 0;
        {extern void qrx_stat_end(void);qrx_stat_end();}
        sync_terminal_geometry();''')
 s+='''
#include <sys/time.h>
static long g_sm_n,g_sm_lag,g_sm_lagmax;static struct timeval g_sm_t0;
void qrx_stat_move(WORD tx,WORD ty,WORD lx,WORD ly){long l=(tx>lx?tx-lx:lx-tx)+(ty>ly?ty-ly:ly-ty);if(!g_sm_n)gettimeofday(&g_sm_t0,NULL);g_sm_n++;g_sm_lag+=l;if(l>g_sm_lagmax)g_sm_lagmax=l;}
void qrx_stat_end(void){struct timeval t;long ms;gettimeofday(&t,NULL);ms=(t.tv_sec-g_sm_t0.tv_sec)*1000+(t.tv_usec-g_sm_t0.tv_usec)/1000;printf("DRAG moves=%ld ms=%ld per_s=%ld lag_avg=%ld lag_max=%ld\\n",g_sm_n,ms,ms?g_sm_n*1000/ms:0,g_sm_n?g_sm_lag/g_sm_n:0,g_sm_lagmax);fflush(stdout);g_sm_n=g_sm_lag=g_sm_lagmax=0;}
'''
s=s.replace('''static void qrx_refresh_mounted_volumes(void) {
    const ULONG flags''','''static void qrx_refresh_mounted_volumes_only(void);
static void qrx_refresh_mounted_volumes(void) {
    qrx_refresh_mounted_volumes_only();
    qrx_add_device_volume("APPS", "APPS");
}
static void qrx_refresh_mounted_volumes_only(void) {
    const ULONG flags''',1)
# ---- IPFS: public drive (assign), publishing and encrypted volume images ----
# The IPFS: assign is created by Startup-Sequence; show assigns in the drive list,
# but only IPFS (not LIBS:, FONTS: and the other system assigns).
patch('const ULONG flags = LDF_READ | LDF_VOLUMES;',
      'const ULONG flags = LDF_READ | LDF_VOLUMES | LDF_ASSIGNS;')
patch('''        qrx_bstr_to_name(entry->dol_Name, name, sizeof(name));
        if (qrx_volume_name_is_builtin(name) || qrx_volume_name_seen(name))''','''        qrx_bstr_to_name(entry->dol_Name, name, sizeof(name));
        if ((entry->dol_Type == DLT_DIRECTORY || entry->dol_Type == DLT_LATE ||
             entry->dol_Type == DLT_NONBINDING) && qrx_ci_cmp(name, "IPFS")) continue;
        if (qrx_volume_name_is_builtin(name) || qrx_volume_name_seen(name))''')
patch('#define QRX_MOUNTED_VOLUME_MAX 4', '#define QRX_MOUNTED_VOLUME_MAX 8')
patch('#include "qrx_icons.c"', 'static int qrx_file_visible_count(void);\n#include "qrx_icons.c"\n#include "ipfs-drive.c"')
patch('''struct QRXFileEntry {
    char name[108];
    LONG dir_type;
    LONG size;
    ULONG protection;
    UBYTE has_icon;
    UBYTE generated_icon;
    UBYTE icon_type;
    char default_tool[128];
};''','''struct QRXFileEntry {
    char name[108];
    LONG dir_type;
    LONG size;
    ULONG protection;
    UBYTE has_icon;
    UBYTE generated_icon;
    UBYTE icon_type;
    char default_tool[128];
    char ipfs_cid[96];
    UBYTE ipfs_pinned;
};''')
patch('''    qrx_draw_archive_menu(rp);
}''','''    qrx_draw_archive_menu(rp);
    qrx_ipfs_drive_draw(rp);
}''')
patch('''    if (cls == IDCMP_VANILLAKEY && g_rename_active) {''','''    if (cls == IDCMP_VANILLAKEY && qrx_ipfs_text_active()) {
        qrx_ipfs_text_key((int)code);
        redraw_filemanager();
        return;
    }

    if (cls == IDCMP_VANILLAKEY && g_rename_active) {''')
# The IPFS badge lives in the title bar, before the title-drag hit test.
patch('''    if (filemanager_close_hit(x, y)) { close_filemanager(); return; }
    if (filemanager_resize_hit(x, y)) { begin_file_gesture(x, y, 1); return; }
    if (filemanager_title_hit(x, y)) { begin_file_gesture(x, y, 0); return; }''','''    if (filemanager_close_hit(x, y)) { close_filemanager(); return; }
    if (filemanager_resize_hit(x, y)) { begin_file_gesture(x, y, 1); return; }
    if (qrx_ipfs_drive_click(x, y)) return;
    if (filemanager_title_hit(x, y)) { begin_file_gesture(x, y, 0); return; }''')

(out/'qrx_desktop.armithlon.c').write_text(s)
for f in ('qrx_icons.c','qrx_wicon.h','ipfs-drive.c'):shutil.copy(extra/f,out/f)
if not (out/'qrx_wallpaper.h').exists():
 generator=next((root/'qrx-os-one').glob('*/amiga-runtime-22.5.11/tests/desktop/gen-test-wallpaper.py'))
 subprocess.run([sys.executable,str(generator),str(out/'qrx_wallpaper.h')],check=True)
sdk=root/'.build/qrx-sdk/bin'
subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC','-Wno-error=incompatible-pointer-types','-I'+str(out),str(out/'qrx_desktop.armithlon.c'),'-o',str(out/'QRXDesktop')],check=True)
subprocess.run([str(sdk/'elf2hunk'),str(out/'QRXDesktop'),str(out/'QRXDesktop.hunk')],check=True)
subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC',str(extra/'QRXDesktopPrefs.c'),'-o',str(out/'QRXDesktopPrefs')],check=True)
subprocess.run([str(sdk/'elf2hunk'),str(out/'QRXDesktopPrefs'),str(out/'QRXDesktopPrefs.hunk')],check=True)
# Wallpapers and design icons, into the system tree when there is one
system=root/'.build/bundle/SystemFull/QRX'
if system.is_dir() and sys.platform=='darwin':
 subprocess.run(['swiftc','-O',str(extra/'gen-assets.swift'),'-o',str(out/'gen-assets')],check=True)
 subprocess.run([str(out/'gen-assets'),str(system)],check=True)
 # Install both into the tree ARMithlon.app actually launches.
 shutil.copy(out/'QRXDesktop.hunk',system/'C/QRXDesktop')
 shutil.copy(out/'QRXDesktopPrefs.hunk',system/'C/QRXDesktopPrefs')
