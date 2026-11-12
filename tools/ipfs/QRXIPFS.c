/*
 * QRXIPFS.c - native 68k AROS front end for the local IPFS daemon.
 *
 * Publish files and drawers, see the node, read the revision log and
 * open the public IPFS: volume.  The warning is deliberately part of
 * the window, not a one-off requester.
 */
#include "qrx_ipfs.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/asl.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <proto/asl.h>

#include <string.h>
#include <stdio.h>

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct Library *AslBase = NULL;

static struct Screen *g_screen;
static struct Window *g_win;
static char g_log[4096] = "";
static char g_status[160] = "Bereit.";
static int  g_confirm = 0;          /* 1 = publish file, 2 = publish drawer */
static char g_pending[420] = "";

#define WIN_W 620
#define WIN_H 450
#define LOG_TOP 150

static void draw_text(struct RastPort *rp, UBYTE pen, WORD x, WORD y, const char *s)
{
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (CONST_STRPTR)s, (LONG)strlen(s));
}

static void fill_box(struct RastPort *rp, UBYTE pen, WORD x1, WORD y1, WORD x2, WORD y2)
{
    SetAPen(rp, pen);
    RectFill(rp, x1, y1, x2, y2);
}

static void outline_box(struct RastPort *rp, UBYTE pen, WORD x1, WORD y1, WORD x2, WORD y2)
{
    SetAPen(rp, pen);
    Move(rp, x1, y1); Draw(rp, x2, y1);
    Draw(rp, x2, y2); Draw(rp, x1, y2); Draw(rp, x1, y1);
}

static void button(struct RastPort *rp, WORD x, WORD w, const char *label)
{
    fill_box(rp, 3, x, 82, x + w, 106);
    outline_box(rp, 1, x, 82, x + w, 106);
    draw_text(rp, 1, x + 8, 98, label);
}

static void append_log(const char *line)
{
    unsigned long have = (unsigned long)strlen(g_log);
    unsigned long add = (unsigned long)strlen(line);
    if (have + add + 2 >= sizeof(g_log)) {
        unsigned long cut = have / 2;
        memmove(g_log, g_log + cut, have - cut + 1);
        have -= cut;
    }
    memcpy(g_log + have, line, add);
    g_log[have + add] = '\n';
    g_log[have + add + 1] = 0;
}

static void draw_wrapped(struct RastPort *rp, UBYTE pen, WORD x, WORD y,
                         const char *text, int cols)
{
    char line[100];
    int n;

    while (*text && y < LOG_TOP - 8) {
        const char *space = NULL;
        int i;
        for (i = 0; i < cols && text[i]; ++i) {
            if (text[i] == ' ') space = text + i;
        }
        if (!text[i]) n = i;
        else if (space) n = (int)(space - text);
        else n = i;
        if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
        memcpy(line, text, n);
        line[n] = 0;
        draw_text(rp, pen, x, y, line);
        text += n;
        while (*text == ' ') ++text;
        y += 10;
    }
}

static void draw(void)
{
    struct RastPort *rp = g_win->RPort;
    WORD y;
    int i;

    fill_box(rp, 0, 8, 6, WIN_W - 10, 60);
    outline_box(rp, 1, 8, 6, WIN_W - 10, 60);
    draw_text(rp, 1, 14, 20, "IPFS IST OEFENTLICH");
    draw_wrapped(rp, 2, 14, 32, QRX_IPFS_WARNING, 88);

    draw_text(rp, 3, 12, 76, g_status);

    button(rp, 10, 78, "STATUS");
    button(rp, 94, 78, "DATEI");
    button(rp, 178, 82, "DRAWER");
    button(rp, 266, 64, "SYNC");
    button(rp, 336, 104, "REVISIONEN");
    button(rp, 446, 76, "VOLUME");
    button(rp, 528, 82, "NODE");

    fill_box(rp, 0, 8, LOG_TOP - 4, WIN_W - 10, WIN_H - 12);
    outline_box(rp, 1, 8, LOG_TOP - 4, WIN_W - 10, WIN_H - 12);
    y = LOG_TOP + 8;
    {
        int lines = 0;
        int start = 0;
        for (i = 0; g_log[i]; ++i) if (g_log[i] == '\n') ++lines;
        if (lines > 26) {
            int skip = lines - 26;
            for (i = 0; g_log[i] && skip; ++i)
                if (g_log[i] == '\n') --skip;
            start = i;
        }
        for (i = start; g_log[i] && y < WIN_H - 18; ++i) {
            char line[140];
            int n = 0;
            while (g_log[i] && g_log[i] != '\n' && n < (int)sizeof(line) - 1)
                line[n++] = g_log[i++];
            if (!g_log[i] && n == 0) break;
            line[n] = 0;
            draw_text(rp, 3, 14, y, line);
            y += 10;
        }
    }

    if (g_confirm) {
        WORD bx = 90, by = 150, bw = 440, bh = 150;
        fill_box(rp, 0, bx, by, bx + bw, by + bh);
        outline_box(rp, 2, bx, by, bx + bw, by + bh);
        draw_text(rp, 2, bx + 14, by + 20, "WIRKLICH IN IPFS VEROEFFENTLICHEN?");
        draw_wrapped(rp, 3, bx + 14, by + 38,
                     "Die Datei wird weltweit abrufbar und erhaelt einen "
                     "unveraenderlichen CID. Auch spaeteres Loeschen macht "
                     "fruehere Kopien nicht ungeschehen.", 62);
        fill_box(rp, 2, bx + 16, by + bh - 36, bx + 210, by + bh - 12);
        draw_text(rp, 1, bx + 30, by + bh - 22, "JA, VEROEFFENTLICHEN");
        fill_box(rp, 3, bx + 226, by + bh - 36, bx + 330, by + bh - 12);
        draw_text(rp, 1, bx + 250, by + bh - 22, "ABBRECHEN");
    }
}

static int file_request(struct Window *w, int drawer_only, char *dest, unsigned long cap)
{
    struct FileRequester *fr;
    if (!AslBase) return 0;
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, w,
        ASLFR_TitleText, (ULONG)(drawer_only ? "IPFS: Drawer waehlen" : "IPFS: Datei waehlen"),
        ASLFR_InitialDrawer, (ULONG)"IPFS:",
        ASLFR_DoSaveMode, FALSE,
        ASLFR_DrawersOnly, drawer_only ? TRUE : FALSE,
        TAG_DONE);
    if (!fr) return 0;
    if (AslRequestTags(fr, TAG_DONE)) {
        const char *d = (const char *)fr->fr_Drawer;
        const char *f = (const char *)fr->fr_File;
        int dl;
        if (!d) d = "IPFS:";
        dl = (int)strlen(d);
        if (drawer_only || !f || !*f)
            snprintf(dest, cap, "%s", d);
        else
            snprintf(dest, cap, "%s%s%s", d,
                     (dl && d[dl - 1] != ':' && d[dl - 1] != '/') ? "/" : "", f);
        FreeAslRequest(fr);
        return dest[0] != 0;
    }
    FreeAslRequest(fr);
    return 0;
}

static void do_status(void)
{
    char agent[64], peer[128];
    if (qrx_ipfs_status(agent, sizeof(agent), peer, sizeof(peer))) {
        char line[220];
        snprintf(line, sizeof(line), "IPFS bereit: %s", agent);
        append_log(line);
        snprintf(line, sizeof(line), "Peer-ID: %s", peer);
        append_log(line);
        snprintf(line, sizeof(line), "API: %s", qrx_ipfs_api());
        append_log(line);
        snprintf(g_status, sizeof(g_status), "Verbunden mit %s", qrx_ipfs_api());
    } else {
        append_log(qrx_ipfs_error());
        snprintf(g_status, sizeof(g_status), "Nicht verbunden: %s", qrx_ipfs_error());
    }
}

static void publish(const char *path)
{
    char cid[128];
    char line[300];
    if (!path || !*path) return;
    snprintf(g_status, sizeof(g_status), "Veroeffentliche %s ...", path);
    draw();
    if (qrx_ipfs_ensure_volume() && qrx_ipfs_add(path, 1, 1, cid, sizeof(cid))) {
        qrx_ipfs_revision_append(QRX_IPFS_REVISION_LOG, path, cid, 0);
        snprintf(line, sizeof(line), "OK  %s", path);
        append_log(line);
        snprintf(line, sizeof(line), "CID %s", cid);
        append_log(line);
        snprintf(g_status, sizeof(g_status), "Veroeffentlicht: %.70s", cid);
    } else {
        snprintf(line, sizeof(line), "FEHLER  %s", qrx_ipfs_error());
        append_log(line);
        snprintf(g_status, sizeof(g_status), "Fehler: %s", qrx_ipfs_error());
    }
}

static void click(WORD x, WORD y)
{
    if (g_confirm) {
        if (y >= 264 && y <= 288 && x >= 106 && x <= 300) {
            g_confirm = 0;
            publish(g_pending);
            draw();
        } else if (y >= 264 && y <= 288 && x >= 316 && x <= 420) {
            g_confirm = 0;
            snprintf(g_status, sizeof(g_status), "Veroeffentlichung abgebrochen.");
            draw();
        }
        return;
    }

    if (y < 82 || y > 106) return;
    if (x >= 10 && x <= 88) {
        do_status();
    } else if (x >= 94 && x <= 172) {
        if (file_request(g_win, 0, g_pending, sizeof(g_pending))) {
            g_confirm = 1;
        }
    } else if (x >= 178 && x <= 260) {
        if (file_request(g_win, 1, g_pending, sizeof(g_pending))) {
            g_confirm = 2;
        }
    } else if (x >= 266 && x <= 330) {
        int n;
        qrx_ipfs_ensure_volume();
        snprintf(g_status, sizeof(g_status), "Synchronisiere IPFS: ...");
        draw();
        n = qrx_ipfs_volume_sync(QRX_IPFS_VOLUME, QRX_IPFS_REVISION_LOG, 0);
        if (n >= 0) {
            char line[120];
            snprintf(line, sizeof(line), "Sync: %d Datei(en) veroeffentlicht.", n);
            append_log(line);
            snprintf(g_status, sizeof(g_status), "%s", line);
        } else {
            append_log(qrx_ipfs_error());
            snprintf(g_status, sizeof(g_status), "Sync-Fehler: %s", qrx_ipfs_error());
        }
    } else if (x >= 336 && x <= 440) {
        char tail[1200];
        qrx_ipfs_revision_tail(QRX_IPFS_REVISION_LOG, tail, sizeof(tail));
        append_log("--- REVISIONEN ---");
        append_log(tail[0] ? tail : "(noch keine Revisionen)");
        snprintf(g_status, sizeof(g_status), "Revisionen: %s", QRX_IPFS_REVISION_LOG);
    } else if (x >= 446 && x <= 522) {
        if (qrx_ipfs_ensure_volume()) {
            append_log("IPFS: ist eingebunden (SYS:Storage/IPFS).");
            snprintf(g_status, sizeof(g_status), "Volume IPFS: bereit.");
        } else {
            append_log(qrx_ipfs_error());
            snprintf(g_status, sizeof(g_status), "Volume-Fehler: %s", qrx_ipfs_error());
        }
    } else if (x >= 528 && x <= 610) {
        if (SystemTags((CONST_STRPTR)"C:QRXIPFSD", SYS_Asynch, TRUE, TAG_DONE))
            append_log("Lokaler Node gestartet (Port 5001).");
        else
            append_log("Node konnte nicht gestartet werden.");
    }
    draw();
}

int main(void)
{
    struct IntuiMessage *msg;
    int run = 1;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 0);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 0);
    if (!IntuitionBase || !GfxBase || !DOSBase) goto out;

    qrx_ipfs_ensure_volume();
    qrx_ipfs_open();
    append_log("QRXIPFS " QRX_IPFS_VERSION " - IPFS Frontend fuer QRX OS One");
    append_log(QRX_IPFS_PUBLIC_NOTE);
    snprintf(g_status, sizeof(g_status), "API %s", qrx_ipfs_api());

    g_screen = LockPubScreen(NULL);
    if (!g_screen) goto out;
    g_win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)g_screen,
        WA_Left, 40, WA_Top, 40,
        WA_Width, WIN_W, WA_Height, WIN_H,
        WA_Title, (ULONG)"QRX IPFS",
        WA_Flags, WFLG_DRAGBAR | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET |
                  WFLG_SMART_REFRESH | WFLG_ACTIVATE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MOUSEBUTTONS |
                  IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
        TAG_DONE);
    if (!g_win) goto out;

    draw();
    while (run) {
        WaitPort(g_win->UserPort);
        while ((msg = (struct IntuiMessage *)GetMsg(g_win->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            WORD mx = msg->MouseX, my = msg->MouseY;
            ReplyMsg((struct Message *)msg);
            if (cls == IDCMP_CLOSEWINDOW) {
                run = 0;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(g_win);
                draw();
                EndRefresh(g_win, TRUE);
            } else if (cls == IDCMP_VANILLAKEY && (code == 27 || code == 'q' || code == 'Q')) {
                run = 0;
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                click(mx, my);
            }
        }
    }

out:
    if (g_win) CloseWindow(g_win);
    if (g_screen) UnlockPubScreen(NULL, g_screen);
    qrx_ipfs_close();
    if (AslBase) CloseLibrary(AslBase);
    if (DOSBase) CloseLibrary((struct Library *)DOSBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
