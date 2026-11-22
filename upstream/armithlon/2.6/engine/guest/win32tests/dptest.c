/* DirectPlay under QRX86 over real host sockets, using the calls GTA uses.
 *   dptest         two hosts and a member inside one program
 *   dptest host    hosts a session and waits for a member from another emulator
 *   dptest join    finds that session and joins it
 * The last two exchange a checked message stream. */
#define INITGUID
#include <windows.h>
#include <dplay.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(APP_GUID, 0x51525844, 0x5054, 0x4553, 0x54, 0x51, 0x52, 0x58, 0x44, 0x50, 0x00, 0x01);
#define MESSAGES 2000
#define WAIT_MS 15000
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)

typedef struct { GUID guid; char name[64]; } provider;
static provider prov[4];
static int nprov;
static BOOL FAR PASCAL prov_cb(LPGUID g, LPSTR name, DWORD maj, DWORD min, LPVOID ctx)
{
    if (nprov < 4) { prov[nprov].guid = *g; strncpy(prov[nprov].name, name, 63); nprov++; }
    return TRUE;
}
typedef struct { GUID inst; char name[32]; DWORD maxp, cur; } sess;
static sess found[8];
static int nfound;
static BOOL FAR PASCAL sess_cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    if (flags & DPESC_TIMEDOUT) return FALSE;
    if (nfound < 8) {
        found[nfound].inst = sd->guidInstance;
        strncpy(found[nfound].name, sd->lpszSessionNameA, 31);
        found[nfound].maxp = sd->dwMaxPlayers;
        found[nfound].cur = sd->dwCurrentPlayers;
        nfound++;
    }
    return TRUE;
}
typedef struct { DPID id; char name[32]; } pl;
static pl players[8];
static int nplayers;
static BOOL FAR PASCAL player_cb(DPID id, DWORD type, LPCDPNAME name, DWORD flags, LPVOID ctx)
{
    if (nplayers < 8) { players[nplayers].id = id; strncpy(players[nplayers].name, name->lpszShortNameA, 31); nplayers++; }
    return TRUE;
}
static LPDIRECTPLAY2A make(void)
{
    LPDIRECTPLAY dp = NULL;
    LPDIRECTPLAY2A dp2 = NULL;
    HRESULT r = DirectPlayCreate(&prov[0].guid, &dp, NULL);
    CHECK(r == DP_OK && dp, "DirectPlayCreate %08lx", (unsigned long)r);
    if (!dp) return NULL;
    r = IDirectPlay_QueryInterface(dp, &IID_IDirectPlay2A, (void **)&dp2);
    CHECK(r == DP_OK && dp2, "QueryInterface %08lx", (unsigned long)r);
    IDirectPlay_Release(dp);
    return dp2;
}
static HRESULT host_open(LPDIRECTPLAY2A dp, const char *name)
{
    DPSESSIONDESC2 sd;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.guidApplication = APP_GUID;
    sd.dwMaxPlayers = 4;
    sd.lpszSessionNameA = (LPSTR)name;
    return IDirectPlay2_Open(dp, &sd, DPOPEN_CREATE);
}
static void search(LPDIRECTPLAY2A dp)
{
    DPSESSIONDESC2 sd;
    HRESULT r;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.guidApplication = APP_GUID;
    nfound = 0;
    r = IDirectPlay2_EnumSessions(dp, &sd, 0, sess_cb, NULL, 0);
    CHECK(r == DP_OK, "EnumSessions %08lx", (unsigned long)r);
}
static HRESULT join(LPDIRECTPLAY2A dp, const GUID *inst)
{
    DPSESSIONDESC2 sd;
    memset(&sd, 0, sizeof sd);
    sd.dwSize = sizeof sd;
    sd.guidInstance = *inst;
    return IDirectPlay2_Open(dp, &sd, DPOPEN_JOIN);
}
static DPID new_player(LPDIRECTPLAY2A dp, const char *name)
{
    DPNAME n;
    DPID id = 0;
    HRESULT r;
    memset(&n, 0, sizeof n);
    n.dwSize = sizeof n;
    n.lpszShortNameA = (LPSTR)name;
    r = IDirectPlay2_CreatePlayer(dp, &id, &n, NULL, NULL, 0, 0);
    CHECK(r == DP_OK && id, "CreatePlayer %s %08lx", name, (unsigned long)r);
    return id;
}
static void list_players(LPDIRECTPLAY2A dp)
{
    HRESULT r;
    nplayers = 0;
    r = IDirectPlay2_EnumPlayers(dp, NULL, player_cb, NULL, 0);
    CHECK(r == DP_OK, "EnumPlayers %08lx", (unsigned long)r);
}
static int receive(LPDIRECTPLAY2A dp, DPID *from, DPID *to, void *buf, DWORD room, DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    for (;;) {
        DWORD size = room;
        HRESULT r = IDirectPlay2_Receive(dp, from, to, DPRECEIVE_ALL, buf, &size);
        if (r == DP_OK) return (int)size;
        CHECK(r == DPERR_NOMESSAGES, "Receive %08lx", (unsigned long)r);
        if (r != DPERR_NOMESSAGES || GetTickCount() > end) return -1;
        Sleep(1);
    }
}
static void fill(unsigned char *b, int n, unsigned seq)
{
    int i;
    b[0] = (unsigned char)(seq >> 8);
    b[1] = (unsigned char)seq;
    for (i = 2; i < n; i++) b[i] = (unsigned char)(seq * 7u + (unsigned)i * 3u);
}
static int intact(const unsigned char *b, int n, unsigned seq)
{
    unsigned char want[256];
    fill(want, n, seq);
    return !memcmp(b, want, (size_t)n);
}
/* Both sides send in turn, so bytes travel through the session for real. */
static void exchange(LPDIRECTPLAY2A dp, DPID me, DPID peer, int first)
{
    unsigned char out[256], in[256];
    DWORD t0 = GetTickCount();
    DPID from, to;
    int i, wrong = 0, got = 0;
    for (i = 0; i < MESSAGES; i++) {
        int n = 16 + i % 240;
        HRESULT r;
        if (!first) {
            int s = receive(dp, &from, &to, in, sizeof in, WAIT_MS);
            if (s < 0) break;
            got++;
            if (s != n || from != peer || to != me || !intact(in, s, (unsigned)i)) wrong++;
        }
        fill(out, n, (unsigned)i);
        r = IDirectPlay2_Send(dp, me, peer, i & 1 ? DPSEND_GUARANTEED : 0, out, (DWORD)n);
        CHECK(r == DP_OK, "Send %08lx", (unsigned long)r);
        if (first) {
            int s = receive(dp, &from, &to, in, sizeof in, WAIT_MS);
            if (s < 0) break;
            got++;
            if (s != n || from != peer || to != me || !intact(in, s, (unsigned)i)) wrong++;
        }
    }
    printf("exchange: %d of %d messages back, %d wrong, %lu ms\n", got, MESSAGES, wrong, (unsigned long)(GetTickCount() - t0));
    CHECK(got == MESSAGES && !wrong, "exchange");
}
static int finish(const char *name)
{
    printf("%s %s\n", name, failures ? "FAILED" : "PASSED");
    return failures != 0;
}
static int run_host(void)
{
    LPDIRECTPLAY2A dp = make();
    HRESULT r;
    DPID me, peer = 0;
    DWORD end = GetTickCount() + 60000;
    unsigned char quit[4];
    DPID from, to;
    if (!dp) return finish("DPTEST HOST");
    r = host_open(dp, "QRX DP TEST");
    CHECK(r == DP_OK, "Open %08lx", (unsigned long)r);
    me = new_player(dp, "host");
    while (GetTickCount() < end && !peer) {
        int i;
        list_players(dp);
        for (i = 0; i < nplayers; i++) if (players[i].id != me) peer = players[i].id;
        Sleep(10);
    }
    CHECK(peer != 0, "no member came");
    if (peer) {
        printf("member %s has id %lx\n", players[nplayers - 1].name, (unsigned long)peer);
        exchange(dp, me, peer, 1);
        /* The member closes first. */
        CHECK(receive(dp, &from, &to, quit, sizeof quit, WAIT_MS) == 4, "goodbye");
    }
    IDirectPlay2_Close(dp);
    IDirectPlay2_Release(dp);
    return finish("DPTEST HOST");
}
static int run_join(void)
{
    LPDIRECTPLAY2A dp = make();
    DWORD end = GetTickCount() + 60000;
    DPID me, peer;
    int i;
    if (!dp) return finish("DPTEST JOIN");
    do search(dp); while (!nfound && GetTickCount() < end);
    CHECK(nfound == 1 && !strcmp(found[0].name, "QRX DP TEST"), "found %d sessions", nfound);
    if (nfound) {
        HRESULT r = join(dp, &found[0].inst);
        CHECK(r == DP_OK, "join %08lx", (unsigned long)r);
        me = new_player(dp, "member");
        list_players(dp);
        CHECK(nplayers == 2, "%d players", nplayers);
        peer = 0;
        for (i = 0; i < nplayers; i++) if (players[i].id != me) peer = players[i].id;
        if (peer) {
            exchange(dp, me, peer, 0);
            IDirectPlay2_Send(dp, me, peer, DPSEND_GUARANTEED, "bye", 4);
            /* The host's roster still has us until the connection ends. */
            Sleep(200);
        }
    }
    IDirectPlay2_Close(dp);
    IDirectPlay2_Release(dp);
    return finish("DPTEST JOIN");
}
static LPDIRECTPLAY2A g_member;
static DPID g_host_id, g_member_id;
static DWORD WINAPI member_thread(void *arg)
{
    (void)arg;
    exchange(g_member, g_member_id, g_host_id, 0);
    return 0;
}
static int run_alone(void)
{
    LPDIRECTPLAY2A a = make(), b = make(), c = make();
    DPID ha, mc, from, to;
    char buf[64], small[2];
    DWORD size, ms;
    HRESULT r;
    HANDLE th;
    int i, s;
    if (!a || !b || !c) return finish("DPTEST");
    size = sizeof buf;
    r = IDirectPlay2_Receive(c, &from, &to, DPRECEIVE_ALL, buf, &size);
    CHECK(r == DPERR_NOMESSAGES, "Receive with no session %08lx", (unsigned long)r);
    /* Two hosts on one computer must take different search ports. */
    r = host_open(a, "first");
    CHECK(r == DP_OK, "Open %08lx", (unsigned long)r);
    r = host_open(b, "second");
    CHECK(r == DP_OK, "second Open %08lx", (unsigned long)r);
    ha = new_player(a, "alice");
    new_player(b, "nobody");
    search(c);
    CHECK(nfound == 2, "found %d sessions", nfound);
    for (i = 0; i < nfound; i++) if (!strcmp(found[i].name, "first")) break;
    CHECK(i < nfound && found[i].maxp == 4 && found[i].cur == 1, "session data");
    if (i >= nfound) return finish("DPTEST");
    r = join(c, &found[i].inst);
    CHECK(r == DP_OK, "join %08lx", (unsigned long)r);
    mc = new_player(c, "carol");
    CHECK(mc > ha, "ids must rise");
    list_players(c);
    CHECK(nplayers == 2 && players[0].id == ha && !strcmp(players[0].name, "alice") && players[1].id == mc, "member's list");
    list_players(a);
    CHECK(nplayers == 2 && players[0].id == ha && players[1].id == mc && !strcmp(players[1].name, "carol"), "host's list");
    /* A message that does not fit stays in the queue. */
    r = IDirectPlay2_Send(a, ha, mc, DPSEND_GUARANTEED, "hello", 6);
    CHECK(r == DP_OK, "Send %08lx", (unsigned long)r);
    for (ms = GetTickCount() + 2000; GetTickCount() < ms; Sleep(1)) {
        size = sizeof small;
        r = IDirectPlay2_Receive(c, &from, &to, DPRECEIVE_ALL, small, &size);
        if (r != DPERR_NOMESSAGES) break;
    }
    CHECK(r == DPERR_BUFFERTOOSMALL && size == 6, "small buffer %08lx size %lu", (unsigned long)r, (unsigned long)size);
    s = receive(c, &from, &to, buf, sizeof buf, 2000);
    CHECK(s == 6 && !strcmp(buf, "hello") && from == ha && to == mc, "hello: %d", s);
    /* Unknown players and sizes are refused. */
    r = IDirectPlay2_Send(c, mc, 0x7777, 0, "x", 2);
    CHECK(r == DPERR_INVALIDPLAYER, "unknown target %08lx", (unsigned long)r);
    /* Message stream between a thread on each side, through real TCP. */
    g_member = c; g_host_id = ha; g_member_id = mc;
    th = CreateThread(NULL, 0, member_thread, NULL, 0, NULL);
    exchange(a, ha, mc, 1);
    WaitForSingleObject(th, WAIT_MS);
    /* A member that leaves disappears from the host's list. */
    r = IDirectPlay2_DestroyPlayer(c, mc);
    CHECK(r == DP_OK, "DestroyPlayer %08lx", (unsigned long)r);
    for (ms = GetTickCount() + 2000; GetTickCount() < ms; Sleep(1)) {
        list_players(a);
        if (nplayers == 1) break;
    }
    CHECK(nplayers == 1 && players[0].id == ha, "host's list after leaving: %d", nplayers);
    mc = new_player(c, "carol2");
    /* When the host goes, a member's next Send says so. */
    IDirectPlay2_Close(a);
    for (ms = GetTickCount() + 2000; GetTickCount() < ms; Sleep(1)) {
        r = IDirectPlay2_Send(c, mc, ha, 0, "x", 2);
        if (r != DP_OK) break;
    }
    CHECK(r == DPERR_NOCONNECTION, "after the host left %08lx", (unsigned long)r);
    IDirectPlay2_Release(a);
    IDirectPlay2_Close(b);
    IDirectPlay2_Release(b);
    IDirectPlay2_Close(c);
    IDirectPlay2_Release(c);
    return finish("DPTEST");
}
int main(int argc, char **argv)
{
    HRESULT r = DirectPlayEnumerateA(prov_cb, NULL);
    CHECK(r == DP_OK, "DirectPlayEnumerateA %08lx", (unsigned long)r);
    CHECK(nprov == 1 && strstr(prov[0].name, "Internet TCP/IP"), "providers: %d", nprov);
    printf("provider: %s\n", nprov ? prov[0].name : "none");
    if (!nprov) return finish("DPTEST");
    if (argc > 1 && !strcmp(argv[1], "host")) return run_host();
    if (argc > 1 && !strcmp(argv[1], "join")) return run_join();
    return run_alone();
}
