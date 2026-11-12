/* malloc for Blender on QRX OS One. Core 1 must allocate without AROS, so all of it comes from one
 * arena taken at the start, under a lock that works on both cores. Boundary tags with free lists
 * by size. Pointers from AROS's own malloc (some libc functions return those) go back to it. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include "qrx_core1.h"

extern void *__real_malloc(size_t);
extern void __real_free(void *);
extern void *__real_realloc(void *, size_t);

typedef struct Chunk {
	uint32_t prev;   /* size of the previous chunk, 0 if first */
	uint32_t size;   /* size of this chunk, header included; low bits are flags */
	struct Chunk *next, *back;  /* only while free */
} Chunk;
#define HDR 8u
#define F_USED 1u
#define F_SHIFTED 2u  /* memalign: the size field holds the distance to the real payload */
#define MINCHUNK 24u
#define CSIZE(c) ((c)->size & ~7u)
#define NEXT(c) ((Chunk *)((char *)(c) + CSIZE(c)))
#define PREV(c) ((Chunk *)((char *)(c) - (c)->prev))
#define PAYLOAD(c) ((void *)((char *)(c) + HDR))
#define CHUNK(p) ((Chunk *)((char *)(p) - HDR))
#define NSMALL 64u    /* by 16 bytes up to 1 KiB */
#define NLARGE 24u    /* by powers of two above */
static Chunk *bins[NSMALL + NLARGE];
static qrx_lock_t heap_lock;
static char *arena, *arena_end;
static int heap_state;  /* 0 not tried, 1 ready, -1 no arena */
static uint32_t bitmap[(NSMALL + NLARGE + 31) / 32];

static unsigned bin_of(uint32_t size)
{
	if (size < NSMALL * 16u) return size >> 4;
	unsigned b = NSMALL;
	size >>= 11;
	while (size && b < NSMALL + NLARGE - 1) { size >>= 1; b++; }
	return b;
}
static void bin_add(Chunk *c)
{
	unsigned b = bin_of(CSIZE(c));
	c->back = NULL;
	c->next = bins[b];
	if (c->next) c->next->back = c;
	bins[b] = c;
	bitmap[b >> 5] |= 1u << (b & 31);
}
static void bin_del(Chunk *c)
{
	unsigned b = bin_of(CSIZE(c));
	if (c->back) c->back->next = c->next;
	else if (!(bins[b] = c->next)) bitmap[b >> 5] &= ~(1u << (b & 31));
	if (c->next) c->next->back = c->back;
}
static void heap_init(void)
{
	/* Takes most of the largest free block: nothing else needs that much while Blender runs. */
	ULONG largest = AvailMem(MEMF_ANY | MEMF_LARGEST);
	/* Only Exec here: this can run before the C library is open (libgcc's __register_frame). */
	ULONG want = largest / 4u * 3u;
	if (want > largest - largest / 16u) want = largest - largest / 16u;
	/* AllocMem did not come back with a block of 3 GiB, so stay below 2 GiB until that is understood. */
	if (want > 0x7FF00000u) want = 0x7FF00000u;
	want &= ~15u;
	arena = AllocMem(want, MEMF_ANY);
	if (!arena) { heap_state = -1; return; }
	arena_end = arena + want;
	Chunk *c = (Chunk *)arena;
	c->prev = 0;
	c->size = (want - HDR) & ~7u;
	Chunk *end = NEXT(c);
	end->prev = CSIZE(c);
	end->size = 0 | F_USED;  /* sentinel */
	bin_add(c);
	heap_state = 1;
}
static Chunk *take(uint32_t need)
{
	unsigned b = bin_of(need);
	/* A bin above the small range holds mixed sizes: look through it first. */
	if (b >= NSMALL || bins[b]) {
		for (Chunk *c = bins[b]; c; c = c->next) {
			if (CSIZE(c) >= need) { bin_del(c); return c; }
		}
		b++;
	}
	for (unsigned w = b >> 5; w < sizeof(bitmap) / sizeof(bitmap[0]); w++) {
		uint32_t m = bitmap[w];
		if (w == b >> 5) m &= ~0u << (b & 31);
		if (m) {
			unsigned bit = 0;
			while (!(m & 1u)) { m >>= 1; bit++; }
			Chunk *c = bins[(w << 5) + bit];
			bin_del(c);
			return c;
		}
	}
	return NULL;
}
static void *alloc_locked(size_t n)
{
	if (n > 0xFFFF0000u) return NULL;
	uint32_t need = (uint32_t)(n + HDR + 7u) & ~7u;
	if (need < MINCHUNK) need = MINCHUNK;
	Chunk *c = take(need);
	if (!c) return NULL;
	uint32_t have = CSIZE(c);
	if (have - need >= MINCHUNK) {
		Chunk *rest = (Chunk *)((char *)c + need);
		rest->prev = need;
		rest->size = have - need;
		NEXT(rest)->prev = have - need;
		c->size = need;
		bin_add(rest);
	}
	c->size |= F_USED;
	return PAYLOAD(c);
}
static void free_locked(Chunk *c)
{
	c->size &= ~F_USED;
	Chunk *n = NEXT(c);
	if (!(n->size & F_USED)) {
		bin_del(n);
		c->size += CSIZE(n);
	}
	if (c->prev) {
		Chunk *p = PREV(c);
		if (!(p->size & F_USED)) {
			bin_del(p);
			p->size += CSIZE(c);
			c = p;
		}
	}
	NEXT(c)->prev = CSIZE(c);
	bin_add(c);
}
static void __attribute__((destructor)) heap_exit(void)
{
	/* AROS gives nothing back when a program ends. */
	if (heap_state == 1) {
		FreeMem(arena, arena_end - arena);
		heap_state = -1;
		arena = arena_end = NULL;
	}
}
static inline int in_arena(void *p) { return (char *)p >= arena && (char *)p < arena_end; }

void *__wrap_malloc(size_t n)
{
	if (!heap_state) heap_init();
	if (heap_state < 0) return __real_malloc(n);
	qrx_lock(&heap_lock);
	void *p = alloc_locked(n);
	qrx_unlock(&heap_lock);
	if (!p) errno = ENOMEM;
	return p;
}
void __wrap_free(void *p)
{
	if (!p) return;
	if (!in_arena(p)) { __real_free(p); return; }
	Chunk *c = CHUNK(p);
	if (c->size & F_SHIFTED) c = (Chunk *)((char *)c - (c->size & ~7u));
	qrx_lock(&heap_lock);
	free_locked(c);
	qrx_unlock(&heap_lock);
}
void *__wrap_calloc(size_t a, size_t b)
{
	size_t n = a * b;
	if (b && n / b != a) return NULL;
	void *p = __wrap_malloc(n);
	if (p) memset(p, 0, n);
	return p;
}
void *memalign(size_t alignment, size_t n)
{
	if (alignment <= 8) return __wrap_malloc(n);
	char *raw = __wrap_malloc(n + alignment + MINCHUNK);
	if (!raw) return NULL;
	char *p = (char *)(((uintptr_t)raw + HDR + alignment - 1) & ~(uintptr_t)(alignment - 1));
	if (p == raw) return raw;
	/* A fake header in front says how far back the real chunk starts. */
	Chunk *fake = CHUNK(p);
	fake->size = (uint32_t)((char *)fake - (char *)CHUNK(raw)) | F_SHIFTED;
	return p;
}
void *__wrap_realloc(void *p, size_t n)
{
	if (!p) return __wrap_malloc(n);
	if (!in_arena(p)) return __real_realloc(p, n);
	if (!n) { __wrap_free(p); return NULL; }
	Chunk *c = CHUNK(p);
	size_t have;
	if (c->size & F_SHIFTED) {
		Chunk *real = (Chunk *)((char *)c - (c->size & ~7u));
		have = CSIZE(real) - HDR - ((char *)p - (char *)PAYLOAD(real));
	}
	else {
		have = CSIZE(c) - HDR;
	}
	if (n <= have) return p;
	void *q = __wrap_malloc(n);
	if (q) {
		memcpy(q, p, have);
		__wrap_free(p);
	}
	return q;
}
char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = __wrap_malloc(n);
	if (p) memcpy(p, s, n);
	return p;
}
char *strndup(const char *s, size_t max)
{
	size_t n = strnlen(s, max);
	char *p = __wrap_malloc(n + 1);
	if (p) { memcpy(p, s, n); p[n] = 0; }
	return p;
}
