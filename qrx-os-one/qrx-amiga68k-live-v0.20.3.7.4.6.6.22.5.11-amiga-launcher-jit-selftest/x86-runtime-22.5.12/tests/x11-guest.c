/* Freestanding actual ELF32/i386 X11 client. No host X libraries or libc. */
typedef unsigned int u32;
static int call3(int nr, u32 a, u32 b, u32 c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory", "cc");
    return r;
}
static int call5(int nr, u32 a, u32 b, u32 c, u32 d, u32 e) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory", "cc");
    return r;
}
static void exit_now(int n) {
    call3(1, n, 0, 0);
    for (;;)
        ;
}
static void check(int condition, int code) {
    if (!condition)
        exit_now(code);
}
static void w16(unsigned char *b, unsigned v) {
    b[0] = v;
    b[1] = v >> 8;
}
static void w32(unsigned char *b, unsigned v) {
    w16(b, v);
    w16(b + 2, v >> 16);
}
static unsigned char response[512], requests[160];
static void run(void) {
    unsigned args[4] = {1, 1, 0, 0};
    unsigned char addr[20] = {1,   0,   '/', 't', 'm', 'p', '/', '.', 'X', '1',
                              '1', '-', 'u', 'n', 'i', 'x', '/', 'X', '0', 0};
    unsigned char hello[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int fd, n;
    unsigned char *p = requests;
    unsigned pf[2];
    fd = call3(102, 1, (u32)args, 0);
    check(fd >= 3, 10);
    args[0] = fd;
    args[1] = (u32)addr;
    args[2] = 20;
    check(call3(102, 3, (u32)args, 0) == 0, 11);
    /* Split handshake and fragmented response exercise stream framing. */
    check(call3(4, fd, (u32)hello, 5) == 5, 12);
    check(call3(4, fd, (u32)(hello + 5), 7) == 7, 13);
    check(call3(3, fd, (u32)response, 8) == 8 && response[0] == 1, 14);
    check(call3(3, fd, (u32)response, 120) == 120, 15);
    /* Create top-level 64x48, white background, ExposureMask. */
    p[0] = 1;
    p[1] = 24;
    w16(p + 2, 10);
    w32(p + 4, 0x200001);
    w32(p + 8, 1);
    w16(p + 16, 64);
    w16(p + 18, 48);
    w16(p + 22, 1);
    w32(p + 24, 3);
    w32(p + 28, (1 << 1) | (1 << 11));
    w32(p + 32, 0xffffff);
    w32(p + 36, 1 << 15);
    p += 40;
    p[0] = 8;
    w16(p + 2, 2);
    w32(p + 4, 0x200001);
    p += 8;
    p[0] = 55;
    w16(p + 2, 5);
    w32(p + 4, 0x200002);
    w32(p + 8, 0x200001);
    w32(p + 12, 4);
    w32(p + 16, 0x12ab34);
    p += 20;
    p[0] = 70;
    w16(p + 2, 5);
    w32(p + 4, 0x200001);
    w32(p + 8, 0x200002);
    w16(p + 12, 4);
    w16(p + 14, 6);
    w16(p + 16, 20);
    w16(p + 18, 10);
    p += 20;
    p[0] = 43;
    w16(p + 2, 1);
    p += 4;
    n = p - requests;
    {
        unsigned iov[4] = {(u32)requests, 7, (u32)(requests + 7), (unsigned)n - 7};
        check(call3(146, fd, (u32)iov, 2) == n, 16);
    }
    {
        unsigned available = 0;
        check(call3(54, fd, 0x541b, (u32)&available) == 0 && available == 64, 26);
    }
    {
        unsigned rd = 1u << fd, tv[2] = {0, 0};
        check(call5(142, fd + 1, (u32)&rd, 0, 0, (u32)tv) == 1 && rd == (1u << fd), 27);
    }
    pf[0] = fd;
    pf[1] = 1;
    check(call3(168, (u32)pf, 1, 0) == 1 && (pf[1] >> 16) == 1, 17);
    {
        unsigned iov[2] = {(u32)response, sizeof(response)};
        unsigned msg[7] = {0, 0, (u32)iov, 1, 0, 0, 0};
        n = call3(372, fd, (u32)msg, 0);
    }
    check(n == 64, 18);
    check(response[0] == 12 && response[32] == 1 && response[34] == 5, 19);
    {
        unsigned rd = 1u << fd, tv[2] = {0, 0};
        check(call5(142, fd + 1, (u32)&rd, 0, 0, (u32)tv) == 0 && rd == 0, 28);
    }
    pf[1] = 1;
    check(call3(168, (u32)pf, 1, 0) == 0 && (pf[1] >> 16) == 0, 20);
    check(call3(55, fd, 4, 04000) == 0, 21);
    check(call3(3, fd, (u32)response, 32) == -11, 22);
    /* BadDrawable request produces an error, never a false success. */
    for (n = 0; n < 20; n++)
        requests[n] = 0;
    requests[0] = 70;
    w16(requests + 2, 3);
    w32(requests + 4, 0x200099);
    w32(requests + 8, 0x200002);
    check(call3(4, fd, (u32)requests, 12) == 12, 23);
    check(call3(3, fd, (u32)response, 32) == 32 && response[0] == 0 && response[1] == 9, 24);
#ifdef QRX_X11_HOLD
    {
        unsigned delay[2] = {5, 0};
        int k;
        for (k = 0; k < 4; k++)
            call3(162, (u32)delay, 0, 0);
    }
#endif
    check(call3(6, fd, 0, 0) == 0, 25);
    {
        static const char msg[] =
            "PASS i386 X11 client: socket, window, fill, exposure, poll, errors\n";
        call3(4, 1, (u32)msg, sizeof(msg) - 1);
    }
    exit_now(0);
}
void _start(void) { run(); }
