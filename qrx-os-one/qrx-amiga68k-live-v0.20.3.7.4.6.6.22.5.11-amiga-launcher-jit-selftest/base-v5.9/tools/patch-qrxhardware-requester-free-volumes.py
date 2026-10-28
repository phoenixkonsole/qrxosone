#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: patch-qrxhardware-requester-free-volumes.py qrx_hardware.c')

p=Path(sys.argv[1])
s=p.read_text(errors='replace')
marker='/* QRX v0.20.3.7.4.6.5.8 requester-free mounted-volume discovery */'
if marker in s:
    print('QRXHardware v0.20.3.7.4.6.5.8 hotfix already present:', p)
    raise SystemExit(0)
if 'static int lock_exists(const char *path)' not in s:
    raise SystemExit('QRXHardware baseline lock_exists anchor missing')

needle='static struct Window *g_window = NULL;\n'
insert=needle + r'''
''' + marker + r'''
#define QRX_HW_VOLUME_MAX 6
#define QRX_HW_VOLUME_NAME 64
static char g_hw_volumes[QRX_HW_VOLUME_MAX][QRX_HW_VOLUME_NAME];
static int g_hw_volume_count = 0;
static struct Process *g_hw_process = NULL;
static APTR g_hw_saved_window_ptr = NULL;
static int g_hw_requesters_disabled = 0;
static int g_hw_default_screen = 0;
'''
s=s.replace(needle,insert,1)

start=s.index('static int lock_exists(const char *path) {')
end=s.index('\nstatic void draw_manager(void) {', start)
replacement=r'''static int hw_ci_char(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int hw_ci_cmp(const char *a, const char *b) {
    int ca, cb;
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b) {
        ca = hw_ci_char((unsigned char)*a++);
        cb = hw_ci_char((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return hw_ci_char((unsigned char)*a) - hw_ci_char((unsigned char)*b);
}

static void hw_disable_requesters(void) {
    if (g_hw_requesters_disabled) return;
    g_hw_process = (struct Process *)FindTask(NULL);
    if (!g_hw_process) return;
    g_hw_saved_window_ptr = g_hw_process->pr_WindowPtr;
    g_hw_process->pr_WindowPtr = (APTR)-1;
    g_hw_requesters_disabled = 1;
}

static void hw_restore_requesters(void) {
    if (!g_hw_requesters_disabled || !g_hw_process) return;
    g_hw_process->pr_WindowPtr = g_hw_saved_window_ptr;
    g_hw_process = NULL;
    g_hw_saved_window_ptr = NULL;
    g_hw_requesters_disabled = 0;
}

static void hw_bstr_to_name(BSTR bstr, char *out, size_t out_size) {
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

static int hw_builtin_volume(const char *name) {
    if (!name || !*name) return 1;
    if (!hw_ci_cmp(name, "RAM Disk")) return 1;
    if (!hw_ci_cmp(name, "AROS Live CD")) return 1;
    if (!hw_ci_cmp(name, "AROS Kickstart")) return 1;
    return 0;
}

static int hw_volume_seen(const char *name) {
    int i;
    for (i = 0; i < g_hw_volume_count; ++i)
        if (!hw_ci_cmp(name, g_hw_volumes[i])) return 1;
    return 0;
}

static void hw_refresh_volumes(void) {
    const ULONG flags = LDF_READ | LDF_VOLUMES;
    struct DosList *cursor = LockDosList(flags);
    struct DosList *entry;
    g_hw_volume_count = 0;
    if (!cursor) return;
    entry = cursor;
    while ((entry = NextDosEntry(entry, flags)) != NULL &&
           g_hw_volume_count < QRX_HW_VOLUME_MAX) {
        char name[QRX_HW_VOLUME_NAME];
        hw_bstr_to_name(entry->dol_Name, name, sizeof(name));
        if (hw_builtin_volume(name) || hw_volume_seen(name)) continue;
        strncpy(g_hw_volumes[g_hw_volume_count], name, QRX_HW_VOLUME_NAME - 1);
        g_hw_volumes[g_hw_volume_count][QRX_HW_VOLUME_NAME - 1] = 0;
        ++g_hw_volume_count;
    }
    UnLockDosList(flags);
}
'''
s=s[:start]+replacement+s[end:]

old=r'''    int usb_any = 0;
    int i;
    char name[16];
'''
new=r'''    int i;
'''
if old not in s: raise SystemExit('QRXHardware USB locals anchor missing')
s=s.replace(old,new,1)

old=r'''    for (i = 0; i < 4; ++i) {
        snprintf(name, sizeof(name), "USB%d:", i);
        if (lock_exists(name)) {
            snprintf(buf, sizeof(buf), "%s mounted and accessible via QRX Drive", name);
            qtext(rp, PEN_TEXT, 34, y, buf);
            y += 16;
            usb_any = 1;
        }
    }

    if (!usb_any) {
        qtext(rp, PEN_MUTED, 34, y,
              "No USB0:-USB3: volume currently mounted.");
        y += 18;
        qtext(rp, PEN_MUTED, 34, y,
              "Poseidon mass-storage volumes remain normal DOS volumes.");
        y += 18;
    }
'''
new=r'''    hw_refresh_volumes();
    if (g_hw_volume_count) {
        for (i = 0; i < g_hw_volume_count; ++i) {
            snprintf(buf, sizeof(buf), "%s: mounted DOS volume", g_hw_volumes[i]);
            qtext(rp, PEN_TEXT, 34, y, buf);
            y += 16;
        }
    } else {
        qtext(rp, PEN_MUTED, 34, y,
              "No additional mounted DOS volumes detected.");
        y += 18;
    }
    qtext(rp, PEN_MUTED, 34, y,
          "Discovery reads DosList only; no USB0: probing/requesters.");
    y += 18;
'''
if old not in s: raise SystemExit('QRXHardware fixed USB probe block missing')
s=s.replace(old,new,1)

needle='    if (!IntuitionBase || !GfxBase || !DOSBase)\n        goto cleanup;\n\n    g_screen = LockPubScreen((UBYTE *)"QRX");\n    if (!g_screen) {\n        PutStr("QRX: public screen \'QRX\' is required.\\n");\n        goto cleanup;\n    }\n'
rep='    if (!IntuitionBase || !GfxBase || !DOSBase)\n        goto cleanup;\n\n    hw_disable_requesters();\n    g_screen = LockPubScreen((UBYTE *)"QRX");\n    if (!g_screen) {\n        g_screen = LockPubScreen(NULL);\n        g_hw_default_screen = g_screen ? 1 : 0;\n    }\n    if (!g_screen) {\n        PutStr("QRX: no public screen available.\\n");\n        goto cleanup;\n    }\n'
if needle not in s: raise SystemExit('QRXHardware public screen anchor missing')
s=s.replace(needle,rep,1)
s=s.replace(needle,rep,1)

s=s.replace('        WA_CustomScreen, (ULONG)g_screen,',
            '        WA_PubScreen, (ULONG)g_screen,',1)

old=r'''    if (g_screen) {
        UnlockPubScreen((UBYTE *)"QRX", g_screen);
        g_screen = NULL;
    }
    if (PoseidonBase) CloseLibrary(PoseidonBase);
'''
new=r'''    if (g_screen) {
        UnlockPubScreen(g_hw_default_screen ? NULL : (UBYTE *)"QRX", g_screen);
        g_screen = NULL;
    }
    hw_restore_requesters();
    if (PoseidonBase) CloseLibrary(PoseidonBase);
'''
if old not in s: raise SystemExit('QRXHardware cleanup anchor missing')
s=s.replace(old,new,1)

for forbidden in ('"USB0:"','"USB1:"','"USB2:"','"USB3:"','USB0:-USB3'):
    if forbidden in s:
        raise SystemExit(f'QRXHardware hard-coded USB probe remains: {forbidden}')
for token in (marker,'LockDosList(flags)','NextDosEntry(entry, flags)','pr_WindowPtr = (APTR)-1','WA_PubScreen'):
    if token not in s: raise SystemExit(f'QRXHardware .5.8 gate missing: {token}')

p.write_text(s)
print('QRXHardware requester-free mounted-volume patch installed:',p)
