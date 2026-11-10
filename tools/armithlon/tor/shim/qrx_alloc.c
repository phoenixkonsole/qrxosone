/* malloc for the tor port. Under tor's worker threads the program died with AROS's "Memory
 * header not located" in libcrt's realloc, with the worker threads allocating at the same time.
 * This one sits on one Exec pool with its own semaphore, shared by all tasks of the program.
 *
 * The SDK's pointer bridges wrap malloc, calloc, realloc, strdup and strndup and expect d0,
 * as from an AROS library. These definitions take the place of libcrt's and return unsigned
 * long for that reason; the assembler names keep the C names apart from the headers' ones. */
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <proto/exec.h>
#include <exec/memory.h>

#define MAGIC 0x51524841ul
struct head { unsigned long magic; void *base; unsigned long total; unsigned long size; };
static APTR pool;
extern void __real_free(void *);
extern int atexit(void (*)(void));

static void pool_gone(void) { if (pool) { DeletePool(pool); pool = NULL; } }
static int pool_ready(void)
{
    if (pool) return 1;
    Forbid();
    if (!pool) pool = CreatePool(MEMF_ANY | MEMF_SEM_PROTECTED, 256 * 1024, 64 * 1024);
    Permit();
    return pool != NULL;
}
static void *alloc_aligned(size_t n, size_t align)
{
    unsigned long total, p;
    struct head *h;
    if (!pool_ready() || n > 0x7FFF0000ul) { errno = ENOMEM; return NULL; }
    total = n + sizeof(struct head) + align;
    void *base = AllocPooled(pool, total);
    if (!base) { errno = ENOMEM; return NULL; }
    p = ((unsigned long)base + sizeof(struct head) + align - 1) & ~(align - 1);
    h = (struct head *)p - 1;
    h->magic = MAGIC ^ p; h->base = base; h->total = total; h->size = n;
    return (void *)p;
}
static struct head *mine(void *p)
{
    struct head *h = (struct head *)p - 1;
    return h->magic == (MAGIC ^ (unsigned long)p) ? h : NULL;
}
unsigned long qrx_malloc(size_t n) __asm__("malloc");
unsigned long qrx_malloc(size_t n) { return (unsigned long)alloc_aligned(n, 16); }
unsigned long qrx_calloc(size_t a, size_t b) __asm__("calloc");
unsigned long qrx_calloc(size_t a, size_t b)
{
    size_t n;
    void *p;
    if (__builtin_mul_overflow(a, b, &n)) { errno = ENOMEM; return 0; }
    p = alloc_aligned(n, 16);
    if (p) { unsigned char *c = p; size_t i = n; while (i >= 4 && !((unsigned long)c & 3)) { *(unsigned long *)c = 0; c += 4; i -= 4; } while (i--) *c++ = 0; }
    return (unsigned long)p;
}
void __wrap_free(void *p)
{
    struct head *h;
    if (!p) return;
    h = mine(p);
    if (!h) { __real_free(p); return; }   /* from inside libcrt: vasprintf, getcwd and the like */
    h->magic = 0;
    FreePooled(pool, h->base, h->total);
}
unsigned long qrx_realloc(void *p, size_t n) __asm__("realloc");
unsigned long qrx_realloc(void *p, size_t n)
{
    struct head *h;
    void *q;
    size_t keep;
    if (!p) return (unsigned long)alloc_aligned(n, 16);
    h = mine(p);
    q = alloc_aligned(n, 16);
    if (!q) return 0;
    if (h) { keep = h->size < n ? h->size : n; CopyMem(p, q, keep); __wrap_free(p); }
    return (unsigned long)q;
}
unsigned long qrx_strdup(const char *s) __asm__("strdup");
unsigned long qrx_strdup(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    char *d = alloc_aligned(n + 1, 16);
    if (d) CopyMem((APTR)s, d, n + 1);
    return (unsigned long)d;
}
unsigned long qrx_strndup(const char *s, size_t max) __asm__("strndup");
unsigned long qrx_strndup(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    char *d = alloc_aligned(n + 1, 16);
    if (d) { CopyMem((APTR)s, d, n); d[n] = 0; }
    return (unsigned long)d;
}
int posix_memalign(void **out, size_t align, size_t n)
{
    void *p;
    if (align < 16) align = 16;
    p = alloc_aligned(n, align);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
