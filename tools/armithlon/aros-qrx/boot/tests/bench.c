/* CPU benchmark for the 68100EC64: an integer hash, a sort and a prime sieve. On AROS it reads the
 * ARMithlon board's clock and the core's instruction count; on the host (-DHOST) only the time. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef HOST
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static uint64_t instructions(void) { return 0; }
#else
#define REG64(o) (*(volatile uint64_t *)(0xFFFFFFFF00000000UL + (o)))
static uint64_t now_us(void) { return REG64(0x0300); }
static uint64_t instructions(void) { return REG64(0x0310); }
#endif

static uint64_t hash(unsigned rounds)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned r = 0; r < rounds; r++)
        for (unsigned i = 0; i < 1000; i++) { h ^= (uint64_t)i * r; h *= 1099511628211ULL; h = h << 13 | h >> 51; }
    return h;
}
static int cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t sort(unsigned n)
{
    uint64_t *v = malloc(n * sizeof *v), s = 0, x = 88172645463325252ULL;
    for (unsigned i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v[i] = x; }
    qsort(v, n, sizeof *v, cmp);
    for (unsigned i = 0; i < n; i += 97) s += v[i] >> 32;
    free(v);
    return s;
}
static unsigned sieve(unsigned n)
{
    unsigned char *c = calloc(n, 1);
    unsigned count = 0;
    for (unsigned i = 2; i < n; i++) if (!c[i]) { count++; for (unsigned j = i * 2; j < n; j += i) c[j] = 1; }
    free(c);
    return count;
}

int main(int argc, char **argv)
{
    unsigned scale = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    uint64_t t0 = now_us(), i0 = instructions();
    uint64_t h = hash(2000 * scale);
    uint64_t t1 = now_us();
    uint64_t s = sort(50000 * scale);
    uint64_t t2 = now_us();
    unsigned p = sieve(2000000 * scale);
    uint64_t t3 = now_us(), i1 = instructions();
    printf("hash %016llx %llu ms, sort %llu %llu ms, sieve %u %llu ms, total %llu ms",
           (unsigned long long)h, (unsigned long long)(t1 - t0) / 1000, (unsigned long long)s, (unsigned long long)(t2 - t1) / 1000,
           p, (unsigned long long)(t3 - t2) / 1000, (unsigned long long)(t3 - t0) / 1000);
    if (i1 > i0) printf(", %llu million instructions, %.1f million a second",
                        (unsigned long long)(i1 - i0) / 1000000, (double)(i1 - i0) / (double)(t3 - t0));
    printf("\n");
    return 0;
}
