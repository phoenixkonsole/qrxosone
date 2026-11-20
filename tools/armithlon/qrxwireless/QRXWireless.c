/* QRXWireless: Wi-Fi and Bluetooth of the ARMithlon appliance (Raspberry Pi image) in QRX OS One's
 * settings. Lists networks and devices, connects with a password, pairs Bluetooth devices.
 * Everything goes through the engine's host control service (door service 11, qrx_hostctl.c),
 * which runs the image's /qrx/netctl. On a Mac (no appliance) the window says so.
 * Built for the 32-bit system (aros-gcc) and QRX OS One 64 (cc64), like QRXNetwork. */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

/* ---- the engine's host control service (11) */
static ULONG q[32] __attribute__((aligned(16)));
static char args[512] __attribute__((aligned(16)));
static char out[8192] __attribute__((aligned(16)));
static int ctl(const char *a0, const char *a1, const char *a2)
{
    const char *a[3] = { a0, a1, a2 };
    int n = 0;
    memset(args, 0, sizeof args);
    for (int i = 0; i < 3 && a[i]; i++) {
        int l = (int)strlen(a[i]);
        if (n + l + 2 > (int)sizeof args) return -1;
        memcpy(args + n, a[i], l);
        n += l + 1;
    }
    out[0] = 0;
    memset(q, 0, sizeof q);
    q[0] = 11; q[4] = (ULONG)(IPTR)args; q[5] = (ULONG)(IPTR)out; q[6] = sizeof out;
#if defined(__m68k64__)
    if (*(volatile UQUAD *)0xFFFFFFFF00000000UL != 0x5152584543363442UL) return -1;
    *(volatile UQUAD *)0xFFFFFFFF00000400UL = (UQUAD)(IPTR)q;
#else
    if (*(volatile ULONG *)0xFF000880UL != 0x51383644UL) return -1;
    *(volatile ULONG *)0xFF00088CUL = (ULONG)(IPTR)q;
#endif
    __asm__ volatile("" ::: "memory");            /* the host wrote q and out */
    return q[12] == 0xFFFFFFFFUL ? -1 : (int)q[13];
}

/* ---- list views: one line of text per entry, the first field kept for the actions */
#define MAX_ITEMS 48
typedef struct {
    struct List list;
    struct Node node[MAX_ITEMS];
    char text[MAX_ITEMS][80];
    char key[MAX_ITEMS][64];
    int count;
} Items;
static Items wifi, bt;
static void items_clear(Items *it) { NewList(&it->list); it->count = 0; }
static void items_add(Items *it, const char *key, const char *text)
{
    if (it->count >= MAX_ITEMS) return;
    int i = it->count++;
    snprintf(it->key[i], sizeof it->key[i], "%s", key);
    snprintf(it->text[i], sizeof it->text[i], "%s", text);
    it->node[i].ln_Name = (STRPTR)it->text[i];
    AddTail(&it->list, &it->node[i]);
}
/* splits a tab-separated line into at most n fields; returns the count */
static int fields(char *line, char **f, int n)
{
    int k = 0;
    while (k < n) {
        f[k++] = line;
        char *t = strchr(line, '\t');
        if (!t) break;
        *t = 0;
        line = t + 1;
    }
    return k;
}
static void lines(void (*each)(char *))
{
    char *p = out;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (*p) each(p);
        if (!e) break;
        p = e + 1;
    }
}
static void wifi_line(char *l)
{
    char *f[3], t[80];
    if (fields(l, f, 3) < 3) return;
    int dbm = atoi(f[0]), bars = dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1;
    snprintf(t, sizeof t, "[%.*s%.*s] %-30.30s %s", bars, "####", 4 - bars, "    ", f[2], strcmp(f[1], "open") ? "secured" : "open");
    items_add(&wifi, f[2], t);
}
static void bt_line(char *l)
{
    char *f[4], t[80];
    if (fields(l, f, 4) < 4) return;
    snprintf(t, sizeof t, "%-24.24s %s%s", f[3], !strcmp(f[2], "yes") ? "connected" : "", !strcmp(f[1], "yes") && strcmp(f[2], "yes") ? "paired" : "");
    items_add(&bt, f[0], t);
}

/* ---- the window */
enum { G_WIFI, G_PASS, G_SCAN, G_CONNECT, G_FORGET, G_BT, G_BTON, G_BTSCAN, G_PAIR, G_REMOVE, G_COUNT };
static struct Gadget *gads[G_COUNT];
static struct Window *win;
static LONG wifi_sel = -1, bt_sel = -1;
static int appliance = 1;

static void say(const char *s)
{
    struct RastPort *rp = win->RPort;
    SetAPen(rp, 0);
    RectFill(rp, win->BorderLeft + 4, win->Height - win->BorderBottom - 22, win->Width - win->BorderRight - 5, win->Height - win->BorderBottom - 6);
    SetAPen(rp, 1);
    Move(rp, win->BorderLeft + 12, win->Height - win->BorderBottom - 11);
    Text(rp, (STRPTR)s, strlen(s));
}
static void status(void)
{
    char s[120], eth[40] = "-", wl[60] = "-", bts[16] = "-";   /* about 70 characters fit */
    if (ctl("status", NULL, NULL) < 0) { appliance = 0; say("No ARMithlon appliance: Wi-Fi and Bluetooth are the host's."); return; }
    char *p = out;
    while (*p) {
        char *e = strchr(p, '\n'), *f[4];
        if (e) *e = 0;
        int n = fields(p, f, 4);
        if (n >= 2 && !strcmp(f[0], "eth0")) snprintf(eth, sizeof eth, "%s", n >= 3 && *f[2] ? f[2] : f[1]);
        if (n >= 2 && !strcmp(f[0], "wlan0")) snprintf(wl, sizeof wl, "%.24s %s", n >= 4 && *f[3] ? f[3] : f[1], n >= 3 ? f[2] : "");
        if (n >= 2 && !strcmp(f[0], "bt")) snprintf(bts, sizeof bts, "%s", !strcmp(f[1], "yes") ? "on" : "off");
        if (!e) break;
        p = e + 1;
    }
    snprintf(s, sizeof s, "LAN %s  Wi-Fi %s  BT %s", eth, wl, bts);
    say(s);
}
static void refresh_wifi(void)
{
    GT_SetGadgetAttrs(gads[G_WIFI], win, NULL, GTLV_Labels, ~0UL, TAG_DONE);
    items_clear(&wifi);
    if (ctl("wifi-list", NULL, NULL) >= 0) lines(wifi_line);
    GT_SetGadgetAttrs(gads[G_WIFI], win, NULL, GTLV_Labels, (IPTR)&wifi.list, TAG_DONE);
    wifi_sel = -1;
}
static void refresh_bt(void)
{
    GT_SetGadgetAttrs(gads[G_BT], win, NULL, GTLV_Labels, ~0UL, TAG_DONE);
    items_clear(&bt);
    if (ctl("bt-list", NULL, NULL) >= 0) lines(bt_line);
    GT_SetGadgetAttrs(gads[G_BT], win, NULL, GTLV_Labels, (IPTR)&bt.list, TAG_DONE);
    bt_sel = -1;
}
static const char *password(void)
{
    STRPTR s = NULL;
    GT_GetGadgetAttrs(gads[G_PASS], win, NULL, GTST_String, (IPTR)&s, TAG_DONE);
    return s ? (const char *)s : "";
}
static void result(const char *what)
{
    char s[120];
    char *e = strchr(out, '\n');
    if (e) *e = 0;
    snprintf(s, sizeof s, "%s: %s", what, *out ? out : "done");
    say(s);
}

int main(void)
{
    struct Screen *scr;
    APTR vi;
    struct Gadget *glist = NULL, *g;
    struct NewGadget ng;
    int done = 0, tick = 0, scanning = 0;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    GadToolsBase = OpenLibrary("gadtools.library", 39);
    if (!IntuitionBase || !GfxBase || !GadToolsBase) { PutStr("QRXWireless needs gadtools.library\n"); return 20; }
    items_clear(&wifi);
    items_clear(&bt);
    scr = LockPubScreen(NULL);
    vi = GetVisualInfo(scr, TAG_DONE);
    g = CreateContext(&glist);
    WORD top = scr->WBorTop + scr->Font->ta_YSize + 20, left = scr->WBorLeft + 12;
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;
    ng.ng_Flags = PLACETEXT_ABOVE;
    /* Wi-Fi */
    ng.ng_LeftEdge = left; ng.ng_TopEdge = top; ng.ng_Width = 420; ng.ng_Height = 110;
    ng.ng_GadgetText = (STRPTR)"Wi-Fi networks"; ng.ng_GadgetID = G_WIFI;
    g = gads[G_WIFI] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (IPTR)&wifi.list, GTLV_ShowSelected, 0, TAG_DONE);
    ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_LeftEdge = left + 80; ng.ng_TopEdge = top + 116; ng.ng_Width = 340; ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Password"; ng.ng_GadgetID = G_PASS;
    g = gads[G_PASS] = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 63, TAG_DONE);
    ng.ng_Flags = PLACETEXT_IN;
    ng.ng_TopEdge = top + 136; ng.ng_Width = 132; ng.ng_Height = 18;
    const char *wb[] = { "Search", "Connect", "Forget" };
    for (int i = 0; i < 3; i++) {
        ng.ng_LeftEdge = left + i * 144; ng.ng_GadgetText = (STRPTR)wb[i]; ng.ng_GadgetID = G_SCAN + i;
        g = gads[G_SCAN + i] = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    /* Bluetooth */
    WORD btop = top + 182;
    ng.ng_Flags = PLACETEXT_ABOVE;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = btop; ng.ng_Width = 420; ng.ng_Height = 90;
    ng.ng_GadgetText = (STRPTR)"Bluetooth devices"; ng.ng_GadgetID = G_BT;
    g = gads[G_BT] = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (IPTR)&bt.list, GTLV_ShowSelected, 0, TAG_DONE);
    ng.ng_Flags = PLACETEXT_IN;
    ng.ng_TopEdge = btop + 96; ng.ng_Width = 99; ng.ng_Height = 18;
    const char *bb[] = { "On / Off", "Search", "Pair", "Remove" };
    for (int i = 0; i < 4; i++) {
        ng.ng_LeftEdge = left + i * 107; ng.ng_GadgetText = (STRPTR)bb[i]; ng.ng_GadgetID = G_BTON + i;
        g = gads[G_BTON + i] = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    win = g ? OpenWindowTags(NULL, WA_Title, (IPTR)"Wi-Fi and Bluetooth - QRX OS One", WA_InnerWidth, 444,
                             WA_InnerHeight, btop + 150 - scr->WBorTop - scr->Font->ta_YSize,
                             WA_Gadgets, (IPTR)glist, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_PubScreen, (IPTR)scr,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | STRINGIDCMP | BUTTONIDCMP | IDCMP_INTUITICKS,
                             TAG_DONE) : NULL;
    UnlockPubScreen(NULL, scr);
    if (!win) { PutStr("QRXWireless: no window\n"); return 20; }
    GT_RefreshWindow(win, NULL);
    status();
    /* what wpa_supplicant already knows shows at once; the new search fills in on the ticks
     * (Intuition sends those to the active window only) */
    if (appliance) { ctl("wifi-scan", NULL, NULL); scanning = 6; refresh_wifi(); refresh_bt(); }
    while (!done) {
        struct IntuiMessage *m;
        WaitPort(win->UserPort);
        while ((m = GT_GetIMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *gad = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_CLOSEWINDOW) done = 1;
            else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); }
            else if (cls == IDCMP_INTUITICKS) {
                /* while a search runs, the lists fill in about every second */
                if (scanning && ++tick % 10 == 0) { scanning--; refresh_wifi(); refresh_bt(); }
                else if (tick++ % 30 == 0 && appliance) status();
            } else if (cls == IDCMP_GADGETUP && appliance) {
                switch (gad->GadgetID) {
                case G_WIFI: wifi_sel = code; break;
                case G_BT: bt_sel = code; break;
                case G_SCAN: ctl("wifi-scan", NULL, NULL); scanning = 6; say("Searching for Wi-Fi networks..."); break;
                case G_CONNECT:
                    if (wifi_sel < 0 || wifi_sel >= wifi.count) { say("Choose a network first."); break; }
                    ctl("wifi-connect", wifi.key[wifi_sel], password());
                    result("Connect");
                    break;
                case G_FORGET:
                    if (wifi_sel < 0 || wifi_sel >= wifi.count) { say("Choose a network first."); break; }
                    ctl("wifi-forget", wifi.key[wifi_sel], NULL); result("Forget"); break;
                case G_BTON: {
                    ctl("status", NULL, NULL);
                    int on = strstr(out, "bt\tyes") != NULL;
                    ctl("bt-power", on ? "off" : "on", NULL); result("Bluetooth"); break; }
                case G_BTSCAN: ctl("bt-scan", NULL, NULL); scanning = 12; say("Searching for Bluetooth devices (12 s)..."); break;
                case G_PAIR:
                    if (bt_sel < 0 || bt_sel >= bt.count) { say("Choose a device first."); break; }
                    ctl("bt-pair", bt.key[bt_sel], NULL); scanning = 20; result("Pairing"); break;
                case G_REMOVE:
                    if (bt_sel < 0 || bt_sel >= bt.count) { say("Choose a device first."); break; }
                    ctl("bt-remove", bt.key[bt_sel], NULL); result("Remove"); refresh_bt(); break;
                default: break;
                }
            }
        }
    }
    memset(args, 0, sizeof args);                 /* the password does not stay in memory */
    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
