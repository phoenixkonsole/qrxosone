/*
 * QRXVolImage.c - create and open encrypted QRX volume images.
 *
 * Username + password protect the image; the container can then be
 * copied to IPFS for backup or sharing.  The password is never stored
 * in the image and never written into the revision log.
 */
#include "qrx_volimage.h"
#include "qrx_ipfs.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/asl.h>
#include <dos/dos.h>
#include <dos/var.h>
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

#define WIN_W 640
#define WIN_H 430

static char g_user[QRX_VOL_USER_MAX + 1] = "qrx";
static char g_pass[QRX_VOL_PASS_MAX + 1] = "";
static char g_source[300] = "";
static char g_image[300] = "IPFS:volume.qrxvol";
static char g_status[220] = "Bereit.";
static char g_log[4096] = "";
static int  g_field = 0;              /* 0 user, 1 pass, 2 source, 3 image */
static int  g_confirm = 0;            /* publish confirmation */

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

static void button(struct RastPort *rp, WORD x, WORD y, WORD w, const char *label)
{
    fill_box(rp, 3, x, y, x + w, y + 24);
    outline_box(rp, 1, x, y, x + w, y + 24);
    draw_text(rp, 1, x + 8, y + 16, label);
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

static void field(struct RastPort *rp, WORD y, const char *label, const char *value,
                  int active, int masked)
{
    char shown[QRX_VOL_PASS_MAX + 8];
    fill_box(rp, active ? 2 : 0, 8, y, WIN_W - 10, y + 22);
    outline_box(rp, 1, 8, y, WIN_W - 10, y + 22);
    draw_text(rp, active ? 1 : 3, 14, y + 16, label);
    if (masked) {
        int n = (int)strlen(value), i;
        if (n > (int)sizeof(shown) - 2) n = sizeof(shown) - 2;
        for (i = 0; i < n; ++i) shown[i] = '*';
        shown[n] = 0;
    } else {
        snprintf(shown, sizeof(shown), "%s", value);
    }
    draw_text(rp, active ? 1 : 3, 136, y + 16, shown);
}

static void draw(void)
{
    struct RastPort *rp = g_win->RPort;
    int i;

    draw_text(rp, 2, 12, 18, "QRX VOLUME IMAGE  -  verschluesselt wie eine DMG");
    draw_text(rp, 3, 12, 36,
              "Benutzername und Passwort schuetzen den Inhalt. Das Image kann in IPFS gesichert werden.");

    field(rp, 48, "BENUTZER", g_user, g_field == 0, 0);
    field(rp, 76, "PASSWORT", g_pass, g_field == 1, 1);
    field(rp, 104, "QUELLE", g_source, g_field == 2, 0);
    field(rp, 132, "IMAGE", g_image, g_field == 3, 0);
    draw_text(rp, 3, 14, 170, "Feld anklicken oder TAB: Benutzer, Passwort, Quelle, Image");

    button(rp, 8, 182, 92, "QUELLE...");
    button(rp, 106, 182, 92, "IMAGE...");
    button(rp, 204, 182, 110, "ERSTELLEN");
    button(rp, 320, 182, 100, "OEFFNEN");
    button(rp, 426, 182, 90, "PRUEFEN");
    button(rp, 522, 182, 110, "IN IPFS");

    fill_box(rp, 0, 8, 216, WIN_W - 10, 258);
    outline_box(rp, 1, 8, 216, WIN_W - 10, 258);
    draw_text(rp, 2, 14, 232, g_status);
    draw_text(rp, 3, 14, 248, "Oeffnen entpackt in einen Zielordner; das Image selbst bleibt verschluesselt.");

    fill_box(rp, 0, 8, 266, WIN_W - 10, WIN_H - 12);
    outline_box(rp, 1, 8, 266, WIN_W - 10, WIN_H - 12);
    {
        int y = 282;
        for (i = 0; g_log[i] && y < WIN_H - 18; ++i) {
            char line[160];
            int n = 0;
            while (g_log[i] && g_log[i] != '\n' && n < (int)sizeof(line) - 1)
                line[n++] = g_log[i++];
            line[n] = 0;
            draw_text(rp, 3, 14, y, line);
            y += 11;
        }
    }

    if (g_confirm) {
        WORD bx = 100, by = 150, bw = 440, bh = 140;
        fill_box(rp, 0, bx, by, bx + bw, by + bh);
        outline_box(rp, 2, bx, by, bx + bw, by + bh);
        draw_text(rp, 2, bx + 14, by + 20, "VERSCHLUESSELTES IMAGE IN IPFS?");
        draw_text(rp, 3, bx + 14, by + 40,
                  "Das Image wird oeffentlich abrufbar. Der Inhalt bleibt");
        draw_text(rp, 3, bx + 14, by + 54,
                  "verschluesselt, aber das Image selbst ist weltweit kopierbar.");
        fill_box(rp, 2, bx + 16, by + bh - 36, bx + 220, by + bh - 12);
        draw_text(rp, 1, bx + 30, by + bh - 22, "JA, IN IPFS SICHERN");
        fill_box(rp, 3, bx + 236, by + bh - 36, bx + 340, by + bh - 12);
        draw_text(rp, 1, bx + 260, by + bh - 22, "ABBRECHEN");
    }
}

static int file_request(struct Window *w, int save, int drawer, const char *title,
                        const char *initial, char *dest, unsigned long cap)
{
    struct FileRequester *fr;
    if (!AslBase) return 0;
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window, w,
        ASLFR_TitleText, (ULONG)title,
        ASLFR_InitialDrawer, (ULONG)"IPFS:",
        ASLFR_InitialFile, (ULONG)initial,
        ASLFR_DoSaveMode, save ? TRUE : FALSE,
        ASLFR_DrawersOnly, drawer ? TRUE : FALSE,
        TAG_DONE);
    if (!fr) return 0;
    if (AslRequestTags(fr, TAG_DONE)) {
        const char *d = (const char *)fr->fr_Drawer;
        const char *f = (const char *)fr->fr_File;
        int dl;
        if (!d) d = "IPFS:";
        dl = (int)strlen(d);
        if (drawer || !f || !*f)
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

static void do_create(void)
{
    char err[200];
    if (!g_user[0] || !g_pass[0] || !g_source[0] || !g_image[0]) {
        snprintf(g_status, sizeof(g_status), "Benutzer, Passwort, Quelle und Image sind Pflicht.");
        return;
    }
    snprintf(g_status, sizeof(g_status), "Erstelle verschluesseltes Image ...");
    draw();
    if (qrx_volimage_create(g_image, g_source, g_user, g_pass, err, sizeof(err))) {
        char line[420];
        snprintf(line, sizeof(line), "Image erstellt: %s", g_image);
        append_log(line);
        snprintf(g_status, sizeof(g_status), "Fertig: %s", g_image);
    } else {
        append_log(err);
        snprintf(g_status, sizeof(g_status), "Fehler: %s", err);
    }
}

static void do_open(void)
{
    char dest[300], err[200];
    if (!g_image[0] || !g_user[0] || !g_pass[0]) {
        snprintf(g_status, sizeof(g_status), "Image, Benutzer und Passwort sind Pflicht.");
        return;
    }
    if (!file_request(g_win, 0, 1, "Zielordner zum Entpacken", "", dest, sizeof(dest))) return;
    snprintf(g_status, sizeof(g_status), "Entpacke nach %s ...", dest);
    draw();
    if (qrx_volimage_extract(g_image, dest, g_user, g_pass, err, sizeof(err))) {
        char line[420];
        snprintf(line, sizeof(line), "Entpackt nach: %s", dest);
        append_log(line);
        snprintf(g_status, sizeof(g_status), "Volume geoeffnet.");
    } else {
        append_log(err);
        snprintf(g_status, sizeof(g_status), "Fehler: %s", err);
    }
}

static void do_verify(void)
{
    char err[200];
    if (qrx_volimage_verify(g_image, g_user, g_pass, err, sizeof(err))) {
        append_log("HMAC ok - Image unveraendert und Passwort richtig.");
        snprintf(g_status, sizeof(g_status), "Image OK.");
    } else {
        append_log(err);
        snprintf(g_status, sizeof(g_status), "Fehler: %s", err);
    }
}

static void do_publish(void)
{
    char cid[128], err[200], line[420];
    if (qrx_ipfs_ensure_volume() && qrx_ipfs_add(g_image, 0, 1, cid, sizeof(cid))) {
        qrx_ipfs_revision_append(QRX_IPFS_REVISION_LOG, g_image, cid, 0);
        snprintf(line, sizeof(line), "In IPFS: %s", cid);
        append_log(line);
        snprintf(g_status, sizeof(g_status), "Image in IPFS: %.70s", cid);
    } else {
        snprintf(err, sizeof(err), "%s", qrx_ipfs_error());
        append_log(err);
        snprintf(g_status, sizeof(g_status), "IPFS-Fehler: %s", err);
    }
}

static void click(WORD x, WORD y, int *run)
{
    if (g_confirm) {
        if (y >= 254 && y <= 278 && x >= 116 && x <= 320) {
            g_confirm = 0;
            do_publish();
            draw();
        } else if (y >= 254 && y <= 278 && x >= 336 && x <= 440) {
            g_confirm = 0;
            snprintf(g_status, sizeof(g_status), "IPFS-Sicherung abgebrochen.");
            draw();
        }
        return;
    }

    if (x >= 8 && x <= WIN_W - 10) {
        if (y >= 48 && y <= 70) { g_field = 0; draw(); return; }
        if (y >= 76 && y <= 98) { g_field = 1; draw(); return; }
        if (y >= 104 && y <= 126) { g_field = 2; draw(); return; }
        if (y >= 132 && y <= 154) { g_field = 3; draw(); return; }
    }

    if (y >= 182 && y <= 206) {
        if (x >= 8 && x <= 100) {
            file_request(g_win, 0, 1, "Quelle (Drawer) waehlen", "", g_source, sizeof(g_source));
        } else if (x >= 106 && x <= 198) {
            file_request(g_win, 1, 0, "Image speichern", "volume.qrxvol", g_image, sizeof(g_image));
        } else if (x >= 204 && x <= 314) {
            do_create();
        } else if (x >= 320 && x <= 420) {
            do_open();
        } else if (x >= 426 && x <= 516) {
            do_verify();
        } else if (x >= 522 && x <= 632) {
            g_confirm = 1;
        }
        draw();
    }
    (void)run;
}

static void key(UWORD code)
{
    char *text = g_field == 0 ? g_user : g_field == 1 ? g_pass :
                 g_field == 2 ? g_source : g_image;
    unsigned long cap = g_field == 0 ? sizeof(g_user) :
                        g_field == 1 ? sizeof(g_pass) :
                        g_field == 2 ? sizeof(g_source) : sizeof(g_image);
    unsigned long len = (unsigned long)strlen(text);

    if (code == 9) {
        g_field = (g_field + 1) & 3;
    } else if (code == 8 || code == 127) {
        if (len) text[len - 1] = 0;
    } else if (code >= 32 && code <= 126 && len + 1 < cap) {
        text[len] = (char)code;
        text[len + 1] = 0;
    }
    draw();
}

int main(void)
{
    struct IntuiMessage *msg;
    int run = 1;
    char env[300];

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 0);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 0);
    if (!IntuitionBase || !GfxBase || !DOSBase) goto out;

    env[0] = 0;
    if (GetVar((CONST_STRPTR)"QRX_VOL_SOURCE", (STRPTR)env, sizeof(env) - 1, GVF_GLOBAL_ONLY) > 0)
        snprintf(g_source, sizeof(g_source), "%s", env);
    env[0] = 0;
    if (GetVar((CONST_STRPTR)"QRX_VOL_IMAGE", (STRPTR)env, sizeof(env) - 1, GVF_GLOBAL_ONLY) > 0)
        snprintf(g_image, sizeof(g_image), "%s", env);

    qrx_ipfs_ensure_volume();
    append_log("QRXVolImage " QRX_IPFS_VERSION " - Image anlegen, pruefen, entpacken, in IPFS sichern.");
    append_log(QRX_IPFS_WARNING);
    snprintf(g_status, sizeof(g_status), "Image: %s", g_image);

    g_screen = LockPubScreen(NULL);
    if (!g_screen) goto out;
    g_win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)g_screen,
        WA_Left, 40, WA_Top, 40,
        WA_Width, WIN_W, WA_Height, WIN_H,
        WA_Title, (ULONG)"QRX Volume Image",
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
            if (cls == IDCMP_CLOSEWINDOW) run = 0;
            else if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(g_win); draw(); EndRefresh(g_win, TRUE);
            } else if (cls == IDCMP_VANILLAKEY) {
                if (code == 27) run = 0;
                else key(code);
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                click(mx, my, &run);
            }
        }
    }

out:
    if (g_win) CloseWindow(g_win);
    if (g_screen) UnlockPubScreen(NULL, g_screen);
    if (AslBase) CloseLibrary(AslBase);
    if (DOSBase) CloseLibrary((struct Library *)DOSBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
