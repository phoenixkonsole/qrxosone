/* The guest half of the GL 3.3 door: contexts with a command stream each, the board call, and the
 * functions the generated stubs in qgl3_gen.c leave to hand-written code (strings, shader text,
 * glGetError). QRX OS One 64 only: the 68100EC64 board's HOST_CALL register takes the block. */
#include "qgl3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOARD_Q(off) (*(volatile uint64_t *)(0xFFFFFFFF00000000ull + (off)))
#define BOARD_L(off) (*(volatile uint32_t *)(0xFFFFFFFF00000000ull + (off)))
#define BOARD_MAGIC 0x5152584543363442ull
#define FEATURE_GL3 8u
#define SVC_GL3 13u
#define STREAM_WORDS (256u * 1024u)

int qgl3_available(void)
{
    static int state;
    if (!state)
        state = BOARD_Q(0) == BOARD_MAGIC && (BOARD_L(0x408) & FEATURE_GL3) ? 1 : -1;
    return state > 0;
}

/* ---- the current context: one for the program, or where the program keeps it per thread */
static qgl3_context none;      /* calls without a context go here; the host refuses them */
static qgl3_context *global_cur;
static qgl3_context *get_global(void) { return global_cur; }
static void set_global(qgl3_context *c) { global_cur = c; }
qgl3_context *(*qgl3_tls_get)(void) = get_global;
static void (*tls_set)(qgl3_context *) = set_global;
static unsigned local_error;   /* raised by this library, given out first by glGetError */

void qgl3_set_tls(qgl3_context *(*get)(void), void (*set)(qgl3_context *))
{
    qgl3_tls_get = get ? get : get_global;
    tls_set = set ? set : set_global;
}

qgl3_context *qgl3_current(void)
{
    qgl3_context *c = qgl3_tls_get();
    return c ? c : &none;
}

/* runs what is in the context's stream; the host leaves the last call's result in the block */
static unsigned long long run(qgl3_context *c)
{
    unsigned long long r = 0;
    if (c->stream && c->cur != c->stream && qgl3_available()) {
        uint32_t *b = c->block;
        b[0] = SVC_GL3; b[1] = 0; b[2] = 0; b[3] = 1;
        b[4] = (uint32_t)((unsigned long long)(unsigned long)c->stream >> 32);
        b[5] = (uint32_t)(unsigned long)c->stream;
        b[6] = (uint32_t)((char *)c->cur - (char *)c->stream);
        b[7] = c->id;
        b[8] = 0; b[9] = 0;
        __asm__ volatile("" ::: "memory");
        BOARD_Q(0x400) = (uint64_t)(unsigned long)b;
        __asm__ volatile("" ::: "memory");
        r = ((unsigned long long)b[8] << 32) | b[9];
    }
    c->cur = c->stream;
    return r;
}

uint32_t *qgl3_flush_for(qgl3_context *c, unsigned words)
{
    if (c->stream)
        run(c);
    if (!c->stream || words > c->words) {
        unsigned n = words > STREAM_WORDS ? words + 1024u : STREAM_WORDS;
        free(c->stream);
        c->stream = malloc(n * 4u);
        if (!c->stream) {
            fprintf(stderr, "qgl3: no memory for a stream of %u words\n", n);
            abort();
        }
        c->words = n;
    }
    c->cur = c->stream;
    c->lim = c->stream + c->words;
    return c->cur;
}

unsigned long long qgl3_sync(void) { return run(qgl3_current()); }
void qgl3_flush(void) { run(qgl3_current()); }

void qgl3_unsupported(const char *name)
{
    static const char *seen[64];
    static unsigned n;
    if (!local_error)
        local_error = 0x0502; /* GL_INVALID_OPERATION */
    for (unsigned i = 0; i < n; i++)
        if (seen[i] == name)
            return;
    if (n < 64)
        seen[n++] = name;
    fprintf(stderr, "qgl3: %s is not carried by the GL door\n", name);
}

static uint32_t *op(unsigned id, unsigned words)
{
    uint32_t *q = qgl3_put(words + 1);
    *q++ = ((uint32_t)id << 20) | words;
    return q;
}

/* ---- contexts */
qgl3_context *qgl3_create_context(qgl3_context *share)
{
    if (!qgl3_available())
        return NULL;
    qgl3_context *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    /* sent on the new context's own stream, with no context in the block */
    qgl3_context *was = qgl3_tls_get();
    tls_set(c);
    uint32_t *q = op(QGL3_OP_CREATE, 1);
    q[0] = share ? share->id : 0;
    c->id = (unsigned)qgl3_sync();
    tls_set(was);
    if (!c->id) {
        free(c->stream);
        free(c);
        return NULL;
    }
    return c;
}

void qgl3_destroy_context(qgl3_context *c)
{
    if (!c)
        return;
    qgl3_context *was = qgl3_tls_get();
    tls_set(c);
    run(c);
    uint32_t *q = op(QGL3_OP_DESTROY, 1);
    q[0] = c->id;
    c->id = 0;          /* the stream runs without a context */
    qgl3_sync();
    tls_set(was == c ? NULL : was);
    free(c->stream);
    free(c);
}

int qgl3_make_current(qgl3_context *c)
{
    qgl3_context *was = qgl3_tls_get();
    if (was && was != c)
        run(was);       /* what it has queued is drawn before another context takes over */
    tls_set(c);
    return 1;
}

int qgl3_resize(int width, int height)
{
    uint32_t *q = op(QGL3_OP_RESIZE, 2);
    q[0] = (uint32_t)width; q[1] = (uint32_t)height;
    return (int)qgl3_sync();
}

void qgl3_read_frame(void *dst, int bytes_per_row, int width, int height)
{
    uint32_t *q = op(QGL3_OP_READFRAME, 5);
    q = qgl3_64(q, (unsigned long long)(unsigned long)dst);
    q[0] = (uint32_t)bytes_per_row; q[1] = (uint32_t)width; q[2] = (uint32_t)height;
    qgl3_sync();
}

void *qgl3_GetProcAddress(const char *name)
{
    unsigned lo = 0, hi = qgl3_nnames;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(name, qgl3_names[mid].name);
        if (!c)
            return qgl3_names[mid].fn;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* ---- strings: kept, as GL promises pointers that stay valid. One host, so one set for all contexts. */
static char *fetch_string(unsigned name, unsigned index)
{
    unsigned size = 256;
    char *s = NULL;
    for (;;) {
        char *n = realloc(s, size);
        if (!n) {
            free(s);
            return NULL;
        }
        s = n;
        uint32_t *q = op(QGL3_OP_GETSTRING, 5);
        q[0] = name; q[1] = index;
        q = qgl3_64(q + 2, (unsigned long long)(unsigned long)s);
        q[0] = size;
        unsigned len = (unsigned)qgl3_sync();
        if (len == 0xFFFFFFFFu) {
            free(s);
            if (!local_error)
                local_error = 0x0500; /* GL_INVALID_ENUM */
            return NULL;
        }
        if (len < size)
            return s;
        size = len + 1;
    }
}

const unsigned char *q3_glGetString(unsigned int name)
{
    static struct { unsigned name; char *s; } cache[8];
    unsigned i;
    for (i = 0; i < 8 && cache[i].s; i++)
        if (cache[i].name == name)
            return (const unsigned char *)cache[i].s;
    char *s = fetch_string(name, 0xFFFFFFFFu);
    if (s && i < 8) {
        cache[i].name = name;
        cache[i].s = s;
    }
    return (const unsigned char *)s;
}

const unsigned char *q3_glGetStringi(unsigned int name, unsigned int index)
{
    /* GL_EXTENSIONS and GL_SHADING_LANGUAGE_VERSION have lists; each entry is asked for once */
    static struct { unsigned name, n; char **s; } lists[2];
    unsigned k = name == 0x1F03 ? 0 : 1;
    if (lists[k].name != name) {
        if (lists[k].s && lists[k].name)
            return (const unsigned char *)fetch_string(name, index);  /* a third list: not kept */
        lists[k].name = name;
    }
    if (index >= lists[k].n) {
        unsigned n = index + 64;
        char **s = realloc(lists[k].s, n * sizeof *s);
        if (!s)
            return NULL;
        memset(s + lists[k].n, 0, (n - lists[k].n) * sizeof *s);
        lists[k].s = s;
        lists[k].n = n;
    }
    if (!lists[k].s[index])
        lists[k].s[index] = fetch_string(name, index);
    return (const unsigned char *)lists[k].s[index];
}

/* ---- shader text: all strings joined, sent inline */
void q3_glShaderSource(unsigned int shader, int count, const char **string, const int *length)
{
    unsigned total = 0;
    for (int i = 0; i < count; i++)
        total += length && length[i] >= 0 ? (unsigned)length[i] : (unsigned)strlen(string[i]);
    uint32_t *q = op(QGL3_OP_SHADERSOURCE, 2 + QGL3_W(total));
    q[0] = shader; q[1] = total;
    char *d = (char *)(q + 2);
    if (QGL3_W(total))
        q[2 + QGL3_W(total) - 1] = 0;
    for (int i = 0; i < count; i++) {
        unsigned l = length && length[i] >= 0 ? (unsigned)length[i] : (unsigned)strlen(string[i]);
        memcpy(d, string[i], l);
        d += l;
    }
}

unsigned int q3_glGetError(void)
{
    if (local_error) {
        unsigned e = local_error;
        local_error = 0;
        return e;
    }
    op(QGL3_OP_GETERROR, 0);
    return (unsigned)qgl3_sync();
}
