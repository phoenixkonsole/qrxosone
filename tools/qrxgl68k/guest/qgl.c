/* The guest half of the GL door: the command stream, the door call, and the functions that the
 * generated stubs in qgl_gen.c leave to hand-written code. */
#include "qgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOOR_MAGIC_REG (*(volatile uint32_t *)0xFF000880u)
#define DOOR_CALL_REG (*(volatile uint32_t *)0xFF00088Cu)
#define DOOR_MAGIC 0x51383644u
#define DOOR_FEATURE_GL 2u
#define SVC_GL 8u
#define STREAM_WORDS (256u * 1024u)

void qgl_glBufferSubData(unsigned int target, long offset, long size, const void *data);

uint32_t *qgl_cur, *qgl_lim;
static uint32_t *stream, stream_words;
static uint32_t block[32] __attribute__((aligned(16)));

#if defined(__m68k64__)
/* QRX OS One 64: the 68100EC64 board's HOST_CALL register (qrx_ec64_board.h) takes the block. */
#define BOARD_Q(off) (*(volatile uint64_t *)(0xFFFFFFFF00000000ull + (off)))
#define BOARD_L(off) (*(volatile uint32_t *)(0xFFFFFFFF00000000ull + (off)))
int qgl_available(void)
{
    static int state;
    if (!state)
        state = BOARD_Q(0) == 0x5152584543363442ull && (BOARD_L(0x408) & 2u) ? 1 : -1;
    return state > 0;
}
static void door_call(void) { BOARD_Q(0x400) = (uint64_t)(unsigned long)block; }
#else
int qgl_available(void)
{
    static int state;
    if (!state) {
        /* Whole 32-bit reads: gcc turns a tested bit into a byte access, which the door faults. */
        uint32_t magic = DOOR_MAGIC_REG, f = 0;
        __asm__ volatile("" : "+m"(magic));
        state = -1;
        if (magic == DOOR_MAGIC) {
            __asm__ volatile("move.l 0xFF000890,%0" : "=d"(f));
            if (f & DOOR_FEATURE_GL)
                state = 1;
        }
    }
    return state > 0;
}
static void door_call(void) { DOOR_CALL_REG = (uint32_t)(unsigned long)block; }
#endif

/* Runs what is in the stream. The engine leaves the last call's result in the block. */
static unsigned long long run(void)
{
    unsigned long long r = 0;
    if (stream && qgl_cur != stream && qgl_available()) {
        block[0] = SVC_GL; block[1] = 0; block[2] = 0; block[3] = 1;
        block[4] = (uint32_t)((unsigned long long)(unsigned long)stream >> 32);
        block[5] = (uint32_t)(unsigned long)stream;
        block[6] = (uint32_t)((char *)qgl_cur - (char *)stream);
        block[8] = 0; block[9] = 0;
        __asm__ volatile("" ::: "memory");
        door_call();
        __asm__ volatile("" ::: "memory");
        r = ((unsigned long long)block[8] << 32) | block[9];
    }
    qgl_cur = stream;
    return r;
}

uint32_t *qgl_flush_for(unsigned words)
{
    if (stream)
        run();
    if (!stream || words > stream_words) {
        unsigned n = words > STREAM_WORDS ? words + 1024u : STREAM_WORDS;
        free(stream);
        stream = malloc(n * 4u);
        if (!stream) {
            fprintf(stderr, "qgl: no memory for a stream of %u words\n", n);
            abort();
        }
        stream_words = n;
    }
    qgl_cur = stream;
    qgl_lim = stream + stream_words;
    return qgl_cur;
}

unsigned long long qgl_sync(void) { return run(); }

void qgl_unsupported(const char *name)
{
    static const char *seen[64];
    static unsigned n;
    for (unsigned i = 0; i < n; i++)
        if (seen[i] == name)
            return;
    if (n < 64)
        seen[n++] = name;
    fprintf(stderr, "qgl: %s is not supported by the GL door\n", name);
}

static uint32_t *op(unsigned id, unsigned words)
{
    uint32_t *q = qgl_put(words + 1);
    *q++ = ((uint32_t)id << 20) | words;
    return q;
}

unsigned qgl_create_context(unsigned share)
{
    if (!qgl_available())
        return 0;
    uint32_t *q = op(QGL_OP_CREATE, 1);
    q[0] = share;
    return (unsigned)qgl_sync();
}

void qgl_destroy_context(unsigned ctx)
{
    uint32_t *q = op(QGL_OP_DESTROY, 1);
    q[0] = ctx;
    qgl_sync();
}

int qgl_make_current(unsigned ctx, int width, int height)
{
    uint32_t *q = op(QGL_OP_MAKECURRENT, 3);
    q[0] = ctx; q[1] = (uint32_t)width; q[2] = (uint32_t)height;
    return (int)qgl_sync();
}

void qgl_read_frame(void *dst, int bytes_per_row, int width, int height)
{
    uint32_t *q = op(QGL_OP_SWAP, 5);
    q = qgl_ptr(q, dst);
    q[0] = (uint32_t)bytes_per_row; q[1] = (uint32_t)width; q[2] = (uint32_t)height;
    qgl_sync();
}

void *qgl_GetProcAddress(const char *name)
{
    unsigned lo = 0, hi = qgl_nnames;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(name, qgl_names[mid].name);
        if (!c)
            return qgl_names[mid].fn;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* ---- glGetString: kept per name, as GL promises a pointer that stays valid. */
const unsigned char *qgl_glGetString(unsigned int name)
{
    static struct { unsigned name; char *s; } cache[8];
    unsigned i;
    for (i = 0; i < 8 && cache[i].s; i++)
        if (cache[i].name == name)
            return (const unsigned char *)cache[i].s;
    if (!qgl_available())
        return NULL;
    unsigned size = 256;
    char *s = NULL;
    for (;;) {
        s = realloc(s, size);
        uint32_t *q = op(QGL_OP_GETSTRING, 4);
        q[0] = name;
        q = qgl_ptr(q + 1, s);
        q[0] = size;
        unsigned len = (unsigned)qgl_sync();
        if (len == 0xFFFFFFFFu) {
            free(s);
            return NULL;
        }
        if (len < size)
            break;
        size = len + 1;
    }
    if (i < 8) {
        cache[i].name = name;
        cache[i].s = s;
    }
    return (const unsigned char *)s;
}
extern __typeof__(qgl_glGetString) glGetString __attribute__((alias("qgl_glGetString")));

/* ---- shader text: all strings joined, sent inline. */
static void shader_source(unsigned shader, unsigned arb, int count, const char *const *str, const int *len)
{
    unsigned total = 0;
    for (int i = 0; i < count; i++)
        total += len && len[i] >= 0 ? (unsigned)len[i] : (unsigned)strlen(str[i]);
    uint32_t *q = op(QGL_OP_SHADERSOURCE, 3 + QGL_W(total));
    q[0] = shader; q[1] = arb; q[2] = total;
    char *d = (char *)(q + 3);
    if (QGL_W(total))
        q[3 + QGL_W(total) - 1] = 0;
    for (int i = 0; i < count; i++) {
        unsigned l = len && len[i] >= 0 ? (unsigned)len[i] : (unsigned)strlen(str[i]);
        memcpy(d, str[i], l);
        d += l;
    }
}
void qgl_glShaderSource(unsigned int shader, int count, const char *const *string, const int *length)
{
    shader_source(shader, 0, count, string, length);
}
void qgl_glShaderSourceARB(unsigned int obj, int count, const char **string, const int *length)
{
    shader_source(obj, 1, count, (const char *const *)string, length);
}

/* ---- mapped buffers: a copy in guest memory, written back on unmap. GL maps per buffer object, so the
 * engine names the buffer bound to the target. */
static struct { unsigned name; void *p; unsigned size; unsigned access; } maps[16];

static unsigned bound_buffer(unsigned target, unsigned *size)
{
    uint32_t *q = op(QGL_OP_BUFSIZE, 1);
    q[0] = target;
    unsigned long long r = qgl_sync();
    *size = (unsigned)r;
    return (unsigned)(r >> 32);
}

static void *map_buffer(unsigned target, unsigned access)
{
    unsigned size, name = bound_buffer(target, &size), i, free_slot = 16;
    if (!name || !size)
        return NULL;
    for (i = 0; i < 16; i++) {
        if (maps[i].p && maps[i].name == name)
            return NULL; /* already mapped: GL_INVALID_OPERATION */
        if (!maps[i].p && free_slot == 16)
            free_slot = i;
    }
    if (free_slot == 16)
        return NULL;
    void *p = malloc(size);
    if (!p)
        return NULL;
    if (access != 0x88B9) { /* not GL_WRITE_ONLY: the program will read it */
        uint32_t *q = op(QGL_OP_BUFREAD, 7);
        q[0] = target;
        q = qgl_64(q + 1, 0);
        q = qgl_64(q, size);
        qgl_ptr(q, p);
        qgl_sync();
    }
    maps[free_slot].name = name; maps[free_slot].p = p; maps[free_slot].size = size; maps[free_slot].access = access;
    return p;
}
static unsigned char unmap_buffer(unsigned target)
{
    unsigned size, name = bound_buffer(target, &size);
    for (unsigned i = 0; i < 16; i++)
        if (maps[i].p && maps[i].name == name) {
            if (maps[i].access != 0x88B8) /* not GL_READ_ONLY */
                qgl_glBufferSubData(target, 0, (long)maps[i].size, maps[i].p);
            free(maps[i].p);
            maps[i].p = NULL;
            return 1;
        }
    return 0;
}
void *qgl_glMapBuffer(unsigned int target, unsigned int access) { return map_buffer(target, access); }
void *qgl_glMapBufferARB(unsigned int target, unsigned int access) { return map_buffer(target, access); }
unsigned char qgl_glUnmapBuffer(unsigned int target) { return unmap_buffer(target); }
unsigned char qgl_glUnmapBufferARB(unsigned int target) { return unmap_buffer(target); }

static void buffer_pointer(unsigned target, unsigned pname, void **params)
{
    *params = NULL;
    if (pname != 0x88BD) /* GL_BUFFER_MAP_POINTER */
        return;
    unsigned size, name = bound_buffer(target, &size);
    for (unsigned i = 0; i < 16; i++)
        if (maps[i].p && maps[i].name == name)
            *params = maps[i].p;
}
void qgl_glGetBufferPointerv(unsigned int target, unsigned int pname, void **params) { buffer_pointer(target, pname, params); }
void qgl_glGetBufferPointervARB(unsigned int target, unsigned int pname, void **params) { buffer_pointer(target, pname, params); }

/* ---- array pointers: the engine keeps the guest addresses. */
static void *get_ptr(unsigned kind, unsigned index, unsigned pname)
{
    uint32_t *q = op(QGL_OP_GETPTR, 3);
    q[0] = kind; q[1] = index; q[2] = pname;
    return (void *)(unsigned long)qgl_sync();
}
void qgl_glGetPointerv(unsigned int pname, void **params) { *params = get_ptr(0, 0, pname); }
extern __typeof__(qgl_glGetPointerv) glGetPointerv __attribute__((alias("qgl_glGetPointerv")));
void qgl_glGetVertexAttribPointerv(unsigned int index, unsigned int pname, void **pointer) { *pointer = get_ptr(1, index, pname); }
void qgl_glGetVertexAttribPointervARB(unsigned int index, unsigned int pname, void **pointer) { *pointer = get_ptr(1, index, pname); }
