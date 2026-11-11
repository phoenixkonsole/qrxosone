/* OpenGL 2.1 for 68k programs on QRX OS One, drawn by the Mac through door service 8 of the ARMithlon
 * engine (full68k/qrx_gl68k.c). Calls go into a command stream; see tools/qrxgl68k/gen.py for its
 * format. This header is the library's own; programs use glew.h or GL/gl.h and qgl_api.h. */
#ifndef QGL_H
#define QGL_H
#include <stdint.h>
#include <string.h>
#include "qgl_ids.h"

/* The door's own requests, ids below the first GL function. */
enum {
    QGL_OP_CREATE = 1,      /* share context -> context id */
    QGL_OP_MAKECURRENT = 2, /* context, width, height -> 1 */
    QGL_OP_DESTROY = 3,     /* context */
    QGL_OP_SWAP = 4,        /* dst (64), bytes per row, width, height: the frame as ARGB, top row first */
    QGL_OP_GETSTRING = 5,   /* name, dst (64), size -> length */
    QGL_OP_SHADERSOURCE = 6, /* shader, ARB flag, inline text */
    QGL_OP_BUFSIZE = 7,     /* target -> size of the bound buffer */
    QGL_OP_BUFREAD = 8,     /* target, offset (64), size (64), dst (64) */
    QGL_OP_GETPTR = 9,      /* kind, index, pname -> guest address (64) */
};

extern uint32_t *qgl_cur, *qgl_lim;
uint32_t *qgl_flush_for(unsigned words);
unsigned long long qgl_sync(void);
void qgl_unsupported(const char *name);

#define QGL_W(b) ((b) == 0xFFFFFFFFu ? 0u : ((b) + 3u) / 4u)

static inline uint32_t *qgl_put(unsigned n)
{
    uint32_t *p = qgl_cur;
    if ((unsigned)(qgl_lim - p) < n)
        p = qgl_flush_for(n);
    qgl_cur = p + n;
    return p;
}
static inline uint32_t qgl_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint32_t *qgl_d(uint32_t *q, double d) { memcpy(q, &d, 8); return q + 2; }
static inline uint32_t *qgl_64(uint32_t *q, unsigned long long v) { q[0] = (uint32_t)(v >> 32); q[1] = (uint32_t)v; return q + 2; }
static inline uint32_t *qgl_ptr(uint32_t *q, const void *p) { q[0] = 0; q[1] = (uint32_t)(unsigned long)p; return q + 2; }
static inline uint32_t *qgl_inline(uint32_t *q, const void *p, unsigned bytes)
{
    *q++ = bytes;
    if (bytes != 0xFFFFFFFFu) {
        unsigned w = QGL_W(bytes);
        if (w)
            q[w - 1] = 0;
        memcpy(q, p, bytes);
        q += w;
    }
    return q;
}

struct qgl_name { const char *name; void *fn; };
extern const struct qgl_name qgl_names[];
extern const unsigned qgl_nnames;

#include "qgl_api.h"
#endif
