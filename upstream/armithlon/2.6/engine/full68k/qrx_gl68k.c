/* OpenGL 2.1 for native 68k programs: door service 8 runs the command stream of the guest library
 * tools/qrxgl68k/guest on a real OpenGL 2.1 context of the Mac (the legacy profile, GLSL 1.20).
 *
 * The guest is big-endian, the Mac little-endian. Pixel transfers are left to GL with the swap-bytes
 * pixel store on; values in the stream are swapped when they are read. Vertex data has no such switch,
 * so buffer objects live here as plain copies in the guest's byte order and every draw call converts
 * the vertices it uses into client arrays. Each context draws into an FBO the size of its window, which
 * stands for framebuffer 0; the guest fetches the frame (QGL_OP_SWAP) and puts it into its window.
 *
 * Calls arrive on the 68k CPU thread, so that thread owns every context. Guest addresses come as 64-bit
 * values, as the stream carries them. */
#define GL_SILENCE_DEPRECATION
#include "qrx_gl68k.h"
#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
typedef CGLContextObj QglNative;
#define QGL_PROC(name) dlsym(RTLD_DEFAULT, name)
#else
/* Linux (Raspberry Pi: Mesa V3D): a surfaceless EGL display with the desktop OpenGL API.
 * Contexts never draw to a window here either; each has its FBO as on the Mac. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
typedef EGLContext QglNative;
#define QGL_PROC(name) ((void *)eglGetProcAddress(name))
/* Extension entry points are not exported by glvnd's libOpenGL (OpenGL without GLX, which the
 * Pi image links so it needs no X11): they come from EGL on first use. */
#define QGL_EXT(type, name) static type qgl_##name(void) { static type f; if (!f) f = (type)QGL_PROC(#name); return f; }
QGL_EXT(PFNGLBINDFRAMEBUFFEREXTPROC, glBindFramebufferEXT)
QGL_EXT(PFNGLBINDRENDERBUFFEREXTPROC, glBindRenderbufferEXT)
QGL_EXT(PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC, glCheckFramebufferStatusEXT)
QGL_EXT(PFNGLDELETEFRAMEBUFFERSEXTPROC, glDeleteFramebuffersEXT)
QGL_EXT(PFNGLDELETERENDERBUFFERSEXTPROC, glDeleteRenderbuffersEXT)
QGL_EXT(PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC, glFramebufferRenderbufferEXT)
QGL_EXT(PFNGLGENFRAMEBUFFERSEXTPROC, glGenFramebuffersEXT)
QGL_EXT(PFNGLGENRENDERBUFFERSEXTPROC, glGenRenderbuffersEXT)
QGL_EXT(PFNGLRENDERBUFFERSTORAGEEXTPROC, glRenderbufferStorageEXT)
QGL_EXT(PFNGLSHADERSOURCEARBPROC, glShaderSourceARB)
#define glBindFramebufferEXT qgl_glBindFramebufferEXT()
#define glBindRenderbufferEXT qgl_glBindRenderbufferEXT()
#define glCheckFramebufferStatusEXT qgl_glCheckFramebufferStatusEXT()
#define glDeleteFramebuffersEXT qgl_glDeleteFramebuffersEXT()
#define glDeleteRenderbuffersEXT qgl_glDeleteRenderbuffersEXT()
#define glFramebufferRenderbufferEXT qgl_glFramebufferRenderbufferEXT()
#define glGenFramebuffersEXT qgl_glGenFramebuffersEXT()
#define glGenRenderbuffersEXT qgl_glGenRenderbuffersEXT()
#define glRenderbufferStorageEXT qgl_glRenderbufferStorageEXT()
#define glShaderSourceARB qgl_glShaderSourceARB()
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_CREATE = 1, OP_MAKECURRENT, OP_DESTROY, OP_SWAP, OP_GETSTRING, OP_SHADERSOURCE, OP_BUFSIZE, OP_BUFREAD, OP_GETPTR };
#define MAX_CTX 32
#define MAX_TEX_UNITS 8
#define MAX_ATTRIBS 16

typedef struct {
    bool on;
    int size;
    uint32_t type;
    int stride;
    bool norm;
    uint32_t buf;  /* buffer object the pointer is an offset into, 0 for guest memory */
    uint64_t addr;
} Array;
enum { A_VERTEX, A_NORMAL, A_COLOR, A_COLOR2, A_FOG, A_INDEX, A_EDGE, A_TEX0, A_ATTR0 = A_TEX0 + MAX_TEX_UNITS, A_COUNT = A_ATTR0 + MAX_ATTRIBS };

typedef struct {
    Array arr[A_COUNT];
    unsigned client_tex;
    bool unpack_swap, pack_swap; /* as the guest sees them */
} ClientState;

typedef struct {
    bool used;
    QglNative cgl;
    GLuint fbo, rb_color, rb_depth;
    int w, h;
    bool started;
    uint32_t draw_fb, read_fb;     /* as the guest sees them: 0 is the window */
    uint32_t draw_buf, read_buf;   /* GL_BACK and so on, as asked for */
    uint32_t bind_array, bind_element, bind_pack, bind_unpack;
    ClientState cs;
    ClientState cs_stack[16];
    unsigned cs_depth;
    uint32_t render_mode;
    uint32_t *select_host; int select_size; uint64_t select_guest;
    float *feedback_host; int feedback_size; uint64_t feedback_guest;
} Ctx;

typedef struct Buf {
    uint32_t name;
    uint8_t *data;
    size_t size;
    uint32_t usage;
    struct Buf *next;
} Buf;

typedef struct QGL {
    uint8_t *ram;
    uint64_t ram_size;
    uint64_t ret;
    Ctx ctx[MAX_CTX];
    Ctx *cur;
    QglNative share;
    /* Per-command scratch memory. */
    uint8_t *arena;
    size_t arena_size, arena_used;
    Buf *bufs[256];
    uint32_t next_buf;
    bool resolved;
    bool warned[4096];
    uint64_t calls, streams;
} QGL;
static QGL G;

/* ---- reading the stream */
static inline uint32_t R32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint64_t R64(const uint8_t *p) { return ((uint64_t)R32(p) << 32) | R32(p + 4); }
static inline float RF(const uint8_t *p) { uint32_t u = R32(p); float f; memcpy(&f, &u, 4); return f; }
static inline double RD(const uint8_t *p) { uint64_t u = R64(p); double d; memcpy(&d, &u, 8); return d; }
static inline void W32(uint8_t *p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }

static void *arena_alloc(QGL *g, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    if (g->arena_used + n > g->arena_size) {
        /* Grows only between commands: pointers handed out in this one stay valid. */
        size_t want = g->arena_size ? g->arena_size : (size_t)4 << 20;
        while (want < g->arena_used + n)
            want *= 2;
        uint8_t *a = malloc(want);
        if (!a)
            return NULL;
        if (g->arena_used) {
            /* The old block stays alive until the command ends; keep it on a list. */
            static uint8_t *old[64];
            static unsigned nold;
            if (nold < 64)
                old[nold++] = g->arena;
            g->arena = a;
            g->arena_size = want;
            g->arena_used = 0;
        } else {
            free(g->arena);
            g->arena = a;
            g->arena_size = want;
        }
    }
    void *p = g->arena + g->arena_used;
    g->arena_used += n;
    return p;
}

static bool guest_ok(QGL *g, uint64_t a, uint64_t n) { return a && a <= g->ram_size && n <= g->ram_size - a; }

static void swap_copy(uint8_t *d, const uint8_t *s, size_t bytes, unsigned es)
{
    switch (es) {
    case 2: for (size_t i = 0; i + 2 <= bytes; i += 2) { d[i] = s[i + 1]; d[i + 1] = s[i]; } break;
    case 4: for (size_t i = 0; i + 4 <= bytes; i += 4) { uint32_t v; memcpy(&v, s + i, 4); v = __builtin_bswap32(v); memcpy(d + i, &v, 4); } break;
    case 8: for (size_t i = 0; i + 8 <= bytes; i += 8) { uint64_t v; memcpy(&v, s + i, 8); v = __builtin_bswap64(v); memcpy(d + i, &v, 8); } break;
    default: memcpy(d, s, bytes); break;
    }
}

/* An inline array from the stream, swapped. A few spare zero bytes cover GL reading more values than
 * the guest sent (a pname with four values where the guest gave one). */
static void *qgl_take_in(QGL *g, const uint8_t **in, const uint8_t *end, unsigned es)
{
    if (*in + 4 > end)
        return NULL;
    uint32_t bytes = R32(*in);
    *in += 4;
    if (bytes == 0xFFFFFFFFu)
        return NULL;
    size_t w = (bytes + 3u) / 4u;
    if (*in + w * 4 > end) {
        *in = end;
        return NULL;
    }
    uint8_t *d = arena_alloc(g, bytes + 128);
    if (!d)
        return NULL;
    memset(d + bytes, 0, 128);
    swap_copy(d, *in, bytes, es);
    *in += w * 4;
    return d;
}

static void *qgl_out(QGL *g, uint64_t ga, size_t n, unsigned es)
{
    if (!ga)
        return NULL;
    size_t bytes = n * es + 128;
    void *p = arena_alloc(g, bytes);
    if (p)
        memset(p, 0, bytes);
    return p;
}

static void qgl_out_done(QGL *g, uint64_t ga, const void *p, size_t n, unsigned es)
{
    if (!ga || !p || !guest_ok(g, ga, n * es))
        return;
    swap_copy(g->ram + ga, p, n * es, es);
}

static void *qgl_raw(QGL *g, uint64_t ga) { return ga && ga < g->ram_size ? g->ram + ga : NULL; }

static Buf *buf_find(QGL *g, uint32_t name)
{
    for (Buf *b = g->bufs[name & 255]; b; b = b->next)
        if (b->name == name)
            return b;
    return NULL;
}
static Buf *buf_get(QGL *g, uint32_t name)
{
    Buf *b = buf_find(g, name);
    if (!b && name) {
        b = calloc(1, sizeof *b);
        b->name = name;
        b->next = g->bufs[name & 255];
        g->bufs[name & 255] = b;
        if (name >= g->next_buf)
            g->next_buf = name + 1;
    }
    return b;
}

/* Pixel data: an offset into a bound pixel buffer, or guest memory. */
static void *qgl_pix(QGL *g, uint64_t ga, int pack)
{
    Ctx *c = g->cur;
    uint32_t pb = c ? (pack ? c->bind_pack : c->bind_unpack) : 0;
    if (pb) {
        Buf *b = buf_find(g, pb);
        if (!b || ga > b->size)
            return NULL;
        return b->data + ga;
    }
    return qgl_raw(g, ga);
}

static void qgl_bad_id(QGL *g, unsigned id)
{
    if (id < 4096 && !g->warned[id]) {
        g->warned[id] = true;
        fprintf(stderr, "[QRX-GL68K] call %u is not handled\n", id);
    }
}

#include "qrx_gl68k_gen.inc"

/* ---- buffer objects */
static uint32_t *bind_slot(Ctx *c, uint32_t target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return &c->bind_array;
    case GL_ELEMENT_ARRAY_BUFFER: return &c->bind_element;
    case GL_PIXEL_PACK_BUFFER: return &c->bind_pack;
    case GL_PIXEL_UNPACK_BUFFER: return &c->bind_unpack;
    default: return NULL;
    }
}
static Buf *bound(QGL *g, uint32_t target)
{
    uint32_t *s = g->cur ? bind_slot(g->cur, target) : NULL;
    return s && *s ? buf_find(g, *s) : NULL;
}
static void qglh_glBindBuffer(QGL *g, uint32_t target, uint32_t buffer)
{
    uint32_t *s = g->cur ? bind_slot(g->cur, target) : NULL;
    if (!s)
        return;
    *s = buffer;
    buf_get(g, buffer);
}
static void qglh_glBindBufferARB(QGL *g, uint32_t target, uint32_t buffer) { qglh_glBindBuffer(g, target, buffer); }
static void gen_buffers(QGL *g, int32_t n, uint64_t ga)
{
    if (n <= 0 || !guest_ok(g, ga, (uint64_t)n * 4))
        return;
    for (int32_t i = 0; i < n; i++) {
        if (!g->next_buf)
            g->next_buf = 1;
        while (buf_find(g, g->next_buf))
            g->next_buf++;
        uint32_t name = g->next_buf++;
        buf_get(g, name);
        W32(g->ram + ga + (uint64_t)i * 4, name);
    }
}
static void qglh_glGenBuffers(QGL *g, int32_t n, uint64_t buffers) { gen_buffers(g, n, buffers); }
static void qglh_glGenBuffersARB(QGL *g, int32_t n, uint64_t buffers) { gen_buffers(g, n, buffers); }
static void delete_buffers(QGL *g, int32_t n, uint64_t ga)
{
    if (n <= 0 || !guest_ok(g, ga, (uint64_t)n * 4))
        return;
    for (int32_t i = 0; i < n; i++) {
        uint32_t name = R32(g->ram + ga + (uint64_t)i * 4);
        if (!name)
            continue;
        for (Buf **p = &g->bufs[name & 255]; *p; p = &(*p)->next)
            if ((*p)->name == name) {
                Buf *b = *p;
                *p = b->next;
                free(b->data);
                free(b);
                break;
            }
        for (unsigned k = 0; k < MAX_CTX; k++) {
            Ctx *c = &g->ctx[k];
            if (!c->used)
                continue;
            if (c->bind_array == name) c->bind_array = 0;
            if (c->bind_element == name) c->bind_element = 0;
            if (c->bind_pack == name) c->bind_pack = 0;
            if (c->bind_unpack == name) c->bind_unpack = 0;
        }
    }
}
static void qglh_glDeleteBuffers(QGL *g, int32_t n, uint64_t buffers) { delete_buffers(g, n, buffers); }
static void qglh_glDeleteBuffersARB(QGL *g, int32_t n, uint64_t buffers) { delete_buffers(g, n, buffers); }
static uint8_t qglh_glIsBuffer(QGL *g, uint32_t buffer) { return buffer && buf_find(g, buffer) != NULL; }
static uint8_t qglh_glIsBufferARB(QGL *g, uint32_t buffer) { return qglh_glIsBuffer(g, buffer); }
static void buffer_data(QGL *g, uint32_t target, intptr_t size, uint64_t data, uint32_t usage)
{
    Buf *b = bound(g, target);
    if (!b || size < 0)
        return;
    uint8_t *d = realloc(b->data, size ? (size_t)size : 1);
    if (!d)
        return;
    b->data = d;
    b->size = (size_t)size;
    b->usage = usage;
    if (data && guest_ok(g, data, (uint64_t)size))
        memcpy(d, g->ram + data, (size_t)size);
    else
        memset(d, 0, (size_t)size);
}
static void qglh_glBufferData(QGL *g, uint32_t target, intptr_t size, uint64_t data, uint32_t usage) { buffer_data(g, target, size, data, usage); }
static void qglh_glBufferDataARB(QGL *g, uint32_t target, intptr_t size, uint64_t data, uint32_t usage) { buffer_data(g, target, size, data, usage); }
static void buffer_sub(QGL *g, uint32_t target, intptr_t off, intptr_t size, uint64_t data, bool get)
{
    Buf *b = bound(g, target);
    if (!b || off < 0 || size < 0 || (size_t)off + (size_t)size > b->size || !guest_ok(g, data, (uint64_t)size))
        return;
    if (get)
        memcpy(g->ram + data, b->data + off, (size_t)size);
    else
        memcpy(b->data + off, g->ram + data, (size_t)size);
}
static void qglh_glBufferSubData(QGL *g, uint32_t t, intptr_t o, intptr_t s, uint64_t d) { buffer_sub(g, t, o, s, d, false); }
static void qglh_glBufferSubDataARB(QGL *g, uint32_t t, intptr_t o, intptr_t s, uint64_t d) { buffer_sub(g, t, o, s, d, false); }
static void qglh_glGetBufferSubData(QGL *g, uint32_t t, intptr_t o, intptr_t s, uint64_t d) { buffer_sub(g, t, o, s, d, true); }
static void qglh_glGetBufferSubDataARB(QGL *g, uint32_t t, intptr_t o, intptr_t s, uint64_t d) { buffer_sub(g, t, o, s, d, true); }
static void buffer_param(QGL *g, uint32_t target, uint32_t pname, uint64_t params)
{
    Buf *b = bound(g, target);
    if (!guest_ok(g, params, 4))
        return;
    int32_t v = 0;
    switch (pname) {
    case GL_BUFFER_SIZE: v = b ? (int32_t)b->size : 0; break;
    case GL_BUFFER_USAGE: v = b ? (int32_t)b->usage : GL_STATIC_DRAW; break;
    case GL_BUFFER_ACCESS: v = GL_READ_WRITE; break;
    default: v = 0; break; /* GL_BUFFER_MAPPED: the guest library keeps maps itself */
    }
    W32(g->ram + params, (uint32_t)v);
}
static void qglh_glGetBufferParameteriv(QGL *g, uint32_t t, uint32_t p, uint64_t a) { buffer_param(g, t, p, a); }
static void qglh_glGetBufferParameterivARB(QGL *g, uint32_t t, uint32_t p, uint64_t a) { buffer_param(g, t, p, a); }

/* ---- client arrays */
static unsigned type_size(uint32_t type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT_ARB: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}
static void set_array(QGL *g, unsigned i, int size, uint32_t type, int stride, bool norm, uint64_t ptr)
{
    if (!g->cur || i >= A_COUNT)
        return;
    Array *a = &g->cur->cs.arr[i];
    a->size = size; a->type = type; a->stride = stride; a->norm = norm;
    a->buf = g->cur->bind_array;
    a->addr = ptr;
}
static void qglh_glVertexPointer(QGL *g, int32_t size, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_VERTEX, size, type, stride, false, p); }
static void qglh_glNormalPointer(QGL *g, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_NORMAL, 3, type, stride, true, p); }
static void qglh_glColorPointer(QGL *g, int32_t size, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_COLOR, size, type, stride, true, p); }
static void qglh_glSecondaryColorPointer(QGL *g, int32_t size, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_COLOR2, size, type, stride, true, p); }
static void qglh_glSecondaryColorPointerEXT(QGL *g, int32_t size, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_COLOR2, size, type, stride, true, p); }
static void qglh_glFogCoordPointer(QGL *g, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_FOG, 1, type, stride, false, p); }
static void qglh_glFogCoordPointerEXT(QGL *g, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_FOG, 1, type, stride, false, p); }
static void qglh_glIndexPointer(QGL *g, uint32_t type, int32_t stride, uint64_t p) { set_array(g, A_INDEX, 1, type, stride, false, p); }
static void qglh_glEdgeFlagPointer(QGL *g, int32_t stride, uint64_t p) { set_array(g, A_EDGE, 1, GL_UNSIGNED_BYTE, stride, false, p); }
static void qglh_glTexCoordPointer(QGL *g, int32_t size, uint32_t type, int32_t stride, uint64_t p)
{
    if (g->cur)
        set_array(g, A_TEX0 + g->cur->cs.client_tex, size, type, stride, false, p);
}
static void qglh_glVertexAttribPointer(QGL *g, uint32_t index, int32_t size, uint32_t type, uint8_t norm, int32_t stride, uint64_t p)
{
    if (index < MAX_ATTRIBS)
        set_array(g, A_ATTR0 + index, size, type, stride, norm, p);
}
static void qglh_glVertexAttribPointerARB(QGL *g, uint32_t index, int32_t size, uint32_t type, uint8_t norm, int32_t stride, uint64_t p)
{
    qglh_glVertexAttribPointer(g, index, size, type, norm, stride, p);
}
static int client_index(QGL *g, uint32_t cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: return A_VERTEX;
    case GL_NORMAL_ARRAY: return A_NORMAL;
    case GL_COLOR_ARRAY: return A_COLOR;
    case GL_SECONDARY_COLOR_ARRAY: return A_COLOR2;
    case GL_FOG_COORD_ARRAY: return A_FOG;
    case GL_INDEX_ARRAY: return A_INDEX;
    case GL_EDGE_FLAG_ARRAY: return A_EDGE;
    case GL_TEXTURE_COORD_ARRAY: return A_TEX0 + (int)g->cur->cs.client_tex;
    default: return -1;
    }
}
static void qglh_glEnableClientState(QGL *g, uint32_t cap)
{
    if (!g->cur) return;
    int i = client_index(g, cap);
    if (i >= 0) g->cur->cs.arr[i].on = true;
    glEnableClientState(cap);
}
static void qglh_glDisableClientState(QGL *g, uint32_t cap)
{
    if (!g->cur) return;
    int i = client_index(g, cap);
    if (i >= 0) g->cur->cs.arr[i].on = false;
    glDisableClientState(cap);
}
static void qglh_glClientActiveTexture(QGL *g, uint32_t tex)
{
    if (!g->cur) return;
    unsigned u = tex - GL_TEXTURE0;
    if (u < MAX_TEX_UNITS) g->cur->cs.client_tex = u;
    glClientActiveTexture(tex);
}
static void qglh_glClientActiveTextureARB(QGL *g, uint32_t tex) { qglh_glClientActiveTexture(g, tex); }
static void attrib_array(QGL *g, uint32_t index, bool on)
{
    if (!g->cur || index >= MAX_ATTRIBS) return;
    g->cur->cs.arr[A_ATTR0 + index].on = on;
    if (on) glEnableVertexAttribArray(index); else glDisableVertexAttribArray(index);
}
static void qglh_glEnableVertexAttribArray(QGL *g, uint32_t i) { attrib_array(g, i, true); }
static void qglh_glDisableVertexAttribArray(QGL *g, uint32_t i) { attrib_array(g, i, false); }
static void qglh_glEnableVertexAttribArrayARB(QGL *g, uint32_t i) { attrib_array(g, i, true); }
static void qglh_glDisableVertexAttribArrayARB(QGL *g, uint32_t i) { attrib_array(g, i, false); }
static void qglh_glPushClientAttrib(QGL *g, uint32_t mask)
{
    if (!g->cur) return;
    Ctx *c = g->cur;
    if (c->cs_depth < 16) c->cs_stack[c->cs_depth] = c->cs;
    c->cs_depth++;
    glPushClientAttrib(mask);
}
static void qglh_glPopClientAttrib(QGL *g)
{
    if (!g->cur) return;
    Ctx *c = g->cur;
    if (!c->cs_depth) return;
    c->cs_depth--;
    if (c->cs_depth < 16) c->cs = c->cs_stack[c->cs_depth];
    glPopClientAttrib();
}
static void qglh_glInterleavedArrays(QGL *g, uint32_t format, int32_t stride, uint64_t p)
{
    if (!g->cur) return;
    /* Sizes per the GL 2.1 specification, table 2.5: tex, color, normal, vertex; color type UNSIGNED_BYTE for C4UB. */
    int tc = 0, cc = 0, nn = 0, vc = 3, tsz = 0;
    uint32_t ctype = GL_FLOAT;
    bool tex4 = false;
    switch (format) {
    case GL_V2F: vc = 2; break;
    case GL_V3F: break;
    case GL_C4UB_V2F: cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 2; break;
    case GL_C4UB_V3F: cc = 4; ctype = GL_UNSIGNED_BYTE; break;
    case GL_C3F_V3F: cc = 3; break;
    case GL_N3F_V3F: nn = 1; break;
    case GL_C4F_N3F_V3F: cc = 4; nn = 1; break;
    case GL_T2F_V3F: tc = 2; break;
    case GL_T4F_V4F: tc = 4; vc = 4; tex4 = true; break;
    case GL_T2F_C4UB_V3F: tc = 2; cc = 4; ctype = GL_UNSIGNED_BYTE; break;
    case GL_T2F_C3F_V3F: tc = 2; cc = 3; break;
    case GL_T2F_N3F_V3F: tc = 2; nn = 1; break;
    case GL_T2F_C4F_N3F_V3F: tc = 2; cc = 4; nn = 1; break;
    case GL_T4F_C4F_N3F_V4F: tc = 4; cc = 4; nn = 1; vc = 4; tex4 = true; break;
    default: return;
    }
    (void)tex4;
    tsz = tc * 4;
    int csz = cc ? (ctype == GL_UNSIGNED_BYTE ? 4 : cc * 4) : 0;
    int nsz = nn ? 12 : 0;
    int total = tsz + csz + nsz + vc * 4;
    if (!stride) stride = total;
    Ctx *c = g->cur;
    uint64_t off = p;
    c->cs.arr[A_TEX0 + c->cs.client_tex].on = tc != 0;
    if (tc) set_array(g, A_TEX0 + c->cs.client_tex, tc, GL_FLOAT, stride, false, off);
    off += (uint64_t)tsz;
    c->cs.arr[A_COLOR].on = cc != 0;
    if (cc) set_array(g, A_COLOR, cc, ctype, stride, true, off);
    off += (uint64_t)csz;
    c->cs.arr[A_NORMAL].on = nn != 0;
    if (nn) set_array(g, A_NORMAL, 3, GL_FLOAT, stride, true, off);
    off += (uint64_t)nsz;
    c->cs.arr[A_VERTEX].on = true;
    set_array(g, A_VERTEX, vc, GL_FLOAT, stride, false, off);
    c->cs.arr[A_INDEX].on = c->cs.arr[A_EDGE].on = false;
    if (tc) glEnableClientState(GL_TEXTURE_COORD_ARRAY); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (cc) glEnableClientState(GL_COLOR_ARRAY); else glDisableClientState(GL_COLOR_ARRAY);
    if (nn) glEnableClientState(GL_NORMAL_ARRAY); else glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_INDEX_ARRAY);
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
}

/* Converts vertices lo..hi of every enabled array into host client arrays. */
static bool arrays_prepare(QGL *g, int64_t lo, int64_t hi)
{
    Ctx *c = g->cur;
    if (!c || lo < 0 || hi < lo || hi - lo > (1 << 24))
        return false;
    size_t n = (size_t)(hi - lo + 1);
    for (unsigned i = 0; i < A_COUNT; i++) {
        Array *a = &c->cs.arr[i];
        if (!a->on)
            continue;
        unsigned ts = type_size(a->type);
        int comps = a->size == GL_BGRA ? 4 : a->size;
        size_t es = (size_t)comps * ts;
        size_t stride = a->stride ? (size_t)a->stride : es;
        const uint8_t *src;
        uint64_t avail;
        if (a->buf) {
            Buf *b = buf_find(g, a->buf);
            if (!b || a->addr > b->size)
                return false;
            src = b->data + a->addr;
            avail = b->size - a->addr;
        } else {
            if (!a->addr || a->addr >= g->ram_size)
                return false;
            src = g->ram + a->addr;
            avail = g->ram_size - a->addr;
        }
        if ((uint64_t)hi * stride + es > avail)
            return false;
        uint8_t *d = arena_alloc(g, n * es + 16);
        if (!d)
            return false;
        for (size_t k = 0; k < n; k++)
            swap_copy(d + k * es, src + (size_t)(lo + (int64_t)k) * stride, es, ts);
        const uint8_t *hp = d - (size_t)lo * es;
        switch (i) {
        case A_VERTEX: glVertexPointer(a->size, a->type, 0, hp); break;
        case A_NORMAL: glNormalPointer(a->type, 0, hp); break;
        case A_COLOR: glColorPointer(a->size, a->type, 0, hp); break;
        case A_COLOR2: glSecondaryColorPointer(a->size, a->type, 0, hp); break;
        case A_FOG: glFogCoordPointer(a->type, 0, hp); break;
        case A_INDEX: glIndexPointer(a->type, 0, hp); break;
        case A_EDGE: glEdgeFlagPointer(0, hp); break;
        default:
            if (i >= A_ATTR0) {
                glVertexAttribPointer(i - A_ATTR0, a->size, a->type, a->norm, 0, hp);
            } else {
                glClientActiveTexture(GL_TEXTURE0 + (i - A_TEX0));
                glTexCoordPointer(a->size, a->type, 0, hp);
                glClientActiveTexture(GL_TEXTURE0 + c->cs.client_tex);
            }
            break;
        }
    }
    return true;
}
static void qglh_glDrawArrays(QGL *g, uint32_t mode, int32_t first, int32_t count)
{
    if (count <= 0 || !arrays_prepare(g, first, (int64_t)first + count - 1))
        return;
    glDrawArrays(mode, first, count);
}
static void qglh_glMultiDrawArrays(QGL *g, uint32_t mode, const int32_t *first, const int32_t *count, int32_t n)
{
    for (int32_t i = 0; first && count && i < n; i++)
        qglh_glDrawArrays(g, mode, first[i], count[i]);
}
static void qglh_glMultiDrawArraysEXT(QGL *g, uint32_t mode, const int32_t *first, const int32_t *count, int32_t n) { qglh_glMultiDrawArrays(g, mode, first, count, n); }
static void qglh_glArrayElement(QGL *g, int32_t i)
{
    if (arrays_prepare(g, i, i))
        glArrayElement(i);
}
/* Indices into host memory, swapped, with their range. */
static void *indices_get(QGL *g, int32_t count, uint32_t type, uint64_t ga, int64_t *lo, int64_t *hi)
{
    unsigned ts = type_size(type);
    size_t bytes = (size_t)count * ts;
    const uint8_t *src;
    if (g->cur && g->cur->bind_element) {
        Buf *b = buf_find(g, g->cur->bind_element);
        if (!b || ga > b->size || bytes > b->size - ga)
            return NULL;
        src = b->data + ga;
    } else {
        if (!guest_ok(g, ga, bytes))
            return NULL;
        src = g->ram + ga;
    }
    uint8_t *d = arena_alloc(g, bytes + 16);
    if (!d)
        return NULL;
    swap_copy(d, src, bytes, ts);
    int64_t mn = INT64_MAX, mx = -1;
    for (int32_t i = 0; i < count; i++) {
        int64_t v = ts == 1 ? d[i] : ts == 2 ? ((uint16_t *)d)[i] : ((uint32_t *)d)[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    *lo = mn; *hi = mx;
    return d;
}
static void qglh_glDrawElements(QGL *g, uint32_t mode, int32_t count, uint32_t type, uint64_t indices)
{
    int64_t lo, hi;
    if (count <= 0)
        return;
    void *idx = indices_get(g, count, type, indices, &lo, &hi);
    if (idx && arrays_prepare(g, lo, hi))
        glDrawElements(mode, count, type, idx);
}
static void qglh_glDrawRangeElements(QGL *g, uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, uint64_t indices)
{
    (void)start; (void)end;
    qglh_glDrawElements(g, mode, count, type, indices);
}
static void qglh_glDrawRangeElementsEXT(QGL *g, uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, uint64_t indices)
{
    qglh_glDrawRangeElements(g, mode, start, end, count, type, indices);
}
static void qglh_glCallLists(QGL *g, int32_t n, uint32_t type, uint64_t lists)
{
    unsigned es = 1, ts = 1;
    switch (type) {
    case GL_SHORT: case GL_UNSIGNED_SHORT: es = ts = 2; break;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: es = ts = 4; break;
    case GL_2_BYTES: es = 2; break;
    case GL_3_BYTES: es = 3; break;
    case GL_4_BYTES: es = 4; break;
    default: break;
    }
    size_t bytes = (size_t)(n > 0 ? n : 0) * es;
    if (!bytes || !guest_ok(g, lists, bytes))
        return;
    uint8_t *d = arena_alloc(g, bytes);
    if (!d)
        return;
    swap_copy(d, g->ram + lists, bytes, ts);
    glCallLists(n, type, d);
}

/* ---- the window's framebuffer */
static uint32_t map_buffer_enum(Ctx *c, uint32_t mode, bool draw)
{
    if ((draw ? c->draw_fb : c->read_fb) != 0)
        return mode;
    switch (mode) {
    case GL_FRONT: case GL_BACK: case GL_FRONT_LEFT: case GL_BACK_LEFT: case GL_LEFT: case GL_FRONT_AND_BACK:
        return GL_COLOR_ATTACHMENT0_EXT;
    default:
        return mode;
    }
}
static void bind_fb(QGL *g, uint32_t target, uint32_t fb)
{
    Ctx *c = g->cur;
    if (!c) return;
    if (target == GL_FRAMEBUFFER_EXT || target == GL_DRAW_FRAMEBUFFER_EXT) c->draw_fb = fb;
    if (target == GL_FRAMEBUFFER_EXT || target == GL_READ_FRAMEBUFFER_EXT) c->read_fb = fb;
    glBindFramebufferEXT(target, fb ? fb : c->fbo);
    /* The window's draw and read buffers come back with it. */
    if (!fb && (target == GL_FRAMEBUFFER_EXT || target == GL_DRAW_FRAMEBUFFER_EXT))
        glDrawBuffer(c->draw_buf == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT);
    if (!fb && (target == GL_FRAMEBUFFER_EXT || target == GL_READ_FRAMEBUFFER_EXT))
        glReadBuffer(c->read_buf == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT);
}
static void qglh_glBindFramebuffer(QGL *g, uint32_t target, uint32_t fb) { bind_fb(g, target, fb); }
static void qglh_glBindFramebufferEXT(QGL *g, uint32_t target, uint32_t fb) { bind_fb(g, target, fb); }
static void qglh_glDrawBuffer(QGL *g, uint32_t mode)
{
    if (!g->cur) return;
    if (!g->cur->draw_fb) g->cur->draw_buf = mode;
    glDrawBuffer(map_buffer_enum(g->cur, mode, true));
}
static void qglh_glReadBuffer(QGL *g, uint32_t mode)
{
    if (!g->cur) return;
    if (!g->cur->read_fb) g->cur->read_buf = mode;
    glReadBuffer(map_buffer_enum(g->cur, mode, false));
}
static void qglh_glDrawBuffers(QGL *g, int32_t n, const uint32_t *bufs)
{
    GLenum b[16];
    if (!g->cur || !bufs || n <= 0 || n > 16) return;
    for (int32_t i = 0; i < n; i++) b[i] = map_buffer_enum(g->cur, bufs[i], true);
    glDrawBuffers(n, b);
}
static void qglh_glDrawBuffersARB(QGL *g, int32_t n, const uint32_t *bufs) { qglh_glDrawBuffers(g, n, bufs); }

/* ---- state queries and the pixel store */
static void qglh_glPixelStorei(QGL *g, uint32_t pname, int32_t v)
{
    if (g->cur && pname == GL_UNPACK_SWAP_BYTES) { g->cur->cs.unpack_swap = v != 0; v = !v; }
    else if (g->cur && pname == GL_PACK_SWAP_BYTES) { g->cur->cs.pack_swap = v != 0; v = !v; }
    glPixelStorei(pname, v);
}
static void qglh_glPixelStoref(QGL *g, uint32_t pname, float v) { qglh_glPixelStorei(g, pname, (int32_t)v); }
/* Values this layer answers itself; false when the host's answer is right. */
static bool own_state(QGL *g, uint32_t pname, double *v)
{
    Ctx *c = g->cur;
    if (!c) return false;
    switch (pname) {
    case GL_FRAMEBUFFER_BINDING_EXT: *v = c->draw_fb; return true;
    case GL_READ_FRAMEBUFFER_BINDING_EXT: *v = c->read_fb; return true;
    case GL_ARRAY_BUFFER_BINDING: *v = c->bind_array; return true;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = c->bind_element; return true;
    case GL_PIXEL_PACK_BUFFER_BINDING: *v = c->bind_pack; return true;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: *v = c->bind_unpack; return true;
    case GL_UNPACK_SWAP_BYTES: *v = c->cs.unpack_swap; return true;
    case GL_PACK_SWAP_BYTES: *v = c->cs.pack_swap; return true;
    case GL_DOUBLEBUFFER: *v = 1; return true;
    case GL_DRAW_BUFFER: if (c->draw_fb) return false; *v = c->draw_buf; return true;
    case GL_READ_BUFFER: if (c->read_fb) return false; *v = c->read_buf; return true;
    case GL_CLIENT_ACTIVE_TEXTURE: *v = GL_TEXTURE0 + c->cs.client_tex; return true;
    default: return false;
    }
}
static unsigned state_count(uint32_t pname)
{
    if (pname == GL_COMPRESSED_TEXTURE_FORMATS) {
        GLint n = 0;
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
        return n > 0 ? (unsigned)n : 1;
    }
    return qgl_pn(pname);
}
static void get_state(QGL *g, uint32_t pname, uint64_t ga, int kind) /* 0 int, 1 float, 2 double, 3 boolean */
{
    double own;
    static const unsigned es_of[4] = {4, 4, 8, 1};
    unsigned es = es_of[kind];
    if (own_state(g, pname, &own)) {
        if (!guest_ok(g, ga, es)) return;
        uint8_t *p = g->ram + ga;
        if (kind == 0) W32(p, (uint32_t)(int32_t)own);
        else if (kind == 1) { float f = (float)own; uint32_t u; memcpy(&u, &f, 4); W32(p, u); }
        else if (kind == 2) { uint64_t u; memcpy(&u, &own, 8); u = __builtin_bswap64(u); memcpy(p, &u, 8); }
        else *p = own != 0;
        return;
    }
    unsigned n = state_count(pname);
    uint8_t *tmp = arena_alloc(g, (size_t)n * 8 + 128);
    if (!tmp) return;
    memset(tmp, 0, (size_t)n * 8 + 128);
    if (kind == 0) glGetIntegerv(pname, (GLint *)tmp);
    else if (kind == 1) glGetFloatv(pname, (GLfloat *)tmp);
    else if (kind == 2) glGetDoublev(pname, (GLdouble *)tmp);
    else glGetBooleanv(pname, (GLboolean *)tmp);
    qgl_out_done(g, ga, tmp, n, es);
}
static void qglh_glGetIntegerv(QGL *g, uint32_t pname, uint64_t p) { get_state(g, pname, p, 0); }
static void qglh_glGetFloatv(QGL *g, uint32_t pname, uint64_t p) { get_state(g, pname, p, 1); }
static void qglh_glGetDoublev(QGL *g, uint32_t pname, uint64_t p) { get_state(g, pname, p, 2); }
static void qglh_glGetBooleanv(QGL *g, uint32_t pname, uint64_t p) { get_state(g, pname, p, 3); }
static void get_attrib(QGL *g, uint32_t index, uint32_t pname, uint64_t ga)
{
    if (!g->cur || !guest_ok(g, ga, 4)) return;
    Array *a = index < MAX_ATTRIBS ? &g->cur->cs.arr[A_ATTR0 + index] : NULL;
    if (a && pname == GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) { W32(g->ram + ga, a->buf); return; }
    if (a && pname == GL_VERTEX_ATTRIB_ARRAY_STRIDE) { W32(g->ram + ga, (uint32_t)a->stride); return; }
    GLint v[4] = {0};
    glGetVertexAttribiv(index, pname, v);
    qgl_out_done(g, ga, v, pname == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1, 4);
}
static void qglh_glGetVertexAttribiv(QGL *g, uint32_t i, uint32_t p, uint64_t a) { get_attrib(g, i, p, a); }
static void qglh_glGetVertexAttribivARB(QGL *g, uint32_t i, uint32_t p, uint64_t a) { get_attrib(g, i, p, a); }

/* ---- selection and feedback: GL writes into host memory, copied out when the mode ends. */
static void qglh_glSelectBuffer(QGL *g, int32_t size, uint64_t buffer)
{
    Ctx *c = g->cur;
    if (!c || size <= 0) return;
    free(c->select_host);
    c->select_host = calloc((size_t)size, 4);
    c->select_size = size;
    c->select_guest = buffer;
    glSelectBuffer(size, c->select_host);
}
static void qglh_glFeedbackBuffer(QGL *g, int32_t size, uint32_t type, uint64_t buffer)
{
    Ctx *c = g->cur;
    if (!c || size <= 0) return;
    free(c->feedback_host);
    c->feedback_host = calloc((size_t)size, 4);
    c->feedback_size = size;
    c->feedback_guest = buffer;
    glFeedbackBuffer(size, type, c->feedback_host);
}
static int32_t qglh_glRenderMode(QGL *g, uint32_t mode)
{
    Ctx *c = g->cur;
    if (!c) return 0;
    int32_t r = glRenderMode(mode);
    if (c->render_mode == GL_SELECT && c->select_host)
        qgl_out_done(g, c->select_guest, c->select_host, (size_t)c->select_size, 4);
    if (c->render_mode == GL_FEEDBACK && c->feedback_host)
        qgl_out_done(g, c->feedback_guest, c->feedback_host, (size_t)c->feedback_size, 4);
    c->render_mode = mode;
    return r;
}

/* ---- contexts */
static bool fbo_size(Ctx *c, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 8192 || h > 8192) return false;
    if (c->fbo && c->w == w && c->h == h) return true;
    if (!c->fbo) {
        glGenFramebuffersEXT(1, &c->fbo);
        glGenRenderbuffersEXT(1, &c->rb_color);
        glGenRenderbuffersEXT(1, &c->rb_depth);
    }
    GLint old_rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &old_rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->rb_color);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->rb_depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, (GLuint)old_rb);
    GLint old_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &old_fb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->fbo);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, c->rb_color);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->rb_depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->rb_depth);
    GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT)
        fprintf(stderr, "[QRX-GL68K] window framebuffer %dx%d incomplete: 0x%x\n", w, h, st);
    if (old_fb && (GLuint)old_fb != c->fbo)
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)old_fb);
    c->w = w;
    c->h = h;
    return st == GL_FRAMEBUFFER_COMPLETE_EXT;
}

/* ---- native contexts: CGL on the Mac, EGL on Linux */
#if defined(__APPLE__)
static QglNative qgl_native_create(QglNative share)
{
    CGLPixelFormatAttribute at[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, kCGLPFAAccelerated,
                                    kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, kCGLPFADepthSize, 24, kCGLPFAStencilSize, 8, 0};
    CGLPixelFormatObj pf;
    GLint npf = 0;
    if (CGLChoosePixelFormat(at, &pf, &npf) != kCGLNoError || !pf) {
        fprintf(stderr, "[QRX-GL68K] no OpenGL 2.1 pixel format\n");
        return NULL;
    }
    CGLContextObj cgl = NULL;
    CGLError e = CGLCreateContext(pf, share, &cgl);
    CGLReleasePixelFormat(pf);
    if (e != kCGLNoError) {
        fprintf(stderr, "[QRX-GL68K] CGLCreateContext: %d\n", e);
        return NULL;
    }
    return cgl;
}
static void qgl_native_current(QglNative c) { CGLSetCurrentContext(c); }
static QglNative qgl_native_get(void) { return CGLGetCurrentContext(); }
static void qgl_native_destroy(QglNative c) { CGLDestroyContext(c); }
#else
static EGLDisplay qgl_dpy = EGL_NO_DISPLAY;
static EGLConfig qgl_cfg;
static bool qgl_egl_init(void)
{
    if (qgl_dpy != EGL_NO_DISPLAY) return true;
    /* Surfaceless first (no window system needed: console or KMS), then the default display. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    EGLint major, minor, n = 0;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
            fprintf(stderr, "[QRX-GL68K] no EGL display\n");
            return false;
        }
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[QRX-GL68K] EGL has no desktop OpenGL\n");
        return false;
    }
    const EGLint at[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    if (!eglChooseConfig(d, at, &qgl_cfg, 1, &n) || n < 1) {
        fprintf(stderr, "[QRX-GL68K] no EGL config for OpenGL\n");
        return false;
    }
    qgl_dpy = d;
    return true;
}
static QglNative qgl_native_create(QglNative share)
{
    if (!qgl_egl_init()) return NULL;
    /* the legacy (compatibility) profile, GLSL 1.20, like kCGLOGLPVersion_Legacy */
    const EGLint at[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLContext c = eglCreateContext(qgl_dpy, qgl_cfg, share ? share : EGL_NO_CONTEXT, at);
    if (c == EGL_NO_CONTEXT) {
        fprintf(stderr, "[QRX-GL68K] eglCreateContext: 0x%x\n", eglGetError());
        return NULL;
    }
    return c;
}
static void qgl_native_current(QglNative c)
{
    if (qgl_dpy != EGL_NO_DISPLAY) eglMakeCurrent(qgl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c ? c : EGL_NO_CONTEXT);
}
static QglNative qgl_native_get(void)
{
    EGLContext c = eglGetCurrentContext();
    return c == EGL_NO_CONTEXT ? NULL : c;
}
static void qgl_native_destroy(QglNative c) { eglDestroyContext(qgl_dpy, c); }
#endif

static uint32_t op_create(QGL *g, uint32_t share)
{
    unsigned i;
    for (i = 1; i < MAX_CTX && g->ctx[i].used; i++) {}
    if (i == MAX_CTX) return 0;
    /* All contexts share one object space, as GHOST's windows expect. */
    QglNative sh = share && share < MAX_CTX && g->ctx[share].used ? g->ctx[share].cgl : g->share;
    QglNative cgl = qgl_native_create(sh);
    if (!cgl) return 0;
    if (!g->share) g->share = cgl;
    Ctx *c = &g->ctx[i];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->cgl = cgl;
    c->draw_buf = c->read_buf = GL_BACK;
    c->render_mode = GL_RENDER;
    if (!g->resolved) {
        qgl_resolve();
        g->resolved = true;
    }
    return i;
}

static uint32_t op_makecurrent(QGL *g, uint32_t id, int w, int h)
{
    if (!id || id >= MAX_CTX || !g->ctx[id].used) {
        qgl_native_current(NULL);
        g->cur = NULL;
        return id == 0;
    }
    Ctx *c = &g->ctx[id];
    if (qgl_native_get() != c->cgl)
        qgl_native_current(c->cgl);
    g->cur = c;
    if (!fbo_size(c, w, h))
        return 0;
    if (!c->started) {
        c->started = true;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->fbo);
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
        /* The guest's pixel data is big-endian; GL swaps it while the guest sees the store as off. */
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
        /* A drawable sets these on first use; an FBO does not. */
        glViewport(0, 0, c->w, c->h);
        glScissor(0, 0, c->w, c->h);
    }
    return 1;
}

static void op_destroy(QGL *g, uint32_t id)
{
    if (!id || id >= MAX_CTX || !g->ctx[id].used) return;
    Ctx *c = &g->ctx[id];
    if (g->cur == c) {
        qgl_native_current(NULL);
        g->cur = NULL;
    }
    qgl_native_current(c->cgl);
    if (c->fbo) {
        glDeleteFramebuffersEXT(1, &c->fbo);
        glDeleteRenderbuffersEXT(1, &c->rb_color);
        glDeleteRenderbuffersEXT(1, &c->rb_depth);
    }
    qgl_native_current(g->cur ? g->cur->cgl : NULL);
    /* The first context holds the share group; it stays until the engine ends. */
    if (c->cgl != g->share)
        qgl_native_destroy(c->cgl);
    free(c->select_host);
    free(c->feedback_host);
    memset(c, 0, sizeof *c);
}

/* QRX_GL68K_CAPTURE=path: every presented frame as path.NNNN (PPM), up to QRX_GL68K_CAPTURE_MAX (50).
 * Shows what a 68k program drew, independent of the guest's screen. */
static void capture_frame(QGL *g, uint64_t src, uint32_t bpr, int w, int h)
{
    static unsigned n;
    const char *path = getenv("QRX_GL68K_CAPTURE");
    const char *mx = getenv("QRX_GL68K_CAPTURE_MAX");
    if (!path || n >= (mx ? (unsigned)atoi(mx) : 50u))
        return;
    char name[1024];
    snprintf(name, sizeof name, "%s.%04u", path, n++);
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = g->ram + src + (uint64_t)y * bpr;
        for (int x = 0; x < w; x++)
            fwrite(row + x * 4 + 1, 1, 3, f);
    }
    fclose(f);
}

/* The window's frame into guest memory as A R G B bytes, top row first. */
static void op_swap(QGL *g, uint64_t dst, uint32_t bpr, int w, int h)
{
    Ctx *c = g->cur;
    if (!c || w <= 0 || h <= 0 || w > c->w || h > c->h || bpr < (uint32_t)w * 4)
        return;
    if (!guest_ok(g, dst, (uint64_t)bpr * (uint64_t)(h - 1) + (uint64_t)w * 4))
        return;
    GLint read_fb = 0, read_buf = 0, align, row, skip_r, skip_p, swap, pack_buf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_r);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_p);
    glGetIntegerv(GL_PACK_SWAP_BYTES, &swap);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, c->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    if (pack_buf) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    uint8_t *tmp = arena_alloc(g, (size_t)w * (size_t)h * 4);
    if (tmp) {
        /* BGRA with 8_8_8_8 puts A R G B in memory on a little-endian host. */
        glReadPixels(0, c->h - h, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, tmp);
        for (int y = 0; y < h; y++)
            memcpy(g->ram + dst + (uint64_t)y * bpr, tmp + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
        capture_frame(g, dst, bpr, w, h);
    }
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, (GLuint)read_fb);
    glReadBuffer((GLenum)read_buf);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_ROW_LENGTH, row);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_r);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_p);
    glPixelStorei(GL_PACK_SWAP_BYTES, swap);
    if (pack_buf) glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
}

static uint32_t op_getstring(QGL *g, uint32_t name, uint64_t dst, uint32_t size)
{
    if (!g->cur) return 0xFFFFFFFFu;
    const char *s = (const char *)glGetString(name);
    if (!s) return 0xFFFFFFFFu;
    size_t len = strlen(s);
    if (size && guest_ok(g, dst, size)) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(g->ram + dst, s, n);
        g->ram[dst + n] = 0;
    }
    return (uint32_t)len;
}

static uint64_t op_getptr(QGL *g, uint32_t kind, uint32_t index, uint32_t pname)
{
    Ctx *c = g->cur;
    if (!c) return 0;
    if (kind == 1)
        return index < MAX_ATTRIBS && pname == GL_VERTEX_ATTRIB_ARRAY_POINTER ? c->cs.arr[A_ATTR0 + index].addr : 0;
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: return c->cs.arr[A_VERTEX].addr;
    case GL_NORMAL_ARRAY_POINTER: return c->cs.arr[A_NORMAL].addr;
    case GL_COLOR_ARRAY_POINTER: return c->cs.arr[A_COLOR].addr;
    case GL_SECONDARY_COLOR_ARRAY_POINTER: return c->cs.arr[A_COLOR2].addr;
    case GL_FOG_COORD_ARRAY_POINTER: return c->cs.arr[A_FOG].addr;
    case GL_INDEX_ARRAY_POINTER: return c->cs.arr[A_INDEX].addr;
    case GL_EDGE_FLAG_ARRAY_POINTER: return c->cs.arr[A_EDGE].addr;
    case GL_TEXTURE_COORD_ARRAY_POINTER: return c->cs.arr[A_TEX0 + c->cs.client_tex].addr;
    case GL_SELECTION_BUFFER_POINTER: return c->select_guest;
    case GL_FEEDBACK_BUFFER_POINTER: return c->feedback_guest;
    default: return 0;
    }
}

static void run_op(QGL *g, unsigned id, const uint8_t *a, const uint8_t *end)
{
    size_t n = (size_t)(end - a);
    switch (id) {
    case OP_CREATE: if (n >= 4) g->ret = op_create(g, R32(a)); break;
    case OP_MAKECURRENT: if (n >= 12) g->ret = op_makecurrent(g, R32(a), (int)R32(a + 4), (int)R32(a + 8)); break;
    case OP_DESTROY: if (n >= 4) op_destroy(g, R32(a)); break;
    case OP_SWAP: if (n >= 20) op_swap(g, R64(a), R32(a + 8), (int)R32(a + 12), (int)R32(a + 16)); break;
    case OP_GETSTRING: if (n >= 16) g->ret = op_getstring(g, R32(a), R64(a + 4), R32(a + 12)); break;
    case OP_SHADERSOURCE:
        if (n >= 12 && g->cur) {
            uint32_t len = R32(a + 8);
            if (len <= n - 12) {
                const GLchar *s = (const GLchar *)(a + 12);
                GLint l = (GLint)len;
                if (R32(a + 4)) glShaderSourceARB((GLhandleARB)(uintptr_t)R32(a), 1, &s, &l);
                else glShaderSource(R32(a), 1, &s, &l);
            }
        }
        break;
    case OP_BUFSIZE: if (n >= 4) { Buf *b = bound(g, R32(a)); g->ret = b ? ((uint64_t)b->name << 32) | (uint32_t)b->size : 0; } break;
    case OP_BUFREAD:
        if (n >= 28) {
            Buf *b = bound(g, R32(a));
            uint64_t off = R64(a + 4), size = R64(a + 12), dst = R64(a + 20);
            if (b && off <= b->size && size <= b->size - off && guest_ok(g, dst, size))
                memcpy(g->ram + dst, b->data + off, size);
        }
        break;
    case OP_GETPTR: if (n >= 12) g->ret = op_getptr(g, R32(a), R32(a + 4), R32(a + 8)); break;
    default: qgl_bad_id(g, id); break;
    }
}

void qrx_gl68k_service(uint8_t *ram, uint64_t ram_size, uint8_t *q)
{
    QGL *g = &G;
    /* On the 68100EC64 each core is a host thread: the current context follows the caller. */
    if (g->cur && qgl_native_get() != g->cur->cgl)
        qgl_native_current(g->cur->cgl);
    g->ram = ram;
    g->ram_size = ram_size;
    g->ret = 0;
    uint64_t addr = ((uint64_t)R32(q + 16) << 32) | R32(q + 20);
    uint32_t bytes = R32(q + 24);
    if (R32(q + 12) != 1 || !guest_ok(g, addr, bytes))
        return;
    const uint8_t *p = ram + addr, *end = p + bytes;
    g->streams++;
    while (p + 4 <= end) {
        uint32_t h = R32(p);
        unsigned id = h >> 20, nw = h & 0xFFFFFu;
        const uint8_t *a = p + 4, *e = a + (size_t)nw * 4;
        if (e > end)
            break;
        g->arena_used = 0;
        g->calls++;
        /* QRX_GL68K_TRACE=n: the first n calls with their argument words on stderr. */
        static long trace = -1;
        if (trace < 0) { const char *t = getenv("QRX_GL68K_TRACE"); trace = t ? atol(t) : 0; }
        if (trace > 0) {
            trace--;
            fprintf(stderr, "[GL] %s", id >= 16 && id < QGL_NFUNCS && qgl_id_names[id] ? qgl_id_names[id] : "op");
            for (unsigned i = 0; i < nw && i < 8; i++) fprintf(stderr, " %x", R32(a + i * 4));
            fprintf(stderr, "\n");
        }
        if (id < 16)
            run_op(g, id, a, e);
        else if (g->cur)
            qgl_dispatch(g, id, a, e);
        p = e;
    }
    g->arena_used = 0;
    W32(q + 32, (uint32_t)(g->ret >> 32));
    W32(q + 36, (uint32_t)g->ret);
}

void qrx_gl68k_report(void)
{
    if (G.streams)
        fprintf(stderr, "[QRX-GL68K] streams=%llu calls=%llu\n", (unsigned long long)G.streams, (unsigned long long)G.calls);
}
