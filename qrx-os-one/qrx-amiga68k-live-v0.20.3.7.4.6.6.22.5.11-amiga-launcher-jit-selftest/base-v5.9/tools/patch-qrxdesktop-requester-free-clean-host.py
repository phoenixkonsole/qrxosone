#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: patch-qrxdesktop-requester-free-clean-host.py qrx_desktop.c')

p = Path(sys.argv[1])
s = p.read_text(errors='replace')
marker = '/* QRX v0.20.3.7.4.6.5.8 requester-free volumes + clean public-screen host */'
if marker in s:
    print('QRXDesktop v0.20.3.7.4.6.5.8 hotfix already present:', p)
    raise SystemExit(0)

required = [
    '/* QRX v0.20.3.7.4.6.5.7 existing public screen + low-depth compatibility */',
    'static int g_screen_borrowed = 0;',
    'static int qrx_ci_cmp(const char *a, const char *b)',
    'static int qrx_drive_drop_target(WORD x, WORD y, char *dest, size_t dest_size)',
    'static void redraw_filemanager(void)',
    'static int qrx_open_runtime_screen(void)',
]
for token in required:
    if token not in s:
        raise SystemExit(f'required .5.7 anchor missing: {token}')

# ---------------------------------------------------------------------------
# Runtime state: requester suppression, real mounted-volume list, host palette.
# ---------------------------------------------------------------------------
needle = 'static int g_screen_borrowed = 0;\n'
insert = needle + r'''
''' + marker + r'''
#define QRX_MOUNTED_VOLUME_MAX 4
#define QRX_MOUNTED_VOLUME_NAME 64

struct QRXMountedVolume {
    char name[QRX_MOUNTED_VOLUME_NAME];
    char path[QRX_MOUNTED_VOLUME_NAME + 2];
};

static struct QRXMountedVolume g_mounted_volumes[QRX_MOUNTED_VOLUME_MAX];
static int g_mounted_volume_count = 0;
static struct Process *g_qrx_process = NULL;
static APTR g_qrx_saved_window_ptr = NULL;
static int g_qrx_requesters_disabled = 0;
static ULONG g_host_palette[256 * 3];
static ULONG g_host_palette_count = 0;
static int g_host_palette_saved = 0;
static int g_host_title_was_visible = 0;
'''
s = s.replace(needle, insert, 1)

# Requester guard and DOS-list-only mounted-volume discovery.
needle = '''static int qrx_ci_cmp(const char *a, const char *b) {
    int ca, cb;
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b) {
        ca = qrx_ci_char((unsigned char)*a++);
        cb = qrx_ci_char((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return qrx_ci_char((unsigned char)*a) - qrx_ci_char((unsigned char)*b);
}

'''
addition = needle + r'''static void qrx_disable_dos_requesters(void) {
    if (g_qrx_requesters_disabled) return;
    g_qrx_process = (struct Process *)FindTask(NULL);
    if (!g_qrx_process) return;
    g_qrx_saved_window_ptr = g_qrx_process->pr_WindowPtr;
    g_qrx_process->pr_WindowPtr = (APTR)-1;
    g_qrx_requesters_disabled = 1;
}

static void qrx_restore_dos_requesters(void) {
    if (!g_qrx_requesters_disabled || !g_qrx_process) return;
    g_qrx_process->pr_WindowPtr = g_qrx_saved_window_ptr;
    g_qrx_requesters_disabled = 0;
    g_qrx_process = NULL;
    g_qrx_saved_window_ptr = NULL;
}

static void qrx_bstr_to_name(BSTR bstr, char *out, size_t out_size) {
    UBYTE *src;
    ULONG len;
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!bstr) return;
    src = (UBYTE *)BADDR(bstr);
    if (!src) return;
    len = src[0];
    if (len >= out_size) len = (ULONG)out_size - 1;
    if (len) CopyMem(src + 1, out, len);
    out[len] = 0;
}

static int qrx_volume_name_is_builtin(const char *name) {
    if (!name || !*name) return 1;
    if (!qrx_ci_cmp(name, "RAM Disk")) return 1;
    if (!qrx_ci_cmp(name, "AROS Live CD")) return 1;
    if (!qrx_ci_cmp(name, "AROS Kickstart")) return 1;
    return 0;
}

static int qrx_volume_name_seen(const char *name) {
    int i;
    for (i = 0; i < g_mounted_volume_count; ++i)
        if (!qrx_ci_cmp(name, g_mounted_volumes[i].name)) return 1;
    return 0;
}

static void qrx_refresh_mounted_volumes(void) {
    const ULONG flags = LDF_READ | LDF_VOLUMES;
    struct DosList *cursor;
    struct DosList *entry;

    g_mounted_volume_count = 0;
    cursor = LockDosList(flags);
    if (!cursor) return;

    entry = cursor;
    while ((entry = NextDosEntry(entry, flags)) != NULL &&
           g_mounted_volume_count < QRX_MOUNTED_VOLUME_MAX) {
        char name[QRX_MOUNTED_VOLUME_NAME];
        qrx_bstr_to_name(entry->dol_Name, name, sizeof(name));
        if (qrx_volume_name_is_builtin(name) || qrx_volume_name_seen(name))
            continue;
        strncpy(g_mounted_volumes[g_mounted_volume_count].name,
                name, sizeof(g_mounted_volumes[g_mounted_volume_count].name) - 1);
        g_mounted_volumes[g_mounted_volume_count].name[
            sizeof(g_mounted_volumes[g_mounted_volume_count].name) - 1] = 0;
        snprintf(g_mounted_volumes[g_mounted_volume_count].path,
                 sizeof(g_mounted_volumes[g_mounted_volume_count].path),
                 "%s:", name);
        ++g_mounted_volume_count;
    }
    UnLockDosList(flags);
}

static int qrx_drive_path_for_row(int row, char *dest, size_t dest_size) {
    static const char *base[] = {"SYS:", "QRX:", "RAM:", "T:"};
    if (!dest || dest_size == 0 || row < 0) return 0;
    if (row < 4) {
        snprintf(dest, dest_size, "%s", base[row]);
        return 1;
    }
    row -= 4;
    if (row < 0 || row >= g_mounted_volume_count) return 0;
    snprintf(dest, dest_size, "%s", g_mounted_volumes[row].path);
    return 1;
}

static int qrx_drive_row_from_y(WORD y) {
    int delta, row;
    if (y < 116) return -1;
    delta = y - 116;
    row = delta / 26;
    if ((delta % 26) > 22) return -1;
    if (row >= 4 + g_mounted_volume_count) return -1;
    return row;
}

'''
if needle not in s:
    raise SystemExit('qrx_ci_cmp function anchor missing')
s = s.replace(needle, addition, 1)


# v6.6.16: drawers must navigate inside the existing file manager.
old_open_entry = r'''static LONG qrx_open_file_entry(int index) {
    struct QRXFileEntry *e;
    char full[FILE_PATH_LEN + 128];
    if (index < 0 || index >= g_file_count) return 0;
    e = &g_file_entries[index];
    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    return qrx_open_path(full, e->dir_type, e->has_icon,
                         e->icon_type, e->default_tool);
}
'''
new_open_entry = r'''static LONG qrx_open_file_entry(int index) {
    struct QRXFileEntry *e;
    char full[FILE_PATH_LEN + 128];
    if (index < 0 || index >= g_file_count) return 0;
    e = &g_file_entries[index];
    qrx_join_path(full, sizeof(full), g_files_path, e->name);
    /* Drawers are navigation, not external objects: keep one browser window. */
    if (e->dir_type > 0) {
        if (qrx_load_directory(full)) { qrx_set_status(""); return 1; }
        qrx_set_status("Cannot enter drawer.");
        redraw_filemanager();
        return 0;
    }
    return qrx_open_path(full, e->dir_type, e->has_icon,
                         e->icon_type, e->default_tool);
}
'''
if old_open_entry not in s:
    raise SystemExit('file entry open anchor missing for hierarchical drawer navigation')
s = s.replace(old_open_entry, new_open_entry, 1)

# Replace fixed USB drop targets with row-driven real volume targets.
start = s.index('static int qrx_drive_drop_target(WORD x, WORD y, char *dest, size_t dest_size) {')
end = s.index('\nstatic void redraw_filemanager(void) {', start)
replacement = r'''static int qrx_drive_drop_target(WORD x, WORD y, char *dest, size_t dest_size) {
    int row;
    if (x < 10 || x > 92) return 0;
    row = qrx_drive_row_from_y(y);
    return qrx_drive_path_for_row(row, dest, dest_size);
}
'''
s = s[:start] + replacement + s[end:]

# Replace fixed USB0/USB1 paint probes with dynamic DOS-list volume buttons.
old = r'''    text_at(rp, PEN_MUTED, 14, 106, "DRIVES");
    fill(rp, PEN_BUTTON, 10, 116, 92, 138); text_at(rp, PEN_TEXT, 22, 131, "SYS:");
    fill(rp, PEN_BUTTON, 10, 142, 92, 164); text_at(rp, PEN_TEXT, 22, 157, "QRX:");
    fill(rp, PEN_BUTTON, 10, 168, 92, 190); text_at(rp, PEN_TEXT, 22, 183, "RAM:");
    fill(rp, PEN_BUTTON, 10, 194, 92, 216); text_at(rp, PEN_TEXT, 22, 209, "T:");
    fill(rp, qrx_path_exists("USB0:") ? PEN_ACCENT : PEN_BUTTON,
         10, 220, 92, 242);
    text_at(rp, qrx_path_exists("USB0:") ? PEN_BLACK : PEN_TEXT,
            22, 235, "USB0:");
    fill(rp, qrx_path_exists("USB1:") ? PEN_ACCENT : PEN_BUTTON,
         10, 246, 92, 268);
    text_at(rp, qrx_path_exists("USB1:") ? PEN_BLACK : PEN_TEXT,
            22, 261, "USB1:");
'''
new = r'''    qrx_refresh_mounted_volumes();
    text_at(rp, PEN_MUTED, 14, 106, "LOCATIONS");
    {
        int drow;
        char dpath[QRX_MOUNTED_VOLUME_NAME + 2];
        char dlabel[12];
        for (drow = 0; drow < 4 + g_mounted_volume_count; ++drow) {
            WORD dy = 116 + drow * 26;
            if (!qrx_drive_path_for_row(drow, dpath, sizeof(dpath))) continue;
            if (drow < 4) {
                snprintf(dlabel, sizeof(dlabel), "%s", dpath);
            } else {
                snprintf(dlabel, sizeof(dlabel), "%.9s:",
                         g_mounted_volumes[drow - 4].name);
            }
            fill(rp, drow >= 4 ? PEN_ACCENT : PEN_BUTTON, 10, dy, 92, dy + 22);
            outline(rp, PEN_EDGE, 10, dy, 92, dy + 22);
            text_at(rp, drow >= 4 ? PEN_BLACK : PEN_TEXT, 16, dy + 15, dlabel);
        }
        if (g_mounted_volume_count == 0)
            text_at(rp, PEN_MUTED, 16, 237, "no media");
    }
'''
if old not in s:
    raise SystemExit('fixed USB draw block missing')
s = s.replace(old, new, 1)

# Replace fixed USB click handlers with dynamic drive row mapping.
old = r'''    if (x >= 10 && x <= 92) {
        if (y >= 116 && y <= 138) { qrx_load_directory("SYS:"); qrx_set_status(""); redraw_filemanager(); return; }
        if (y >= 142 && y <= 164) { qrx_load_directory("QRX:"); qrx_set_status(""); redraw_filemanager(); return; }
        if (y >= 168 && y <= 190) { qrx_load_directory("RAM:"); qrx_set_status(""); redraw_filemanager(); return; }
        if (y >= 194 && y <= 216) { qrx_load_directory("T:"); qrx_set_status(""); redraw_filemanager(); return; }
        if (y >= 220 && y <= 242) { qrx_load_directory("USB0:"); qrx_set_status(""); redraw_filemanager(); return; }
        if (y >= 246 && y <= 268) { qrx_load_directory("USB1:"); qrx_set_status(""); redraw_filemanager(); return; }
    }
'''
new = r'''    if (x >= 10 && x <= 92) {
        char drive_path[QRX_MOUNTED_VOLUME_NAME + 2];
        int drive_row = qrx_drive_row_from_y(y);
        if (qrx_drive_path_for_row(drive_row, drive_path, sizeof(drive_path))) {
            if (!qrx_load_directory(drive_path))
                qrx_set_status("Volume unavailable or media removed.");
            else
                qrx_set_status("");
            redraw_filemanager();
            return;
        }
    }
'''
if old not in s:
    raise SystemExit('fixed USB click block missing')
s = s.replace(old, new, 1)

# Replace terminal USB probing with the same real mounted-volume list.
old = r'''    } else if (!strcmp(cmd, "usb")) {
        int i, mounted = 0;
        add_line(PoseidonBase ? "Poseidon library: available" : "Poseidon library: not detected");
        for (i = 0; i < 4; ++i) {
            char usbname[16];
            snprintf(usbname, sizeof(usbname), "USB%d:", i);
            if (qrx_path_exists(usbname)) {
                snprintf(buf, sizeof(buf), "%s mounted", usbname);
                add_line(buf);
                mounted = 1;
            }
        }
        if (!mounted)
            add_line("No USB0:-USB3: DOS volume mounted.");
        add_line("Any mounted USB volume can be opened with: files <volume>:");
'''
new = r'''    } else if (!strcmp(cmd, "usb")) {
        int i;
        add_line(PoseidonBase ? "Poseidon library: available" : "Poseidon library: not detected");
        qrx_refresh_mounted_volumes();
        if (g_mounted_volume_count == 0) {
            add_line("No additional mounted DOS volumes detected.");
        } else {
            for (i = 0; i < g_mounted_volume_count; ++i) {
                snprintf(buf, sizeof(buf), "%s mounted", g_mounted_volumes[i].path);
                add_line(buf);
            }
        }
        add_line("Only volumes already present in the DOS list are shown; no USB0: probing.");
'''
if old not in s:
    raise SystemExit('terminal fixed USB probe block missing')
s = s.replace(old, new, 1)

# ---------------------------------------------------------------------------
# Borrowed screen: preserve palette/title, use it FIRST, hide owner UI behind
# a full-screen normal QRX host window, and restore state on exit.
# ---------------------------------------------------------------------------
s = s.replace(
    '    /* Never change the palette of a screen owned by Wanderer/another task. */\n    if (g_screen_borrowed) return;\n',
    '    /* Borrowed screens are mutated only after their palette was saved. */\n    if (g_screen_borrowed && !g_host_palette_saved) return;\n', 1)

# Make borrowed redraw use the saved/restorable QRX palette.
old = r'''    if (!g_screen_borrowed) {
        draw_wallpaper(g_screen);
        apply_look_palette();
    } else {
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_desktop->Width - 1, g_desktop->Height - 1);
    }
'''
new = r'''    if (!g_screen_borrowed) {
        draw_wallpaper(g_screen);
        apply_look_palette();
    } else {
        apply_look_palette();
        SetAPen(rp, qrx_pen(PEN_PANEL_2));
        RectFill(rp, 0, 0, g_desktop->Width - 1, g_desktop->Height - 1);
    }
'''
if old not in s:
    raise SystemExit('borrowed redraw anchor missing')
s = s.replace(old, new, 1)

# Insert host state helpers before run_desktop geometry.
needle = 'static void qrx_clamp_runtime_geometry(void) {\n'
helpers = r'''static void qrx_save_host_palette(void) {
    ULONG count;
    if (!g_screen || !g_screen_borrowed || !g_screen->ViewPort.ColorMap) return;
    count = g_screen_depth >= 8 ? 256UL : (1UL << (g_screen_depth ? g_screen_depth : 1));
    if (count > 256UL) count = 256UL;
    GetRGB32(g_screen->ViewPort.ColorMap, 0, count, g_host_palette);
    g_host_palette_count = count;
    g_host_palette_saved = 1;
}

static void qrx_restore_host_palette(void) {
    ULONG i;
    if (!g_screen || !g_host_palette_saved) return;
    for (i = 0; i < g_host_palette_count; ++i) {
        SetRGB32(&g_screen->ViewPort, i,
                 g_host_palette[i * 3],
                 g_host_palette[i * 3 + 1],
                 g_host_palette[i * 3 + 2]);
    }
    g_host_palette_saved = 0;
    g_host_palette_count = 0;
}

static int qrx_adopt_public_screen(struct Screen *host) {
    ULONG host_depth;
    if (!host) return 0;
    host_depth = GetBitMapAttr(host->RastPort.BitMap, BMA_DEPTH);
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
             "AROS public host %dx%dx%d",
             (int)g_screen_width, (int)g_screen_height,
             (int)g_screen_depth);
    g_host_title_was_visible = (host->Flags & SHOWTITLE) ? 1 : 0;
    qrx_save_host_palette();
    apply_look_palette();
    if (g_host_title_was_visible) ShowTitle(host, FALSE);
    PutStr("QRX: clean host mode on existing AROS public screen.\n");
    return 1;
}

'''
if needle not in s:
    raise SystemExit('geometry helper anchor missing')
s = s.replace(needle, helpers + needle, 1)

# Compact borrowed classic geometry: terminal and file manager each occupy the
# clean content surface rather than exposing strips of the owner shell.
needle = '''static void qrx_clamp_runtime_geometry(void) {
    if (g_term_width > g_screen_width)
'''
rep = r'''static void qrx_clamp_runtime_geometry(void) {
    if (g_screen_borrowed && (g_screen_width <= 700 || g_screen_height <= 550)) {
        g_term_left = 12;
        g_term_top = 34;
        g_term_width = g_screen_width - 24;
        g_term_height = g_screen_height - 46;
        g_files_left = 12;
        g_files_top = 34;
        g_files_width = g_screen_width - 24;
        g_files_height = g_screen_height - 46;
    }

    if (g_term_width > g_screen_width)
'''
if needle not in s:
    raise SystemExit('qrx_clamp_runtime_geometry preamble missing')
s = s.replace(needle, rep, 1)

# Full-screen borrowed QRX window must be a normal front window, not BACKDROP.
old = r'''    g_desktop = OpenWindowTags(NULL,
        (g_screen_borrowed ? WA_PubScreen : WA_CustomScreen), (ULONG)g_screen,
        WA_Left, 0,
        WA_Top, 0,
        WA_Width, g_screen_width,
        WA_Height, g_screen_height,
        WA_Flags, WFLG_BACKDROP | WFLG_BORDERLESS | WFLG_SMART_REFRESH |
                  WFLG_RMBTRAP,
'''
new = r'''    g_desktop = OpenWindowTags(NULL,
        (g_screen_borrowed ? WA_PubScreen : WA_CustomScreen), (ULONG)g_screen,
        WA_Left, 0,
        WA_Top, 0,
        WA_Width, g_screen_width,
        WA_Height, g_screen_height,
        WA_Flags, (g_screen_borrowed ? 0 : WFLG_BACKDROP) |
                  WFLG_BORDERLESS | WFLG_SMART_REFRESH | WFLG_RMBTRAP,
'''
if old not in s:
    raise SystemExit('desktop OpenWindowTags flag block missing')
s = s.replace(old, new, 1)

needle = '''    if (!g_desktop) return 0;

    g_terminal = OpenWindowTags(NULL,
'''
rep = r'''    if (!g_desktop) return 0;
    if (g_screen_borrowed) WindowToFront(g_desktop);

    g_terminal = OpenWindowTags(NULL,
'''
if needle not in s:
    raise SystemExit('desktop fronting anchor missing')
s = s.replace(needle, rep, 1)

# Prefer the already proven Wanderer/Workbench public screen before creating a
# second screen. This is the key .5.8 host policy.
needle = '''    snprintf(g_display_label, sizeof(g_display_label), "AGA 640x512x8");

    for (i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
'''
rep = r'''    snprintf(g_display_label, sizeof(g_display_label), "AGA 640x512x8");
    g_host_palette_saved = 0;
    g_host_palette_count = 0;
    g_host_title_was_visible = 0;

    {
        struct Screen *host = LockPubScreen(NULL);
        if (host) qrx_adopt_public_screen(host);
    }

    for (i = 0; !g_screen && i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
'''
if needle not in s:
    raise SystemExit('runtime screen candidate preamble missing')
s = s.replace(needle, rep, 1)

# Use helper in final fallback too, avoiding duplicate borrowed-screen setup.
old = r'''    if (!g_screen) {
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
'''
new = r'''    if (!g_screen) {
        struct Screen *host = LockPubScreen(NULL);
        if (host) qrx_adopt_public_screen(host);
    }
'''
if old not in s:
    raise SystemExit('legacy borrowed public-screen fallback block missing')
s = s.replace(old, new, 1)

# Restore owner screen state before unlocking it.
old = r'''        if (g_screen_borrowed) {
            UnlockPubScreen(NULL, g_screen);
        } else {
'''
new = r'''        if (g_screen_borrowed) {
            if (g_host_title_was_visible) ShowTitle(g_screen, TRUE);
            qrx_restore_host_palette();
            UnlockPubScreen(NULL, g_screen);
            g_host_title_was_visible = 0;
        } else {
'''
if old not in s:
    raise SystemExit('borrowed screen close block missing')
s = s.replace(old, new, 1)

# Suppress DOS missing-volume requesters for the whole QRXDesktop process.
needle = '''    if (!IntuitionBase || !GfxBase || !DOSBase)
        goto cleanup;

    load_preferences();
'''
rep = r'''    if (!IntuitionBase || !GfxBase || !DOSBase)
        goto cleanup;

    qrx_disable_dos_requesters();
    load_preferences();
'''
if needle not in s:
    raise SystemExit('main requester-disable anchor missing')
s = s.replace(needle, rep, 1)

needle = '''cleanup:
    qrx_close_runtime_screen();

    if (PoseidonBase) CloseLibrary(PoseidonBase);
'''
rep = r'''cleanup:
    qrx_close_runtime_screen();
    qrx_restore_dos_requesters();

    if (PoseidonBase) CloseLibrary(PoseidonBase);
'''
if needle not in s:
    raise SystemExit('main requester-restore anchor missing')
s = s.replace(needle, rep, 1)

# Gate: no hard-coded USB probes may remain in QRXDesktop.
for forbidden in ('"USB0:"', '"USB1:"', '"USB2:"', '"USB3:"', 'USB0:-USB3'):
    if forbidden in s:
        raise SystemExit(f'hard-coded removable-volume probe remains: {forbidden}')

for token in (
    marker,
    'LockDosList(flags)',
    'NextDosEntry(entry, flags)',
    'g_qrx_process->pr_WindowPtr = (APTR)-1;',
    'qrx_adopt_public_screen(host)',
    'ShowTitle(host, FALSE)',
    '(g_screen_borrowed ? 0 : WFLG_BACKDROP)',
    'Only volumes already present in the DOS list are shown',
):
    if token not in s:
        raise SystemExit(f'v0.20.3.7.4.6.5.8 source gate missing: {token}')

p.write_text(s)
print('QRXDesktop requester-free volume discovery + clean host patch installed:', p)
