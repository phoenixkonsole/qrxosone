/* QRXNetwork: the network settings of QRX OS One. Direct, Tor, WireGuard or offline.
 * With Tor or WireGuard every connection of every program goes through the tunnel, and nothing
 * goes out while the tunnel is down: the engine has no other way out (qrx_netpolicy.c, qrxnetd).
 * The settings are the Mac's (~/Library/Application Support/QRX OS One/net.cfg); this sends them
 * with the network service's QRX_NET_CONFIG and shows QRX_NET_STATUS.
 * Built for QRX OS One 64 (cc64) and the 32-bit system (aros-gcc). */
#include <exec/types.h>
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

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

/* ---- the engine's network service (service 4): EC64 board HOST_CALL or the ARMithlon door */
static ULONG q[32] __attribute__((aligned(16)));
static int net_call(ULONG op, ULONG a1, ULONG a2)
{
    memset(q, 0, sizeof q);
    q[0] = 4; q[3] = op; q[5] = a1; q[6] = a2;     /* q[4..] are a[0..] */
#if defined(__m68k64__)
    if (*(volatile UQUAD *)0xFFFFFFFF00000000UL != 0x5152584543363442UL) return -1;
    *(volatile UQUAD *)0xFFFFFFFF00000400UL = (UQUAD)(IPTR)q;
#else
    {
        ULONG magic = *(volatile ULONG *)0xFF000880UL;
        if (magic != 0x51383644UL) return -1;
        *(volatile ULONG *)0xFF00088CUL = (ULONG)(IPTR)q;
    }
#endif
    __asm__ volatile("" ::: "memory");            /* the host wrote q */
    return q[12] == 0xFFFFFFFFUL ? -1 : 0;
}
#define QRX_NET_CONFIG 21
#define QRX_NET_STATUS 22

/* ---- the form */
enum { G_MODE, G_KEY, G_ADDR, G_DNS, G_PEER, G_PSK, G_ENDPOINT, G_KEEPALIVE, G_SAVE, G_COUNT };
static struct Gadget *gads[G_COUNT];
static const char *mode_labels[] = { "Direct", "Tor (all traffic)", "WireGuard VPN", "Offline", NULL };
static const char *mode_keys[] = { "direct", "tor", "wireguard", "offline" };
static LONG mode_now;
static const char *field_names[] = { NULL, "Private key", "Address", "DNS", "Server key", "Preshared key", "Endpoint IP:port", "Keepalive s" };
static const char *cfg_keys[] = { NULL, "private_key", "address", "dns", "peer_public_key", "preshared_key", "endpoint", "keepalive" };
static char values[G_COUNT][96];

static const char *string_of(int g)
{
    STRPTR s = NULL;
    GT_GetGadgetAttrs(gads[g], NULL, NULL, GTST_String, (IPTR)&s, TAG_DONE);
    return s ? (const char *)s : "";
}
static void status(struct Window *w)
{
    char s[96];
    const char *m[] = { "direct", "Tor", "WireGuard", "offline" };
    if (net_call(QRX_NET_STATUS, 0, 0) < 0) snprintf(s, sizeof s, "No ARMithlon network service");
    else snprintf(s, sizeof s, "Now: %-9s %s", q[14] < 4 ? m[q[14]] : "?",
                  q[14] == 3 ? "- no network" : q[15] ? "- connected" : "- waiting for the tunnel, no traffic leaves");
    SetAPen(w->RPort, 0);
    RectFill(w->RPort, w->BorderLeft + 4, w->Height - w->BorderBottom - 26, w->Width - w->BorderRight - 5, w->Height - w->BorderBottom - 8);
    SetAPen(w->RPort, 1);
    Move(w->RPort, w->BorderLeft + 12, w->Height - w->BorderBottom - 13);
    Text(w->RPort, (STRPTR)s, strlen(s));
}
static void save(struct Window *w)
{
    static char text[1024];
    int n = snprintf(text, sizeof text, "mode = %s\n", mode_keys[mode_now]);
    for (int g = G_KEY; g <= G_KEEPALIVE; g++) {
        const char *v = string_of(g);
        if (*v && n < (int)sizeof text - 120) n += snprintf(text + n, sizeof text - n, "%s = %s\n", cfg_keys[g], v);
    }
    if (net_call(QRX_NET_CONFIG, (ULONG)(IPTR)text, (ULONG)n) < 0 && q[13] == 10013) {
        /* the engine takes the network settings only from an administrator */
        const char *s = "Only an administrator can change the network.";
        SetAPen(w->RPort, 0);
        RectFill(w->RPort, w->BorderLeft + 4, w->Height - w->BorderBottom - 26, w->Width - w->BorderRight - 5, w->Height - w->BorderBottom - 8);
        SetAPen(w->RPort, 1);
        Move(w->RPort, w->BorderLeft + 12, w->Height - w->BorderBottom - 13);
        Text(w->RPort, (STRPTR)s, strlen(s));
        memset(text, 0, sizeof text);
        return;
    }
    memset(text, 0, sizeof text);
    status(w);
}

int main(void)
{
    struct Screen *scr;
    APTR vi;
    struct Gadget *glist = NULL, *g;
    struct NewGadget ng;
    struct Window *win;
    int done = 0, tick = 0;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    GadToolsBase = OpenLibrary("gadtools.library", 39);
    if (!IntuitionBase || !GfxBase || !GadToolsBase) { PutStr("QRXNetwork needs gadtools.library\n"); return 20; }
    if (net_call(QRX_NET_STATUS, 0, 0) == 0) mode_now = q[14] < 4 ? (LONG)q[14] : 0;
    scr = LockPubScreen(NULL);
    vi = GetVisualInfo(scr, TAG_DONE);
    g = CreateContext(&glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = scr->WBorLeft + 150;
    ng.ng_TopEdge = scr->WBorTop + scr->Font->ta_YSize + 14;
    ng.ng_Width = 300; ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Network";
    ng.ng_GadgetID = G_MODE;
    g = gads[G_MODE] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (IPTR)mode_labels, GTCY_Active, mode_now, TAG_DONE);
    ng.ng_TopEdge += 30;
    for (int f = G_KEY; f <= G_KEEPALIVE; f++) {
        ng.ng_GadgetText = (STRPTR)field_names[f];
        ng.ng_GadgetID = f;
        g = gads[f] = CreateGadget(STRING_KIND, g, &ng, GTST_String, (IPTR)values[f], GTST_MaxChars, 90, TAG_DONE);
        ng.ng_TopEdge += 18;
    }
    ng.ng_TopEdge += 8;
    ng.ng_Width = 120; ng.ng_Height = 18;
    ng.ng_GadgetText = (STRPTR)"Save and apply";
    ng.ng_GadgetID = G_SAVE;
    g = gads[G_SAVE] = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    win = g ? OpenWindowTags(NULL, WA_Title, (IPTR)"Network - QRX OS One", WA_InnerWidth, 480, WA_InnerHeight, ng.ng_TopEdge + 50 - scr->WBorTop - scr->Font->ta_YSize,
                             WA_Gadgets, (IPTR)glist, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                             WA_PubScreen, (IPTR)scr,
                             WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW | CYCLEIDCMP | STRINGIDCMP | IDCMP_INTUITICKS,
                             TAG_DONE) : NULL;
    UnlockPubScreen(NULL, scr);
    if (!win) { PutStr("QRXNetwork: no window\n"); return 20; }
    GT_RefreshWindow(win, NULL);
    {
        const char *note = "Tor and the VPN carry ALL traffic; when they are down nothing goes out.";
        SetAPen(win->RPort, 1);
        Move(win->RPort, win->BorderLeft + 12, win->BorderTop + 8 + win->RPort->TxBaseline);
        Text(win->RPort, (STRPTR)note, strlen(note));
    }
    status(win);
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
            else if (cls == IDCMP_GADGETUP && gad->GadgetID == G_MODE) mode_now = code;
            else if (cls == IDCMP_GADGETUP && gad->GadgetID == G_SAVE) save(win);
            else if (cls == IDCMP_INTUITICKS && ++tick % 10 == 0) status(win);
        }
    }
    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
