/* OpenGL 3.3 core for programs of QRX OS One 64, drawn by the host through HOST_CALL service 13 of the
 * 68100EC64 board (engine full68k/qrx_gl3.c). Calls go into the command stream of the current context;
 * see tools/qrxgl68k/gen3.py for its format and what the host does with pointers.
 *
 * Programs get the functions with qgl3_GetProcAddress. Buffer data goes to the host as bytes: vertex,
 * instance and index data must be little endian. Texel data is the guest's own (big endian). */
#ifndef QGL3_H
#define QGL3_H
#include <stdint.h>
#include <string.h>
#include "qgl3_ids.h"

/* the door's own requests, ids below the first GL function */
enum {
    QGL3_OP_CREATE = 1,       /* share context id -> context id */
    QGL3_OP_DESTROY = 2,      /* context id */
    QGL3_OP_RESIZE = 3,       /* width, height of framebuffer 0 -> 1 */
    QGL3_OP_READFRAME = 4,    /* dst (64), bytes per row, width, height: framebuffer 0 as ARGB, top row first */
    QGL3_OP_GETSTRING = 5,    /* name, index (~0 for glGetString), dst (64), size -> length */
    QGL3_OP_SHADERSOURCE = 6, /* shader, length, the text inline */
    QGL3_OP_GETERROR = 7,     /* -> the error */
};

typedef struct qgl3_context {
    unsigned id;
    uint32_t *stream, *cur, *lim;
    unsigned words;
    uint32_t block[32] __attribute__((aligned(16)));
} qgl3_context;

/* the current context of the calling thread (qgl3_set_tls, else one for the program) */
extern qgl3_context *(*qgl3_tls_get)(void);
qgl3_context *qgl3_current(void);
uint32_t *qgl3_flush_for(qgl3_context *c, unsigned words);
unsigned long long qgl3_sync(void);
void qgl3_unsupported(const char *name);

#define QGL3_W(b) ((b) == 0xFFFFFFFFu ? 0u : ((b) + 3u) / 4u)

static inline uint32_t *qgl3_put(unsigned n)
{
    qgl3_context *c = qgl3_current();
    uint32_t *p = c->cur;
    if ((unsigned)(c->lim - p) < n)
        p = qgl3_flush_for(c, n);
    c->cur = p + n;
    return p;
}
static inline uint32_t qgl3_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint32_t *qgl3_d(uint32_t *q, double d) { memcpy(q, &d, 8); return q + 2; }
static inline uint32_t *qgl3_64(uint32_t *q, unsigned long long v) { q[0] = (uint32_t)(v >> 32); q[1] = (uint32_t)v; return q + 2; }
static inline uint32_t *qgl3_inline(uint32_t *q, const void *p, unsigned bytes)
{
    *q++ = bytes;
    if (bytes != 0xFFFFFFFFu) {
        unsigned w = QGL3_W(bytes);
        if (w)
            q[w - 1] = 0;
        memcpy(q, p, bytes);
        q += w;
    }
    return q;
}

struct qgl3_name { const char *name; void *fn; };
extern const struct qgl3_name qgl3_names[];
extern const unsigned qgl3_nnames;

/* ---- the program's side */
int qgl3_available(void);
/* where the current context of a thread is kept; without it one for the whole program */
void qgl3_set_tls(qgl3_context *(*get)(void), void (*set)(qgl3_context *));
qgl3_context *qgl3_create_context(qgl3_context *share);
void qgl3_destroy_context(qgl3_context *c);
int qgl3_make_current(qgl3_context *c);  /* NULL: none */
int qgl3_resize(int width, int height);   /* framebuffer 0 of the current context */
void qgl3_read_frame(void *dst, int bytes_per_row, int width, int height);
void qgl3_flush(void);
void *qgl3_GetProcAddress(const char *name);
#endif
