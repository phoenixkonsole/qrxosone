/* QRX IPFS settings: node status, gateway information, auto-fetch switch and
 * bootstrap overview.  The panel deliberately reads the small files written
 * by QRXIPFSP2P/QRXIPFSD instead of linking the network stack into a GUI. */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <graphics/rpattr.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct Library *CyberGfxBase;

#define WW 560
#define WH 420
#define CFG_PATH   "SYS:QRX/Prefs/ipfs.cfg"
#define PREFS_DIR  "SYS:QRX/Prefs"
#define STATUS_PATH "IPFS:.qrx-node/p2p-status"
#define BOOT_PATH   "IPFS:.qrx-node/bootstrap"
#define PINS_FILE   "IPFS:.qrx-node/pins"

static int g_autofetch = 1;
static int g_online = 0;
static int g_peers = 0;
static int g_pins = 0;
static char g_bootstrap[180] = "";
static char g_msg[180] = "";

static void trim_line(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

static void load_config(void)
{
    BPTR f = Open((STRPTR)CFG_PATH, MODE_OLDFILE);
    char line[180];

    g_autofetch = 1;
    if (!f) return;
    while (FGets(f, line, sizeof(line))) {
        if (!strncmp(line, "autofetch=", 10))
            g_autofetch = atoi(line + 10) != 0;
    }
    Close(f);
}

static int count_pins(void)
{
    BPTR f;
    char line[128];
    char *seen;
    int n = 0, i, len;

    f = Open((STRPTR)PINS_FILE, MODE_OLDFILE);
    if (!f) return 0;
    seen = (char *)AllocVec(128 * 96, MEMF_ANY);
    if (!seen) {
        Close(f);
        return 0;
    }
    while (FGets(f, line, sizeof(line))) {
        trim_line(line);
        if (line[0] == '-') {
            for (i = 0; i < n; ++i) {
                if (!strcmp(seen + i * 96, line + 1)) {
                    if (i != n - 1)
                        strcpy(seen + i * 96, seen + (n - 1) * 96);
                    --n;
                    break;
                }
            }
            continue;
        }
        if (!line[0]) continue;
        len = (int)strlen(line);
        if (len < 8 || len > 95) continue;
        for (i = 0; i < n; ++i)
            if (!strcmp(seen + i * 96, line)) break;
        if (i == n && n < 128) {
            strncpy(seen + n * 96, line, 95);
            seen[n * 96 + 95] = 0;
            ++n;
        }
    }
    Close(f);
    FreeVec(seen);
    return n;
}

static void load_status(void)
{
    BPTR f;
    char line[220];

    g_online = 0;
    g_peers = 0;
    g_pins = count_pins();
    g_bootstrap[0] = 0;

    f = Open((STRPTR)STATUS_PATH, MODE_OLDFILE);
    if (f) {
        while (FGets(f, line, sizeof(line))) {
            if (!strncmp(line, "online=", 7))
                g_online = atoi(line + 7) != 0;
            else if (!strncmp(line, "peers=", 6))
                g_peers = atoi(line + 6);
        }
        Close(f);
    }

    f = Open((STRPTR)BOOT_PATH, MODE_OLDFILE);
    if (f) {
        if (FGets(f, line, sizeof(line))) {
            trim_line(line);
            strncpy(g_bootstrap, line, sizeof(g_bootstrap) - 1);
            g_bootstrap[sizeof(g_bootstrap) - 1] = 0;
        }
        Close(f);
    }
    if (!g_bootstrap[0])
        strcpy(g_bootstrap, "keine Bootstrap-Datei");
}

static int save_config(void)
{
    BPTR f;

    CreateDir((STRPTR)PREFS_DIR);
    f = Open((STRPTR)CFG_PATH, MODE_NEWFILE);
    if (!f) {
        strcpy(g_msg, "Speichern fehlgeschlagen.");
        return 0;
    }
    FPrintf(f, (STRPTR)"autofetch=%d\n", g_autofetch ? 1 : 0);
    Close(f);
    strcpy(g_msg, "Gespeichert. Auto-Download gilt beim naechsten Node-Start.");
    return 1;
}

static void colour(struct RastPort *rp, ULONG rgb)
{
    SetRPAttrs(rp, RPTAG_PenMode, FALSE, RPTAG_FgColor,
               0xFF000000UL | rgb, TAG_DONE);
}

static void box(struct RastPort *rp, ULONG rgb, int x0, int y0, int x1, int y1)
{
    colour(rp, rgb);
    RectFill(rp, x0, y0, x1, y1);
}

static void label(struct RastPort *rp, ULONG rgb, int x, int y, const char *s)
{
    colour(rp, rgb);
    SetDrMd(rp, JAM1);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, strlen(s));
}

static void button(struct RastPort *rp, int x0, int y0, int x1, int y1,
                   const char *s)
{
    box(rp, 0x2B3A58, x0, y0, x1, y1);
    colour(rp, 0x37E6C8);
    Move(rp, x0, y0); Draw(rp, x1, y0); Draw(rp, x1, y1);
    Draw(rp, x0, y1); Draw(rp, x0, y0);
    label(rp, 0xFFFFFF, x0 + ((x1 - x0) - (int)strlen(s) * 8) / 2, y0 + 15, s);
}

static void button_col(struct RastPort *rp, int x0, int y0, int x1, int y1,
                       ULONG fill, const char *s)
{
    box(rp, fill, x0, y0, x1, y1);
    colour(rp, 0x37E6C8);
    Move(rp, x0, y0); Draw(rp, x1, y0); Draw(rp, x1, y1);
    Draw(rp, x0, y1); Draw(rp, x0, y0);
    label(rp, 0xFFFFFF, x0 + ((x1 - x0) - (int)strlen(s) * 8) / 2, y0 + 15, s);
}

static void draw(struct Window *w)
{
    struct RastPort *rp = w->RPort;
    int ox = w->BorderLeft, oy = w->BorderTop;
    int right = w->Width - w->BorderRight - 1;
    int bottom = w->Height - w->BorderBottom - 1;
    char buf[220];

    box(rp, 0x121826, ox, oy, right, bottom);
    label(rp, 0x37E6C8, ox + 18, oy + 25, "IPFS SETTINGS");
    label(rp, 0xFFFFFF, ox + 18, oy + 42,
          "Lokaler Node, Gateway und Pinning");

    box(rp, 0x1B2537, ox + 16, oy + 54, right - 16, oy + 150);
    colour(rp, 0x37E6C8);
    Move(rp, ox + 16, oy + 54); Draw(rp, right - 16, oy + 54);
    Draw(rp, right - 16, oy + 150); Draw(rp, ox + 16, oy + 150);
    Draw(rp, ox + 16, oy + 54);

    label(rp, 0x8FA6C8, ox + 30, oy + 76, "NODE");
    label(rp, g_online ? 0x55E6A5 : 0xFF6B6B, ox + 150, oy + 76,
          g_online ? "ONLINE / BEREIT" : "OFFLINE / NICHT ERREICHBAR");
    snprintf(buf, sizeof(buf), "P2P-PEERS: %d", g_peers);
    label(rp, 0xFFFFFF, ox + 30, oy + 96, buf);
    snprintf(buf, sizeof(buf), "GATEWAY: http://127.0.0.1:5001");
    label(rp, 0xFFFFFF, ox + 30, oy + 116, buf);
    snprintf(buf, sizeof(buf), "PINNED CIDs: %d", g_pins);
    label(rp, 0xFFFFFF, ox + 30, oy + 136, buf);

    label(rp, 0x37E6C8, ox + 18, oy + 178, "AUTO-DOWNLOAD");
    button_col(rp, ox + 18, oy + 188, ox + 140, oy + 216,
               g_autofetch ? 0x1F6B4A : 0x6B2C3A,
               g_autofetch ? "AN" : "AUS");
    label(rp, 0x8FA6C8, ox + 154, oy + 205,
          "Fehlende Bloecke automatisch ueber P2P nachladen.");

    label(rp, 0x37E6C8, ox + 18, oy + 240, "BOOTSTRAP");
    snprintf(buf, sizeof(buf), "%.64s", g_bootstrap);
    label(rp, 0xFFFFFF, ox + 18, oy + 258, buf);
    if (strlen(g_bootstrap) > 64)
        label(rp, 0xFFFFFF, ox + 18, oy + 272,
              g_bootstrap + 64);

    button(rp, ox + 18, oy + 292, ox + 180, oy + 320, "IPFS ANWENDUNG");
    button(rp, ox + 192, oy + 292, ox + 340, oy + 320, "AKTUALISIEREN");
    button(rp, ox + 352, oy + 292, ox + 504, oy + 320, "SPEICHERN");

    if (g_msg[0])
        label(rp, 0x55E6A5, ox + 18, oy + 344, g_msg);
    label(rp, 0x5E7290, ox + 18, oy + 372,
          "Auto-Download wird beim naechsten Start von QRXIPFSD aktiv.");
}

static void open_ipfs(void)
{
    if (SystemTags((STRPTR)"C:QRXIPFS", SYS_Asynch, TRUE, TAG_DONE) == -1)
        strcpy(g_msg, "QRXIPFS konnte nicht gestartet werden.");
    else
        strcpy(g_msg, "IPFS-Anwendung gestartet.");
}

int main(void)
{
    struct Screen *s;
    struct Window *w;
    int run = 1;

    CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 41);
    if (!CyberGfxBase) {
        puts("QRXIPFSPrefs needs a true colour screen");
        return 20;
    }
    load_config();
    load_status();
    s = LockPubScreen(NULL);
    if (!s) return 20;
    w = OpenWindowTags(NULL,
        WA_PubScreen, (IPTR)s,
        WA_InnerWidth, WW - 20,
        WA_InnerHeight, WH - 30,
        WA_Left, (s->Width - WW) / 2,
        WA_Top, (s->Height - WH) / 2,
        WA_Title, (IPTR)"QRX IPFS Settings",
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_SmartRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_REFRESHWINDOW,
        TAG_DONE);
    UnlockPubScreen(NULL, s);
    if (!w) return 20;
    draw(w);

    while (run) {
        struct IntuiMessage *m;
        WaitPort(w->UserPort);
        while ((m = (struct IntuiMessage *)GetMsg(w->UserPort)) != NULL) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            int x = m->MouseX - w->BorderLeft;
            int y = m->MouseY - w->BorderTop;
            ReplyMsg((struct Message *)m);
            if (cls == IDCMP_CLOSEWINDOW) {
                run = 0;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(w);
                draw(w);
                EndRefresh(w, TRUE);
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (y >= 188 && y <= 216 && x >= 18 && x <= 140) {
                    g_autofetch = !g_autofetch;
                    g_msg[0] = 0;
                } else if (y >= 292 && y <= 320 && x >= 18 && x <= 180) {
                    open_ipfs();
                } else if (y >= 292 && y <= 320 && x >= 192 && x <= 340) {
                    load_config();
                    load_status();
                    strcpy(g_msg, "Status aktualisiert.");
                } else if (y >= 292 && y <= 320 && x >= 352 && x <= 504) {
                    save_config();
                }
                draw(w);
            }
        }
    }

    CloseWindow(w);
    CloseLibrary(CyberGfxBase);
    return 0;
}
