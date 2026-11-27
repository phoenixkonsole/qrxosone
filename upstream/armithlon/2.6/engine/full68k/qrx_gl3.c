/* OpenGL 3.3 core for QRX OS One 64: HOST_CALL service 13 runs the command stream of the guest library
 * tools/qrxgl68k/guest/qgl3.c on an OpenGL 3.3 core context of the host (EGL surfaceless on Linux,
 * CGL 3.2 core on the Mac, which gives 4.1). It is what WebRender draws with.
 *
 * Unlike the GL 2.1 door (qrx_gl68k.c) nothing is converted behind the program's back: buffer data is
 * passed through as bytes, so a program brings vertex, instance and index data in the host's byte
 * order (little endian; WebRender does on QRX OS One 64). Values in the stream and arrays given to
 * calls are swapped as they are read. Pixel transfers run with the swap-bytes pixel store on, which
 * makes texel data of every type big endian for the guest, from its memory and from pixel buffers.
 *
 * Framebuffer 0 of a context is a host framebuffer object of the size the guest asks for (OP_RESIZE);
 * OP_READFRAME puts it into guest memory as A R G B bytes, top row first.
 *
 * EGL lets a context be current on one thread only, and the guest's cores are host threads, so a
 * worker thread of its own owns every context: a call hands the request block over and waits. */
#define GL_SILENCE_DEPRECATION
#include "qrx_gl3.h"
#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
typedef CGLContextObj QglNative;
#define QGL3_PROC(name) ((void *)dlsym(RTLD_DEFAULT, name))
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
typedef EGLContext QglNative;
#define QGL3_PROC(name) ((void *)eglGetProcAddress(name))
#endif
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_CREATE = 1, OP_DESTROY, OP_RESIZE, OP_READFRAME, OP_GETSTRING, OP_SHADERSOURCE, OP_GETERROR };
#define MAX_CTX 32

typedef struct {
    bool used, started, sized;  /* sized: the guest gave the window's size */
    QglNative nat;
    GLuint fbo, rb_color, rb_depth, vao;
    int w, h;
} Ctx;

typedef struct QGL3 {
    uint8_t *ram;
    uint64_t ram_size;
    uint64_t ret;
    Ctx ctx[MAX_CTX];
    Ctx *cur;
    QglNative share;
    uint8_t *arena;
    size_t arena_size, arena_used;
    uint32_t error;          /* the first error the door itself raised since the last glGetError */
    bool resolved;
    bool warned[4096];
    uint64_t calls, streams;
} QGL3;
static QGL3 G3;

static inline uint32_t R32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint64_t R64(const uint8_t *p) { return ((uint64_t)R32(p) << 32) | R32(p + 4); }
static inline float RF(const uint8_t *p) { uint32_t u = R32(p); float f; memcpy(&f, &u, 4); return f; }
static inline double RD(const uint8_t *p) { uint64_t u = R64(p); double d; memcpy(&d, &u, 8); return d; }
static inline void W32(uint8_t *p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }

static void raise_error(QGL3 *g, uint32_t e) { if (!g->error) g->error = e; }

static void *arena_alloc(QGL3 *g, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    if (g->arena_used + n > g->arena_size) {
        size_t want = g->arena_size ? g->arena_size : (size_t)4 << 20;
        while (want < g->arena_used + n)
            want *= 2;
        uint8_t *a = malloc(want);
        if (!a)
            return NULL;
        if (g->arena_used) {
            /* pointers handed out for this call stay valid: the old block lives on */
            static uint8_t *old[64];
            static unsigned nold;
            if (nold < 64) old[nold++] = g->arena;
        } else {
            free(g->arena);
        }
        g->arena = a;
        g->arena_size = want;
        g->arena_used = 0;
    }
    void *p = g->arena + g->arena_used;
    g->arena_used += n;
    return p;
}

static bool guest_ok(QGL3 *g, uint64_t a, uint64_t n) { return a && a <= g->ram_size && n <= g->ram_size - a; }

static void swap_copy(uint8_t *d, const uint8_t *s, size_t bytes, unsigned es)
{
    switch (es) {
    case 2: for (size_t i = 0; i + 2 <= bytes; i += 2) { d[i] = s[i + 1]; d[i + 1] = s[i]; } break;
    case 4: for (size_t i = 0; i + 4 <= bytes; i += 4) { uint32_t v; memcpy(&v, s + i, 4); v = __builtin_bswap32(v); memcpy(d + i, &v, 4); } break;
    case 8: for (size_t i = 0; i + 8 <= bytes; i += 8) { uint64_t v; memcpy(&v, s + i, 8); v = __builtin_bswap64(v); memcpy(d + i, &v, 8); } break;
    default: memcpy(d, s, bytes); break;
    }
}

/* an inline array from the stream, swapped, with spare zero bytes behind it */
static void *qgl3_take_in(QGL3 *g, const uint8_t **in, const uint8_t *end, unsigned es)
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

static void *qgl3_out(QGL3 *g, uint64_t ga, size_t n, unsigned es)
{
    if (!ga)
        return NULL;
    size_t bytes = n * es + 128;
    void *p = arena_alloc(g, bytes);
    if (p)
        memset(p, 0, bytes);
    return p;
}

static void qgl3_out_done(QGL3 *g, uint64_t ga, const void *p, size_t n, unsigned es)
{
    if (!ga || !p || !guest_ok(g, ga, n * es))
        return;
    swap_copy(g->ram + ga, p, n * es, es);
}

/* bytes in guest memory, used where they are (buffer data, names, info logs) */
static void *qgl3_raw(QGL3 *g, uint64_t ga) { return ga && ga < g->ram_size ? g->ram + ga : NULL; }

static void qgl3_bad_id(QGL3 *g, unsigned id)
{
    raise_error(g, GL_INVALID_OPERATION);
    if (id < 4096 && !g->warned[id]) {
        g->warned[id] = true;
        fprintf(stderr, "[QRX-GL3] call %u is not handled\n", id);
    }
}

#include "qrx_gl3_gen.inc"

/* ---- pixels: an offset into the bound pixel buffer, or guest memory */
static const void *pix(QGL3 *g, uint64_t ga, bool pack)
{
    GLint pb = 0;
    glGetIntegerv(pack ? GL_PIXEL_PACK_BUFFER_BINDING : GL_PIXEL_UNPACK_BUFFER_BINDING, &pb);
    if (pb)
        return (const void *)(uintptr_t)ga;
    return qgl3_raw(g, ga);
}
#define UNPACK(p) pix(g, p, false)
#define PACK(p) ((void *)pix(g, p, true))
static void qgl3h_glTexImage1D(QGL3 *g, uint32_t t, int32_t l, int32_t i, int32_t w, int32_t b, uint32_t f, uint32_t ty, uint64_t p)
{ glTexImage1D(t, l, i, w, b, f, ty, UNPACK(p)); }
static void qgl3h_glTexImage2D(QGL3 *g, uint32_t t, int32_t l, int32_t i, int32_t w, int32_t h, int32_t b, uint32_t f, uint32_t ty, uint64_t p)
{ glTexImage2D(t, l, i, w, h, b, f, ty, UNPACK(p)); }
static void qgl3h_glTexImage3D(QGL3 *g, uint32_t t, int32_t l, int32_t i, int32_t w, int32_t h, int32_t d, int32_t b, uint32_t f, uint32_t ty, uint64_t p)
{ glTexImage3D(t, l, i, w, h, d, b, f, ty, UNPACK(p)); }
static void qgl3h_glTexSubImage1D(QGL3 *g, uint32_t t, int32_t l, int32_t x, int32_t w, uint32_t f, uint32_t ty, uint64_t p)
{ glTexSubImage1D(t, l, x, w, f, ty, UNPACK(p)); }
static void qgl3h_glTexSubImage2D(QGL3 *g, uint32_t t, int32_t l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, uint32_t ty, uint64_t p)
{ glTexSubImage2D(t, l, x, y, w, h, f, ty, UNPACK(p)); }
static void qgl3h_glTexSubImage3D(QGL3 *g, uint32_t t, int32_t l, int32_t x, int32_t y, int32_t z, int32_t w, int32_t h, int32_t d, uint32_t f, uint32_t ty, uint64_t p)
{ glTexSubImage3D(t, l, x, y, z, w, h, d, f, ty, UNPACK(p)); }
static void qgl3h_glCompressedTexImage1D(QGL3 *g, uint32_t t, int32_t l, uint32_t i, int32_t w, int32_t b, int32_t n, uint64_t p)
{ glCompressedTexImage1D(t, l, i, w, b, n, UNPACK(p)); }
static void qgl3h_glCompressedTexImage2D(QGL3 *g, uint32_t t, int32_t l, uint32_t i, int32_t w, int32_t h, int32_t b, int32_t n, uint64_t p)
{ glCompressedTexImage2D(t, l, i, w, h, b, n, UNPACK(p)); }
static void qgl3h_glCompressedTexImage3D(QGL3 *g, uint32_t t, int32_t l, uint32_t i, int32_t w, int32_t h, int32_t d, int32_t b, int32_t n, uint64_t p)
{ glCompressedTexImage3D(t, l, i, w, h, d, b, n, UNPACK(p)); }
static void qgl3h_glCompressedTexSubImage1D(QGL3 *g, uint32_t t, int32_t l, int32_t x, int32_t w, uint32_t f, int32_t n, uint64_t p)
{ glCompressedTexSubImage1D(t, l, x, w, f, n, UNPACK(p)); }
static void qgl3h_glCompressedTexSubImage2D(QGL3 *g, uint32_t t, int32_t l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, int32_t n, uint64_t p)
{ glCompressedTexSubImage2D(t, l, x, y, w, h, f, n, UNPACK(p)); }
static void qgl3h_glCompressedTexSubImage3D(QGL3 *g, uint32_t t, int32_t l, int32_t x, int32_t y, int32_t z, int32_t w, int32_t h, int32_t d, uint32_t f, int32_t n, uint64_t p)
{ glCompressedTexSubImage3D(t, l, x, y, z, w, h, d, f, n, UNPACK(p)); }
static void qgl3h_glReadPixels(QGL3 *g, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, uint32_t ty, uint64_t p)
{ glReadPixels(x, y, w, h, f, ty, PACK(p)); }
static void qgl3h_glGetTexImage(QGL3 *g, uint32_t t, int32_t l, uint32_t f, uint32_t ty, uint64_t p)
{ glGetTexImage(t, l, f, ty, PACK(p)); }
static void qgl3h_glGetCompressedTexImage(QGL3 *g, uint32_t t, int32_t l, uint64_t p)
{ glGetCompressedTexImage(t, l, PACK(p)); }

/* ---- framebuffer 0 is the context's own */
static bool on_window(GLenum binding)
{
    GLint fb = 0;
    glGetIntegerv(binding, &fb);
    return G3.cur && (GLuint)fb == G3.cur->fbo;
}
static GLenum window_buffer(GLenum b)
{
    switch (b) {
    case GL_FRONT: case GL_BACK: case GL_FRONT_LEFT: case GL_BACK_LEFT: case GL_LEFT: case GL_FRONT_AND_BACK:
        return GL_COLOR_ATTACHMENT0;
    default:
        return b;
    }
}
static void qgl3h_glBindFramebuffer(QGL3 *g, uint32_t target, uint32_t fb)
{
    glBindFramebuffer(target, fb || !g->cur ? fb : g->cur->fbo);
}
static void qgl3h_glDrawBuffer(QGL3 *g, uint32_t b)
{
    (void)g;
    glDrawBuffer(on_window(GL_DRAW_FRAMEBUFFER_BINDING) ? window_buffer(b) : b);
}
static void qgl3h_glReadBuffer(QGL3 *g, uint32_t b)
{
    (void)g;
    glReadBuffer(on_window(GL_READ_FRAMEBUFFER_BINDING) ? window_buffer(b) : b);
}
static void qgl3h_glDrawBuffers(QGL3 *g, int32_t n, uint64_t ga)
{
    GLenum b[16];
    if (n < 0 || n > 16 || !guest_ok(g, ga, (uint64_t)n * 4)) { raise_error(g, GL_INVALID_VALUE); return; }
    bool win = on_window(GL_DRAW_FRAMEBUFFER_BINDING);
    for (int32_t i = 0; i < n; i++) {
        b[i] = R32(g->ram + ga + (uint64_t)i * 4);
        if (win) b[i] = window_buffer(b[i]);
    }
    glDrawBuffers(n, b);
}

/* ---- state: what the guest must see differently, then the host's answer */
static bool own_state(QGL3 *g, uint32_t pname, int64_t *v)
{
    Ctx *c = g->cur;
    GLint x = 0;
    if (!c) return false;
    switch (pname) {
    case GL_DRAW_FRAMEBUFFER_BINDING: case GL_READ_FRAMEBUFFER_BINDING:
        glGetIntegerv(pname, &x);
        *v = (GLuint)x == c->fbo ? 0 : x;
        return true;
    case GL_DRAW_BUFFER: case GL_DRAW_BUFFER0: case GL_READ_BUFFER:
        if (!on_window(pname == GL_READ_BUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING))
            return false;
        glGetIntegerv(pname, &x);
        *v = x == GL_COLOR_ATTACHMENT0 ? GL_BACK : x;
        return true;
    case GL_UNPACK_SWAP_BYTES: case GL_PACK_SWAP_BYTES:
        *v = 0;
        return true;
    case GL_DOUBLEBUFFER:
        *v = 1;
        return true;
    default:
        return false;
    }
}
static unsigned state_count(uint32_t pname)
{
    GLint n = 0;
    switch (pname) {
    case GL_COMPRESSED_TEXTURE_FORMATS:
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
        return n > 0 ? (unsigned)n : 1;
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_WRITEMASK: case GL_COLOR_CLEAR_VALUE: case GL_BLEND_COLOR:
        return 4;
    case GL_DEPTH_RANGE: case GL_MAX_VIEWPORT_DIMS: case GL_ALIASED_LINE_WIDTH_RANGE: case GL_SMOOTH_LINE_WIDTH_RANGE:
    case GL_POINT_SIZE_RANGE:
        return 2;
    default:
        return qgl_pn(pname);
    }
}
/* kind: 0 int, 1 float, 2 double, 3 boolean, 4 int64; indexed when idx >= 0 */
static void get_state(QGL3 *g, uint32_t pname, int64_t idx, uint64_t ga, int kind)
{
    static const unsigned es_of[5] = {4, 4, 8, 1, 8};
    unsigned es = es_of[kind];
    int64_t own;
    if (idx < 0 && own_state(g, pname, &own)) {
        if (!guest_ok(g, ga, es)) return;
        uint8_t *p = g->ram + ga;
        if (kind == 0) W32(p, (uint32_t)(int32_t)own);
        else if (kind == 1) { float f = (float)own; uint32_t u; memcpy(&u, &f, 4); W32(p, u); }
        else if (kind == 2) { double d = (double)own; uint64_t u; memcpy(&u, &d, 8); u = __builtin_bswap64(u); memcpy(p, &u, 8); }
        else if (kind == 3) *p = own != 0;
        else { uint64_t u = __builtin_bswap64((uint64_t)own); memcpy(p, &u, 8); }
        return;
    }
    unsigned n = idx < 0 ? state_count(pname) : (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX || pname == GL_COLOR_WRITEMASK ? 4 : 1);
    uint8_t *tmp = arena_alloc(g, (size_t)n * 8 + 128);
    if (!tmp) return;
    memset(tmp, 0, (size_t)n * 8 + 128);
    GLuint i = (GLuint)idx;
    switch (kind) {
    case 0: if (idx < 0) glGetIntegerv(pname, (GLint *)tmp); else glGetIntegeri_v(pname, i, (GLint *)tmp); break;
    case 1: glGetFloatv(pname, (GLfloat *)tmp); break;
    case 2: glGetDoublev(pname, (GLdouble *)tmp); break;
    case 3: if (idx < 0) glGetBooleanv(pname, (GLboolean *)tmp); else glGetBooleani_v(pname, i, (GLboolean *)tmp); break;
    default: if (idx < 0) glGetInteger64v(pname, (GLint64 *)tmp); else glGetInteger64i_v(pname, i, (GLint64 *)tmp); break;
    }
    qgl3_out_done(g, ga, tmp, n, es);
}
static void qgl3h_glGetIntegerv(QGL3 *g, uint32_t p, uint64_t a) { get_state(g, p, -1, a, 0); }
static void qgl3h_glGetFloatv(QGL3 *g, uint32_t p, uint64_t a) { get_state(g, p, -1, a, 1); }
static void qgl3h_glGetDoublev(QGL3 *g, uint32_t p, uint64_t a) { get_state(g, p, -1, a, 2); }
static void qgl3h_glGetBooleanv(QGL3 *g, uint32_t p, uint64_t a) { get_state(g, p, -1, a, 3); }
static void qgl3h_glGetInteger64v(QGL3 *g, uint32_t p, uint64_t a) { get_state(g, p, -1, a, 4); }
static void qgl3h_glGetIntegeri_v(QGL3 *g, uint32_t p, uint32_t i, uint64_t a) { get_state(g, p, i, a, 0); }
static void qgl3h_glGetBooleani_v(QGL3 *g, uint32_t p, uint32_t i, uint64_t a) { get_state(g, p, i, a, 3); }
static void qgl3h_glGetInteger64i_v(QGL3 *g, uint32_t p, uint32_t i, uint64_t a) { get_state(g, p, i, a, 4); }

/* ---- native contexts */
#if defined(__APPLE__)
static QglNative native_create(QglNative share)
{
    CGLPixelFormatAttribute at[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
                                    kCGLPFAAccelerated, kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, 0};
    CGLPixelFormatObj pf;
    GLint npf = 0;
    if (CGLChoosePixelFormat(at, &pf, &npf) != kCGLNoError || !pf) {
        fprintf(stderr, "[QRX-GL3] no OpenGL core pixel format\n");
        return NULL;
    }
    CGLContextObj c = NULL;
    CGLError e = CGLCreateContext(pf, share, &c);
    CGLReleasePixelFormat(pf);
    if (e != kCGLNoError) {
        fprintf(stderr, "[QRX-GL3] CGLCreateContext: %d\n", e);
        return NULL;
    }
    return c;
}
static void native_current(QglNative c) { CGLSetCurrentContext(c); }
static void native_destroy(QglNative c) { CGLDestroyContext(c); }
#else
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLConfig cfg;
static bool egl_init(void)
{
    if (dpy != EGL_NO_DISPLAY) return true;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    EGLint major, minor, n = 0;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
            fprintf(stderr, "[QRX-GL3] no EGL display\n");
            return false;
        }
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[QRX-GL3] EGL has no desktop OpenGL\n");
        return false;
    }
    const EGLint at[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    if (!eglChooseConfig(d, at, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "[QRX-GL3] no EGL config for OpenGL\n");
        return false;
    }
    dpy = d;
    return true;
}
static QglNative native_create(QglNative share)
{
    if (!egl_init()) return NULL;
    const EGLint at[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                         EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext c = eglCreateContext(dpy, cfg, share ? share : EGL_NO_CONTEXT, at);
    if (c == EGL_NO_CONTEXT) {
        fprintf(stderr, "[QRX-GL3] eglCreateContext (3.3 core): 0x%x\n", eglGetError());
        return NULL;
    }
    return c;
}
static void native_current(QglNative c)
{
    if (dpy != EGL_NO_DISPLAY) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c ? c : EGL_NO_CONTEXT);
}
static void native_destroy(QglNative c) { eglDestroyContext(dpy, c); }
#endif

static void make_current(QGL3 *g, Ctx *c)
{
    if (g->cur == c) return;
    native_current(c ? c->nat : NULL);
    g->cur = c;
}

static bool fbo_size(Ctx *c, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 16384 || h > 16384) return false;
    if (c->fbo && c->w == w && c->h == h) return true;
    if (!c->fbo) {
        glGenFramebuffers(1, &c->fbo);
        glGenRenderbuffers(1, &c->rb_color);
        glGenRenderbuffers(1, &c->rb_depth);
    }
    GLint old_rb = 0, old_draw = 0, old_read = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, c->rb_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, c->rb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)old_rb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c->rb_color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->rb_depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[QRX-GL3] window framebuffer %dx%d incomplete: 0x%x\n", w, h, st);
    if (!c->started) {
        /* a new context starts on its window, as a drawable would */
        c->started = true;
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    } else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read);
    }
    c->w = w;
    c->h = h;
    return st == GL_FRAMEBUFFER_COMPLETE;
}
/* OP_RESIZE: the first size sets the viewport and scissor box, as a drawable's first use would */
static bool op_resize(Ctx *c, int w, int h)
{
    bool ok = fbo_size(c, w, h);
    if (ok && !c->sized) {
        c->sized = true;
        glViewport(0, 0, c->w, c->h);
        glScissor(0, 0, c->w, c->h);
    }
    return ok;
}

static uint32_t op_create(QGL3 *g, uint32_t share)
{
    unsigned i;
    for (i = 1; i < MAX_CTX && g->ctx[i].used; i++) {}
    if (i == MAX_CTX) return 0;
    QglNative sh = share && share < MAX_CTX && g->ctx[share].used ? g->ctx[share].nat : g->share;
    QglNative n = native_create(sh);
    if (!n) return 0;
    if (!g->share) g->share = n;
    Ctx *c = &g->ctx[i];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->nat = n;
    g->cur = NULL;
    make_current(g, c);
    if (!g->resolved) {
        qgl3_resolve();
        g->resolved = true;
    }
    fbo_size(c, 1, 1);
    return i;
}

static void op_destroy(QGL3 *g, uint32_t id)
{
    if (!id || id >= MAX_CTX || !g->ctx[id].used) return;
    Ctx *c = &g->ctx[id];
    g->cur = NULL;
    make_current(g, c);
    glDeleteFramebuffers(1, &c->fbo);
    glDeleteRenderbuffers(1, &c->rb_color);
    glDeleteRenderbuffers(1, &c->rb_depth);
    make_current(g, NULL);
    if (c->nat != g->share)
        native_destroy(c->nat);
    memset(c, 0, sizeof *c);
}

/* QRX_GL3_CAPTURE=path: every read frame as path.NNNN (PPM), up to QRX_GL3_CAPTURE_MAX (50) */
static void capture_frame(QGL3 *g, uint64_t src, uint32_t bpr, int w, int h)
{
    static unsigned n;
    const char *path = getenv("QRX_GL3_CAPTURE");
    const char *mx = getenv("QRX_GL3_CAPTURE_MAX");
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

/* the window's frame into guest memory, A R G B bytes, top row first; the guest's state is kept */
static void op_readframe(QGL3 *g, uint64_t dst, uint32_t bpr, int w, int h)
{
    Ctx *c = g->cur;
    if (!c || w <= 0 || h <= 0 || w > c->w || h > c->h || bpr < (uint32_t)w * 4) return;
    if (!guest_ok(g, dst, (uint64_t)bpr * (uint64_t)(h - 1) + (uint64_t)w * 4)) return;
    GLint read_fb = 0, read_buf = 0, align, row, skip_r, skip_p, pack_buf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buf);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_r);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_p);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    if (pack_buf) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    uint8_t *tmp = arena_alloc(g, (size_t)w * (size_t)h * 4);
    if (tmp) {
        /* BGRA with 8_8_8_8 is A R G B in memory on a little-endian host */
        glReadPixels(0, c->h - h, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, tmp);
        for (int y = 0; y < h; y++)
            memcpy(g->ram + dst + (uint64_t)y * bpr, tmp + (size_t)(h - 1 - y) * (size_t)w * 4, (size_t)w * 4);
        capture_frame(g, dst, bpr, w, h);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glReadBuffer((GLenum)read_buf);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_ROW_LENGTH, row);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_r);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_p);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    if (pack_buf) glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
}

/* glGetString (index 0xFFFFFFFF) and glGetStringi: the length, the text into dst if it fits */
static uint32_t op_getstring(QGL3 *g, uint32_t name, uint32_t index, uint64_t dst, uint32_t size)
{
    const char *s = index == 0xFFFFFFFFu ? (const char *)glGetString(name) : (const char *)glGetStringi(name, index);
    if (!s) { glGetError(); return 0xFFFFFFFFu; }
    size_t len = strlen(s);
    if (size && guest_ok(g, dst, size)) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(g->ram + dst, s, n);
        g->ram[dst + n] = 0;
    }
    return (uint32_t)len;
}

static void run_op(QGL3 *g, unsigned id, const uint8_t *a, const uint8_t *end)
{
    size_t n = (size_t)(end - a);
    switch (id) {
    case OP_CREATE: if (n >= 4) g->ret = op_create(g, R32(a)); break;
    case OP_DESTROY: if (n >= 4) op_destroy(g, R32(a)); break;
    case OP_RESIZE: if (n >= 8 && g->cur) g->ret = op_resize(g->cur, (int)R32(a), (int)R32(a + 4)); break;
    case OP_READFRAME: if (n >= 20) op_readframe(g, R64(a), R32(a + 8), (int)R32(a + 12), (int)R32(a + 16)); break;
    case OP_GETSTRING: if (n >= 20 && g->cur) g->ret = op_getstring(g, R32(a), R32(a + 4), R64(a + 8), R32(a + 16)); break;
    case OP_SHADERSOURCE:
        if (n >= 8 && g->cur) {
            uint32_t len = R32(a + 4);
            if (len <= n - 8) {
                const GLchar *s = (const GLchar *)(a + 8);
                GLint l = (GLint)len;
                glShaderSource(R32(a), 1, &s, &l);
            }
        }
        break;
    case OP_GETERROR:
        if (g->cur) {
            uint32_t e = glGetError();
            g->ret = g->error ? g->error : e;
            g->error = 0;
        }
        break;
    default: qgl3_bad_id(g, id); break;
    }
}

/* QRX_GL3_TRACE=n: the first n calls on stderr */
static void run_stream(QGL3 *g, uint8_t *q)
{
    g->ret = 0;
    uint64_t addr = ((uint64_t)R32(q + 16) << 32) | R32(q + 20);
    uint32_t bytes = R32(q + 24), ctx = R32(q + 28);
    if (R32(q + 12) != 1 || !guest_ok(g, addr, bytes))
        return;
    if (ctx && ctx < MAX_CTX && g->ctx[ctx].used)
        make_current(g, &g->ctx[ctx]);
    const uint8_t *p = g->ram + addr, *end = p + bytes;
    static long trace = -1;
    if (trace < 0) { const char *t = getenv("QRX_GL3_TRACE"); trace = t ? atol(t) : 0; }
    g->streams++;
    while (p + 4 <= end) {
        uint32_t h = R32(p);
        unsigned id = h >> 20, nw = h & 0xFFFFFu;
        const uint8_t *a = p + 4, *e = a + (size_t)nw * 4;
        if (e > end)
            break;
        g->arena_used = 0;
        g->calls++;
        if (trace > 0) {
            trace--;
            fprintf(stderr, "[GL3] %s", id >= 16 && id < QGL3_NFUNCS && qgl3_id_names[id] ? qgl3_id_names[id] : "op");
            for (unsigned i = 0; i < nw && i < 8; i++) fprintf(stderr, " %x", R32(a + i * 4));
            fprintf(stderr, "\n");
        }
        if (id < 16)
            run_op(g, id, a, e);
        else if (g->cur && g->ctx[ctx].used && g->cur == &g->ctx[ctx])
            qgl3_dispatch(g, id, a, e);
        else
            raise_error(g, GL_INVALID_OPERATION);
        p = e;
    }
    g->arena_used = 0;
    W32(q + 32, (uint32_t)(g->ret >> 32));
    W32(q + 36, (uint32_t)g->ret);
}

/* ---- the GL thread */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t callers = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *pending;
static int probe_state;      /* 0 not yet, 1 a 3.3 core context can be made, -1 not */
static bool probe_ask;

static void *gl_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!pending && !probe_ask)
            pthread_cond_wait(&wake, &lock);
        if (probe_ask) {
            QglNative n = native_create(NULL);
            if (n) {
                /* kept: it becomes the share group of every context */
                G3.share = n;
                probe_state = 1;
            } else {
                probe_state = -1;
            }
            probe_ask = false;
        }
        if (pending) {
            run_stream(&G3, pending);
            pending = NULL;
        }
        pthread_cond_broadcast(&done);
    }
    return NULL;
}

static void start(void)
{
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)8 << 20);
    pthread_create(&t, &at, gl_thread, NULL);
    pthread_attr_destroy(&at);
}
static void start_thread(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start);
}

bool qrx_gl3_available(void)
{
    const char *e = getenv("QRX_EC64_GL3");
    if (e && *e == '0') return false;
    start_thread();
    pthread_mutex_lock(&lock);
    if (!probe_state) {
        probe_ask = true;
        pthread_cond_signal(&wake);
        while (probe_ask)
            pthread_cond_wait(&done, &lock);
    }
    bool ok = probe_state > 0;
    pthread_mutex_unlock(&lock);
    return ok;
}

void qrx_gl3_service(uint8_t *ram, uint64_t ram_size, uint8_t *q)
{
    if (!qrx_gl3_available()) return;
    pthread_mutex_lock(&callers);      /* one request at a time */
    pthread_mutex_lock(&lock);
    G3.ram = ram;
    G3.ram_size = ram_size;
    pending = q;
    pthread_cond_signal(&wake);
    while (pending)
        pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
    pthread_mutex_unlock(&callers);
}

void qrx_gl3_report(void)
{
    if (G3.streams)
        fprintf(stderr, "[QRX-GL3] streams=%llu calls=%llu\n", (unsigned long long)G3.streams, (unsigned long long)G3.calls);
}
