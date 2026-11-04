#!/usr/bin/env python3
"""Build canonical QRXDesktop with local patches, leaving the canonical source and
its hash manifests untouched: a layer-safe RTG wallpaper and icon support for
Windows, AmigaOS and AROS (tools/armithlon/desktop).
A synthetic wallpaper is explicitly used only when the original generated
wallpaper is unavailable.
"""
from pathlib import Path
import shutil,subprocess,sys
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
# A Windows program showing its own icon is not an "auto icon".
patch('if (g_file_entries[idx].generated_icon)\n','if (g_file_entries[idx].generated_icon && !qrx_ci_ends(g_file_entries[idx].name, ".exe") && !qrx_ci_ends(g_file_entries[idx].name, ".ico"))\n')
patch('IconBase = OpenLibrary("icon.library", 37);','IconBase = OpenLibrary("icon.library", 37);\n    CyberGfxBase = OpenLibrary("cybergraphics.library", 41);')
patch('    if (IconBase) CloseLibrary(IconBase);','    if (IconBase) { qrx_icon_cache_free(); CloseLibrary(IconBase); }\n    if (CyberGfxBase) CloseLibrary(CyberGfxBase);')
patch('snprintf(buf, sizeof(buf), "%.12s%s",\n                     g_file_entries[idx].name,','snprintf(buf, sizeof(buf), "%.11s%s",\n                     g_file_entries[idx].name,')
# Windows programs run on QRX86, without waiting: Wine, behind QRXWinRun, is not shipped.
patch('''snprintf(command, sizeof(command), "QRX:C/QRXWinRun \\"%s\\"", full);
        execute_capture(command);
        return 1;''','''snprintf(command, sizeof(command), "C:qrx-win32 --chdir \\"%s\\"", full);
        return launch_native(command);''')
(out/'qrx_desktop.armithlon.c').write_text(s)
for f in ('qrx_icons.c','qrx_wicon.h'):shutil.copy(extra/f,out/f)
if not (out/'qrx_wallpaper.h').exists():
 generator=next((root/'qrx-os-one').glob('*/amiga-runtime-22.5.11/tests/desktop/gen-test-wallpaper.py'))
 subprocess.run([sys.executable,str(generator),str(out/'qrx_wallpaper.h')],check=True)
sdk=root/'.build/qrx-sdk/bin'
subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC','-Wno-error=incompatible-pointer-types','-I'+str(out),str(out/'qrx_desktop.armithlon.c'),'-o',str(out/'QRXDesktop')],check=True)
subprocess.run([str(sdk/'elf2hunk'),str(out/'QRXDesktop'),str(out/'QRXDesktop.hunk')],check=True)
