/*
 * NSPR on QRX OS One 64: threads, locks and condition variables, monitors, time, intervals,
 * thread-private data, files, TCP over loopback with PR_Poll, name lookup, random noise, the
 * processor count. Prints one PASS/FAIL line per part and "NSPR ALL PASS" at the end.
 */
#include <stdio.h>
#include <string.h>
#include "nspr.h"
#include "plstr.h"
#include "plhash.h"

static int failures;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) failures++; } while (0)

/* producer/consumer through a lock and a condition variable */
static PRLock *lock;
static PRCondVar *cv;
static int queue[16], qn, produced_sum, consumed_sum, done_producers;
static void producer(void *arg)
{
    int base = (int)(PRUptrdiff)arg;
    for (int i = 1; i <= 100; i++) {
        PR_Lock(lock);
        while (qn == 16) PR_WaitCondVar(cv, PR_INTERVAL_NO_TIMEOUT);
        queue[qn++] = base + i;
        produced_sum += base + i;
        PR_NotifyAllCondVar(cv);
        PR_Unlock(lock);
    }
    PR_Lock(lock); done_producers++; PR_NotifyAllCondVar(cv); PR_Unlock(lock);
}
static void consumer(void *arg)
{
    (void)arg;
    for (;;) {
        PR_Lock(lock);
        while (qn == 0 && done_producers < 3) PR_WaitCondVar(cv, PR_INTERVAL_NO_TIMEOUT);
        if (qn == 0) { PR_Unlock(lock); return; }
        consumed_sum += queue[--qn];
        PR_NotifyAllCondVar(cv);
        PR_Unlock(lock);
    }
}

static PRMonitor *mon;
static int mon_count;
static void mon_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 500; i++) { PR_EnterMonitor(mon); PR_EnterMonitor(mon); mon_count++; PR_ExitMonitor(mon); PR_ExitMonitor(mon); }
}

static PRUintn tpd_index;
static int tpd_freed;
static void tpd_dtor(void *p) { (void)p; tpd_freed++; }
static int tpd_ok = 1;
static void tpd_worker(void *arg)
{
    PR_SetThreadPrivate(tpd_index, arg);
    PR_Sleep(PR_MillisecondsToInterval(20));
    if (PR_GetThreadPrivate(tpd_index) != arg) tpd_ok = 0;
}

/* a TCP echo server thread on 127.0.0.1 */
static PRFileDesc *listener;
static void echo_server(void *arg)
{
    (void)arg;
    PRNetAddr peer;
    PRFileDesc *c = PR_Accept(listener, &peer, PR_SecondsToInterval(20));
    if (!c) return;
    char buf[256];
    PRInt32 n;
    while ((n = PR_Recv(c, buf, sizeof buf, 0, PR_SecondsToInterval(20))) > 0)
        PR_Send(c, buf, n, 0, PR_SecondsToInterval(20));
    PR_Close(c);
}

int main(void)
{
    PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
    printf("NSPR %s on %d processor(s)\n", PR_GetVersion(), (int)PR_GetNumberOfProcessors());

    char sys[64] = "", arch[64] = "";
    PR_GetSystemInfo(PR_SI_SYSNAME, sys, sizeof sys);
    PR_GetSystemInfo(PR_SI_ARCHITECTURE, arch, sizeof arch);
    CHECK("system info", !strcmp(sys, "AROS") && !strcmp(arch, "m68k64"));

    lock = PR_NewLock(); cv = PR_NewCondVar(lock);
    PRThread *t[6];
    for (int i = 0; i < 3; i++) t[i] = PR_CreateThread(PR_USER_THREAD, producer, (void *)(PRUptrdiff)(i * 1000), PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
    for (int i = 3; i < 5; i++) t[i] = PR_CreateThread(PR_USER_THREAD, consumer, NULL, PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
    int created = 1;
    for (int i = 0; i < 5; i++) { if (!t[i]) created = 0; else PR_JoinThread(t[i]); }
    CHECK("threads, lock, condvar", created && produced_sum == consumed_sum && produced_sum == 3 * 5050 + 100 * 3000);

    mon = PR_NewMonitor();
    for (int i = 0; i < 4; i++) t[i] = PR_CreateThread(PR_USER_THREAD, mon_worker, NULL, PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
    for (int i = 0; i < 4; i++) PR_JoinThread(t[i]);
    CHECK("reentrant monitor", mon_count == 2000);

    PR_NewThreadPrivateIndex(&tpd_index, tpd_dtor);
    for (int i = 0; i < 3; i++) t[i] = PR_CreateThread(PR_USER_THREAD, tpd_worker, (void *)(PRUptrdiff)(i + 1), PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
    for (int i = 0; i < 3; i++) PR_JoinThread(t[i]);
    CHECK("thread-private data", tpd_ok && tpd_freed == 3);

    PRIntervalTime a = PR_IntervalNow();
    PRTime t0 = PR_Now();
    PR_Sleep(PR_MillisecondsToInterval(200));
    PRUint32 ms = PR_IntervalToMilliseconds(PR_IntervalNow() - a);
    PRTime dt = PR_Now() - t0;
    CHECK("sleep and intervals", ms >= 180 && ms < 2000 && dt >= 180000 && dt < 2000000);

    PRExplodedTime et;
    PR_ExplodeTime(PR_Now(), PR_GMTParameters, &et);
    char tbuf[64];
    PR_FormatTimeUSEnglish(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &et);
    printf("now (UTC): %s\n", tbuf);
    PRTime parsed;
    CHECK("time parse/explode", et.tm_year >= 2025 && PR_ParseTimeString("Tue, 24 Nov 2026 12:34:56 GMT", PR_TRUE, &parsed) == PR_SUCCESS &&
          parsed == (PRTime)1795523696 * PR_USEC_PER_SEC);

    PRFileDesc *f = PR_Open("T:nspr-test.txt", PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE, 0644);
    const char text[] = "NSPR auf QRX OS One 64\n";
    int wrote = f && PR_Write(f, text, sizeof text - 1) == (PRInt32)(sizeof text - 1);
    if (f) PR_Close(f);
    char rb[64] = "";
    f = PR_Open("T:nspr-test.txt", PR_RDONLY, 0);
    PRInt32 got = f ? PR_Read(f, rb, sizeof rb - 1) : -1;
    PRFileInfo64 fi;
    int st = f && PR_GetOpenFileInfo64(f, &fi) == PR_SUCCESS && fi.size == (PRInt64)(sizeof text - 1);
    if (f) PR_Close(f);
    CHECK("file write/read/info", wrote && got == (PRInt32)(sizeof text - 1) && !memcmp(rb, text, got) && st);
    CHECK("file delete", PR_Delete("T:nspr-test.txt") == PR_SUCCESS && PR_Access("T:nspr-test.txt", PR_ACCESS_EXISTS) != PR_SUCCESS);

    PLHashTable *h = PL_NewHashTable(16, PL_HashString, PL_CompareStrings, PL_CompareValues, NULL, NULL);
    PL_HashTableAdd(h, "eins", (void *)1); PL_HashTableAdd(h, "zwei", (void *)2);
    CHECK("plds hash, plc strings", PL_HashTableLookup(h, "zwei") == (void *)2 && PL_strcasecmp("QRX", "qrx") == 0);
    PL_HashTableDestroy(h);

    /* TCP loopback with PR_Poll */
    listener = PR_NewTCPSocket();
    PRNetAddr addr;
    PR_InitializeNetAddr(PR_IpAddrLoopback, 0, &addr);
    int net_ok = listener && PR_Bind(listener, &addr) == PR_SUCCESS && PR_Listen(listener, 4) == PR_SUCCESS &&
                 PR_GetSockName(listener, &addr) == PR_SUCCESS;
    printf("listening on port %d\n", net_ok ? PR_ntohs(addr.inet.port) : -1);
    PRThread *srv = net_ok ? PR_CreateThread(PR_USER_THREAD, echo_server, NULL, PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0) : NULL;
    PRFileDesc *c = net_ok ? PR_NewTCPSocket() : NULL;
    int conn = c && PR_Connect(c, &addr, PR_SecondsToInterval(20)) == PR_SUCCESS;
    const char msg[] = "Hallo NSPR-Netz";
    int sent = conn && PR_Send(c, msg, sizeof msg, 0, PR_SecondsToInterval(20)) == (PRInt32)sizeof msg;
    PRPollDesc pd = { c, PR_POLL_READ, 0 };
    int polled = sent && PR_Poll(&pd, 1, PR_SecondsToInterval(20)) == 1 && (pd.out_flags & PR_POLL_READ);
    char eb[64] = "";
    PRInt32 er = polled ? PR_Recv(c, eb, sizeof eb, 0, PR_SecondsToInterval(20)) : -1;
    if (c) PR_Close(c);
    if (srv) PR_JoinThread(srv);
    if (listener) PR_Close(listener);
    CHECK("tcp loopback, poll", er == (PRInt32)sizeof msg && !strcmp(eb, msg));

    /* IPv6: text form, TCP echo over [::1], getaddrinfo with both families */
    PRNetAddr a6, b6;
    char s6[64] = "";
    int conv = PR_StringToNetAddr("2001:db8::1:0:0:42", &a6) == PR_SUCCESS && a6.raw.family == PR_AF_INET6 &&
               PR_NetAddrToString(&a6, s6, sizeof s6) == PR_SUCCESS;
    printf("2001:db8::1:0:0:42 -> %s\n", s6);
    CHECK("ipv6 text form", conv && !strcmp(s6, "2001:db8::1:0:0:42") &&
          PR_StringToNetAddr("::ffff:10.1.2.3", &b6) == PR_SUCCESS && PR_IsNetAddrType(&b6, PR_IpAddrV4Mapped));
    listener = PR_OpenTCPSocket(PR_AF_INET6);
    PR_SetNetAddr(PR_IpAddrLoopback, PR_AF_INET6, 0, &addr);
    net_ok = listener && PR_Bind(listener, &addr) == PR_SUCCESS && PR_Listen(listener, 4) == PR_SUCCESS &&
             PR_GetSockName(listener, &addr) == PR_SUCCESS && addr.raw.family == PR_AF_INET6;
    char ls[64] = "";
    if (net_ok) PR_NetAddrToString(&addr, ls, sizeof ls);
    printf("listening on [%s]:%d\n", ls, net_ok ? PR_ntohs(addr.ipv6.port) : -1);
    srv = net_ok ? PR_CreateThread(PR_USER_THREAD, echo_server, NULL, PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0) : NULL;
    c = net_ok ? PR_OpenTCPSocket(PR_AF_INET6) : NULL;
    conn = c && PR_Connect(c, &addr, PR_SecondsToInterval(20)) == PR_SUCCESS;
    const char msg6[] = "Hallo IPv6";
    sent = conn && PR_Send(c, msg6, sizeof msg6, 0, PR_SecondsToInterval(20)) == (PRInt32)sizeof msg6;
    memset(eb, 0, sizeof eb);
    er = sent ? PR_Recv(c, eb, sizeof eb, 0, PR_SecondsToInterval(20)) : -1;
    PRNetAddr peer6;
    int peer_ok = conn && PR_GetPeerName(c, &peer6) == PR_SUCCESS && peer6.raw.family == PR_AF_INET6 &&
                  PR_IsNetAddrType(&peer6, PR_IpAddrLoopback);
    if (c) PR_Close(c);
    if (srv) PR_JoinThread(srv);
    if (listener) PR_Close(listener);
    /* a host without IPv6 runs the socket as IPv4 (qrx_hostnet.h): then ::1 reads back v4-mapped */
    CHECK("tcp over [::1]", (!strcmp(ls, "::1") || !strcmp(ls, "::ffff:127.0.0.1")) && er == (PRInt32)sizeof msg6 && !strcmp(eb, msg6) && peer_ok);

    PRAddrInfo *ai = PR_GetAddrInfoByName("localhost", PR_AF_UNSPEC, PR_AI_ADDRCONFIG);
    int have4 = 0, have6 = 0;
    void *it = NULL;
    PRNetAddr ea;
    while (ai && (it = PR_EnumerateAddrInfo(it, ai, 0, &ea)) != NULL) {
        if (ea.raw.family == PR_AF_INET) have4 = 1;
        if (ea.raw.family == PR_AF_INET6) have6 = 1;
    }
    if (ai) PR_FreeAddrInfo(ai);
    printf("getaddrinfo(localhost): IPv4 %d, IPv6 %d\n", have4, have6);
    CHECK("getaddrinfo both families", have4 && have6);

    char hb[PR_NETDB_BUF_SIZE];
    PRHostEnt he;
    PRNetAddr ha;
    int dns = PR_GetHostByName("localhost", hb, sizeof hb, &he) == PR_SUCCESS && PR_EnumerateHostEnt(0, &he, 0, &ha) > 0;
    char as[64] = "";
    if (dns) PR_NetAddrToString(&ha, as, sizeof as);
    printf("localhost -> %s\n", as);
    CHECK("name lookup", dns && !strcmp(as, "127.0.0.1"));

    unsigned char noise[32];
    PRSize nn = PR_GetRandomNoise(noise, sizeof noise);
    CHECK("random noise", nn > 0);

    printf(failures ? "NSPR %d FAILED\n" : "NSPR ALL PASS\n", failures);
    PR_Cleanup();
    return failures != 0;
}
