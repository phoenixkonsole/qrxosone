/* C source for the m68k64 target, built for the Mac as well: both must give the same numbers.
 * Freestanding on purpose: no library is there yet. Each test_x returns one value. */
typedef __INT64_TYPE__ i64;
typedef __UINT64_TYPE__ u64;
typedef __INT32_TYPE__ i32;
typedef __UINT32_TYPE__ u32;
typedef __INT16_TYPE__ i16;
typedef __UINT16_TYPE__ u16;
typedef __INT8_TYPE__ i8;
typedef __UINT8_TYPE__ u8;
typedef __SIZE_TYPE__ size_t;

void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int v, size_t n) { u8 *a = d; while (n--) *a++ = (u8)v; return d; }
int memcmp(const void *x, const void *y, size_t n) { const u8 *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

i64 test_sizes(void) { return (i64)sizeof(void *) << 32 | sizeof(long) << 24 | sizeof(long long) << 16 | sizeof(int) << 8 | _Alignof(long long); }

/* data, relocated pointers, strings */
static const char *const names[] = { "alpha", "beta", "gamma", "delta" };
static int counter = 40;
i64 test_globals(void) {
    u64 h = 0;
    for (unsigned i = 0; i < 4; i++) { const char *p = names[i]; while (*p) h = h * 31 + (u8)*p++; }
    counter += 2;
    return (i64)(h ^ (u64)counter);
}
/* function pointers and a jump table */
static i64 twice(i64 x) { return 2 * x; }
static i64 neg(i64 x) { return -x; }
static i64 (*const ops[])(i64) = { twice, neg };
static int sw(int v) {
    switch (v) { case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44; case 4: return 55; case 5: return 66; case 6: return 77; default: return -1; }
}
i64 test_calls(void) {
    i64 r = 0;
    for (int i = 0; i < 16; i++) { r = ops[i & 1](r + i); r += sw(i & 7); }
    return r;
}
/* A sparse switch over format characters: 'A' to 'x' needs a 64-bit mask, which becomes a bit test */
static int fmtkind(int c) {
    switch (c) { case 'A': case 'E': case 'G': case 'X': case 'a': case 'c': case 'd': case 'e': case 'g':
    case 'i': case 'o': case 'p': case 's': case 'u': case 'x': return 1; default: return 0; }
}
i64 test_bittest(void) {
    i64 r = 0;
    for (int c = 0; c < 128; c++) r = r * 3 + fmtkind(c) + (c & 1);
    return r;
}
/* __builtin_frame_address and __builtin_return_address: the callee sees the caller's frame */
__attribute__((noinline)) static void *callers_frame(void **ret) {
    *ret = __builtin_return_address(0);
    return __builtin_frame_address(1);
}
__attribute__((noinline)) static i64 frame_check(void) {
    void *ret, *up = callers_frame(&ret);
    void *mine = __builtin_frame_address(0);
    i64 inside = (char *)ret > (char *)frame_check && (char *)ret < (char *)frame_check + 4096;
    return (up == mine) * 2 + inside;
}
i64 test_frames(void) { return frame_check() + frame_check() * 10; }
/* structs in all their ways */
struct point { i16 x; i8 c; i32 y; i64 z; };
struct small { u8 a, b; };
static struct point mk(i64 v) { struct point p = { (i16)v, (i8)(v >> 8), (i32)(v >> 16), v * 3 }; return p; }
static i64 use(struct point p, struct small s, int k) { return p.x + p.c + p.y + p.z + s.a * s.b + k; }
i64 test_structs(void) {
    struct point a[8];
    i64 r = 0;
    for (int i = 0; i < 8; i++) a[i] = mk(0x123456789AL * (i + 1));
    for (int i = 0; i < 8; i++) { struct small s = { (u8)i, (u8)(i + 1) }; r ^= use(a[i], s, i); }
    return r + (i64)sizeof(struct point) * 1000 + (i64)__builtin_offsetof(struct point, z);
}
/* varargs with every width */
#include <stdarg.h>
static i64 vsum(int n, ...) {
    va_list ap; i64 r = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        switch (i & 3) {
        case 0: r += va_arg(ap, int); break;
        case 1: r += va_arg(ap, long); break;
        case 2: r += (i64)va_arg(ap, char *); break;
        default: r += va_arg(ap, unsigned); break;
        }
    }
    va_end(ap);
    return r;
}
i64 test_varargs(void) { return vsum(8, -5, 0x100000000L, (char *)0x7000, 7u, 9, -1L, (char *)8, 0xFFFFFFFFu); }
/* sizes and signs */
i64 test_widths(void) {
    volatile i8 a = -7; volatile u8 b = 250; volatile i16 c = -3000; volatile u16 d = 60000; volatile i32 e = -100000; volatile u32 f = 4000000000u; volatile i64 g = -5000000000L; volatile u64 h = 0xF000000000000000UL;
    i64 r = a * b + c / 7 + d % 13 + e / a + (i64)f * e;
    r ^= (i64)(h >> 60) + (g >> 3) + (i64)(g / 1000) + (i64)((u64)g % 977);
    r += (a < b) + ((u64)g > h) * 2 + (g < e) * 4 + (f > 3) * 8 + ((i32)f < 0) * 16;
    return r;
}
/* a real algorithm: SHA-256 of a generated message */
static const u32 K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(u32 *h, const u8 *p) {
    u32 w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        u32 t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
static u8 msg[8192];
i64 test_sha256(void) {
    u32 h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    u64 n = 8000;
    for (u64 i = 0; i < n; i++) msg[i] = (u8)(i * 167 + (i >> 5));
    u64 total = n;
    msg[n++] = 0x80;
    while (n % 64 != 56) msg[n++] = 0;
    for (int i = 7; i >= 0; i--) msg[n++] = (u8)((total * 8) >> (i * 8));
    for (u64 i = 0; i < n; i += 64) sha_block(h, msg + i);
    return (i64)((u64)h[0] << 32 | h[7]);
}
/* recursion and sorting */
static void qsort64(u64 *a, long l, long r) {
    if (l >= r) return;
    u64 p = a[(l + r) / 2];
    long i = l, j = r;
    while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { u64 t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
    qsort64(a, l, j); qsort64(a, i, r);
}
static u64 data[4000];
i64 test_sort(void) {
    u64 x = 88172645463325252UL, r = 0;
    for (int i = 0; i < 4000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; data[i] = x; }
    qsort64(data, 0, 3999);
    for (int i = 1; i < 4000; i++) if (data[i - 1] > data[i]) return -1;
    for (int i = 0; i < 4000; i += 97) r = r * 3 + data[i];
    return (i64)r;
}
/* a bump allocator and a linked list, so pointers live in memory */
struct node { struct node *next; u64 v; };
static u8 heap[65536];
static size_t used;
static void *alloc(size_t n) { void *p = heap + used; used += (n + 15) & ~(size_t)15; return p; }
i64 test_list(void) {
    struct node *h = 0; u64 s = 0;
    used = 0;
    for (u64 i = 0; i < 1000; i++) { struct node *n = alloc(sizeof *n); n->next = h; n->v = i * i; h = n; }
    while (h) { s = s * 5 + h->v; h = h->next; }
    return (i64)s;
}
/* memcpy-shaped loops, strings, block copies */
i64 test_memory(void) {
    char a[100], b[100];
    memset(a, 'x', sizeof a); a[99] = 0;
    memcpy(b, a, sizeof a); b[50] = 0;
    struct { u64 v[9]; } s, t;
    for (int i = 0; i < 9; i++) s.v[i] = (u64)i << 40;
    t = s;
    return (i64)(strlen(a) * 1000 + strlen(b) + (u64)memcmp(a, b, 50) + t.v[8]);
}
/* byte swaps, as little-endian file systems read their fields: rol.w, ror.l and the shifts */
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static volatile u16 sw16[3] = { 0x1234, 0xFF00, 0x00A5 };
static volatile u32 sw32[2] = { 0x12345678u, 0xDEADBEEFu };
static volatile u64 sw64 = 0x0123456789ABCDEFull;
i64 test_bswap(void) {
    u8 raw[4] = { 0x34, 0x12, 0xEF, 0xBE };
    u64 r = 0;
    for (int i = 0; i < 3; i++) r = r * 65537 + __builtin_bswap16(sw16[i]);
    for (int i = 0; i < 2; i++) r = r * 31 + __builtin_bswap32(sw32[i]);
    r ^= __builtin_bswap64(sw64);
    r += (u64)le16(raw) << 16 | le16(raw + 2);
    return (i64)r;
}
/* floating point in single and double precision goes through the library calls that do not exist yet
 * or through the FPU, which this core does not have: not tested here. */
#ifdef HOST_TEST
#include <stdio.h>
#define T(n) printf(#n " %016llx\n", (unsigned long long)test_##n());
int main(void) { T(sizes) T(globals) T(calls) T(bittest) T(frames) T(structs) T(varargs) T(widths) T(sha256) T(sort) T(list) T(memory) T(bswap) return 0; }
#endif
