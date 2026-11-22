/* See qrx_hostgpu.h. Linux (Raspberry Pi) version of macos/qrx_hostgpu.m: OpenGL 2.1 on a
 * surfaceless EGL context (Mesa V3D on the Pi 4/5), drawing into an offscreen FBO.
 *
 * It keeps the Metal version's conventions so frames are byte-identical in layout:
 *  - row 0 of the target is the top row: the vertex shader flips y, and because that mirrors
 *    the triangles, the front-face winding is inverted;
 *  - clip-space z runs 0..w as in Metal: the shader maps it to GL's -w..w;
 *  - viewports count y from the top, which with the flip is GL's own row order.
 * Draw calls merge while the state stays the same. QRX_HG_CAPTURE=path and
 * QRX_HG_CAPTURE_FRAME=n write that frame as a PPM, as on the Mac. A window frontend reads
 * finished frames with qrx_hg_read_frame(). */
#define GL_GLEXT_PROTOTYPES 1
#include "qrx_hostgpu.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_TEXTURES 16384u
#define BATCH_MAX (256u * 1024u)

typedef struct { GLuint tex; bool linear, wrap_s, wrap_t; } HGTex;
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static GLuint prog, fbo, rb_color, rb_depth, white;
static GLint u_tex, u_env, u_afunc, u_aref, u_fog, u_frgb, u_fstart, u_fend, u_textured;
static unsigned tw, th;
static QRXHGState cur;
static bool cur_valid;
static QRXHGVertex *batch;
static unsigned batch_n;
static HGTex textures[MAX_TEXTURES];
static uint32_t next_texture = 1;
static uint64_t frames;
static _Atomic uint64_t last_present_us;
static void *layer;
/* frames handed to the window's thread: the GL context belongs to the CPU thread */
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame_px;
static unsigned frame_w, frame_h;

static const char *vs_src =
    "#version 120\n"
    "attribute vec4 p; attribute vec2 t; attribute vec4 c;\n"
    "varying vec2 vt; varying vec4 vc; varying float vdepth;\n"
    "void main(){ vt=t; vc=c; vdepth=p.w;\n"
    "  gl_Position=vec4(p.x,-p.y,2.0*p.z-p.w,p.w); }\n";
static const char *fs_src =
    "#version 120\n"
    "uniform sampler2D tex; uniform int env, afunc, fog, textured; uniform float aref;\n"
    "uniform vec3 frgb; uniform float fstart, fend;\n"
    "varying vec2 vt; varying vec4 vc; varying float vdepth;\n"
    "void main(){ vec4 c=vc;\n"
    "  if(textured!=0){ vec4 t=texture2D(tex,vt);\n"
    "    if(env==1) c=t; else if(env==2) c=vec4(mix(c.rgb,t.rgb,t.a),c.a); else c*=t; }\n"
    "  if(afunc==0) discard;\n"
    "  else if(afunc==1){ if(!(c.a<aref)) discard; }\n"
    "  else if(afunc==2){ if(c.a!=aref) discard; }\n"
    "  else if(afunc==3){ if(!(c.a<=aref)) discard; }\n"
    "  else if(afunc==4){ if(!(c.a>aref)) discard; }\n"
    "  else if(afunc==5){ if(c.a==aref) discard; }\n"
    "  else if(afunc==6){ if(!(c.a>=aref)) discard; }\n"
    "  if(fog!=0){ float f=clamp((fend-vdepth)/(fend-fstart),0.0,1.0); c.rgb=mix(frgb,c.rgb,f); }\n"
    "  gl_FragColor=c; }\n";

void qrx_hg_state_default(QRXHGState *s)
{
    memset(s, 0, sizeof(*s));
    s->src = HG_BLEND_ONE; s->dst = HG_BLEND_ZERO; s->depth_func = HG_CMP_LESS; s->depth_write = true;
    s->alpha_func = HG_CMP_ALWAYS; s->color_write = true; s->front_ccw = true;
}
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }

/* The GL door uses its own contexts on the same thread: take ours back on every entry. */
static bool current(void)
{
    if (ctx == EGL_NO_CONTEXT) return false;
    if (eglGetCurrentContext() != ctx) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    return true;
}
static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[HG] shader: %s\n", log); }
    return s;
}
static bool gl_init(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLint major, minor, n = 0;
    EGLConfig cfg;
    dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "[HG] no EGL display\n"); return false; }
    }
    const EGLint cat[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    const EGLint xat[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(dpy, cat, &cfg, 1, &n) || n < 1) { fprintf(stderr, "[HG] no OpenGL config\n"); return false; }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xat);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "[HG] eglCreateContext: 0x%x\n", eglGetError()); return false; }
    current();
    prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "p");
    glBindAttribLocation(prog, 1, "t");
    glBindAttribLocation(prog, 2, "c");
    glLinkProgram(prog);
    {
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { char log[1024]; glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "[HG] link: %s\n", log); return false; }
    }
    u_tex = glGetUniformLocation(prog, "tex"); u_env = glGetUniformLocation(prog, "env");
    u_afunc = glGetUniformLocation(prog, "afunc"); u_aref = glGetUniformLocation(prog, "aref");
    u_fog = glGetUniformLocation(prog, "fog"); u_frgb = glGetUniformLocation(prog, "frgb");
    u_fstart = glGetUniformLocation(prog, "fstart"); u_fend = glGetUniformLocation(prog, "fend");
    u_textured = glGetUniformLocation(prog, "textured");
    glGenTextures(1, &white);
    glBindTexture(GL_TEXTURE_2D, white);
    {
        uint32_t w = 0xFFFFFFFFu;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &w);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    batch = malloc(BATCH_MAX * sizeof(QRXHGVertex));
    return batch != NULL;
}

bool qrx_hg_open(unsigned width, unsigned height)
{
    if (ctx == EGL_NO_CONTEXT && !gl_init()) return false;
    current();
    if (width == tw && height == th && fbo) return true;
    if (!fbo) { glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb_color); glGenRenderbuffers(1, &rb_depth); }
    glBindRenderbuffer(GL_RENDERBUFFER, rb_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)width, (GLsizei)height);
    glBindRenderbuffer(GL_RENDERBUFFER, rb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)width, (GLsizei)height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[HG] FBO incomplete\n"); return false; }
    tw = width; th = height;
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, (GLsizei)tw, (GLsizei)th);
    glClearColor(0, 0, 0, 1); glClearDepth(1.0);
    glDepthMask(GL_TRUE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    cur_valid = false;
    return true;
}

static void flush_batch(void)
{
    if (!batch_n) return;
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(QRXHGVertex), &batch[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(QRXHGVertex), &batch[0].u);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(QRXHGVertex), &batch[0].r);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)batch_n);
    batch_n = 0;
}
void qrx_hg_clear(bool color, bool depth, float r, float g, float b, float a, float z)
{
    if (!tw || !current()) return;
    flush_batch();
    GLbitfield m = 0;
    glDisable(GL_SCISSOR_TEST);
    if (color) { glClearColor(r, g, b, a); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); m |= GL_COLOR_BUFFER_BIT; }
    if (depth) { glClearDepth(z); glDepthMask(GL_TRUE); m |= GL_DEPTH_BUFFER_BIT; }
    if (m) glClear(m);
    cur_valid = false;      /* the masks changed */
}
static GLenum factor(uint8_t f)
{
    static const GLenum t[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                               GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR};
    return f < 10 ? t[f] : GL_ONE_MINUS_DST_COLOR;
}
static void apply_state(const QRXHGState *s)
{
    static const GLenum cmp[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glUseProgram(prog);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    if (s->blend) { glEnable(GL_BLEND); glBlendFunc(factor(s->src), factor(s->dst)); } else glDisable(GL_BLEND);
    glColorMask(s->color_write, s->color_write, s->color_write, s->color_write);
    glEnable(GL_DEPTH_TEST);                                  /* off = ALWAYS, so the write mask still applies */
    glDepthFunc(s->depth_test ? cmp[s->depth_func & 7u] : GL_ALWAYS);
    glDepthMask(s->depth_test && s->depth_write);
    if (s->cull == HG_CULL_NONE) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glCullFace(s->cull == HG_CULL_FRONT ? GL_FRONT : GL_BACK); }
    glFrontFace(s->front_ccw ? GL_CW : GL_CCW);               /* inverted: the y flip mirrors every triangle */
    if (s->vw > 0 && s->vh > 0) glViewport(s->vx, s->vy, s->vw, s->vh);
    else glViewport(0, 0, (GLsizei)tw, (GLsizei)th);
    bool textured = s->texture && s->texture < MAX_TEXTURES && textures[s->texture].tex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textured ? textures[s->texture].tex : white);
    glUniform1i(u_tex, 0);
    glUniform1i(u_textured, textured);
    glUniform1i(u_env, s->env);
    glUniform1i(u_afunc, s->alpha_test ? s->alpha_func : HG_CMP_ALWAYS);
    glUniform1f(u_aref, s->alpha_ref);
    glUniform1i(u_fog, s->fog);
    glUniform3fv(u_frgb, 1, s->fog_rgb);
    glUniform1f(u_fstart, s->fog_start);
    glUniform1f(u_fend, s->fog_end);
    cur = *s;
    cur_valid = true;
}
void qrx_hg_triangles(const QRXHGState *s, const QRXHGVertex *v, unsigned count)
{
    if (!tw || !count || !current()) return;
    if (!cur_valid || memcmp(&cur, s, sizeof(*s))) { flush_batch(); apply_state(s); }
    while (count) {
        unsigned room = BATCH_MAX - batch_n, k = count < room ? count : room;
        k -= k % 3u;
        if (!k) { flush_batch(); continue; }
        memcpy(batch + batch_n, v, (size_t)k * sizeof(QRXHGVertex));
        batch_n += k; v += k; count -= k;
        if (count) flush_batch();
    }
}

/* the target as tightly packed RGBA8, row 0 on top; CPU thread only */
static bool gl_read(void *rgba)
{
    if (!tw || !current()) return false;
    flush_batch();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)tw, (GLsizei)th, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return true;
}
/* The last presented frame, from any thread (the window's) */
bool qrx_hg_read_frame(void *rgba, unsigned *w, unsigned *h)
{
    bool ok;
    pthread_mutex_lock(&frame_lock);
    ok = frame_px != NULL;
    if (ok) { *w = frame_w; *h = frame_h; if (rgba) memcpy(rgba, frame_px, (size_t)frame_w * frame_h * 4); }
    pthread_mutex_unlock(&frame_lock);
    return ok;
}
static void hand_over(void)
{
    static uint8_t *back;
    static size_t cap;
    size_t n = (size_t)tw * th * 4;
    if (n > cap) { free(back); back = malloc(n); cap = back ? n : 0; }
    if (!back || !gl_read(back)) return;
    pthread_mutex_lock(&frame_lock);
    uint8_t *t = frame_px;
    frame_px = back; frame_w = tw; frame_h = th;
    pthread_mutex_unlock(&frame_lock);
    back = t;
    if (!back) cap = 0;
}
static void capture_frame(void)
{
    static int want = -2;
    if (want == -2) { const char *f = getenv("QRX_HG_CAPTURE_FRAME"); want = getenv("QRX_HG_CAPTURE") ? (f ? atoi(f) : 1) : -1; }
    if (want < 0 || frames != (uint64_t)want) return;
    uint8_t *px = malloc((size_t)tw * th * 4);
    FILE *o = fopen(getenv("QRX_HG_CAPTURE"), "wb");
    if (o && px && gl_read(px)) {
        fprintf(o, "P6\n%u %u\n255\n", tw, th);
        for (size_t i = 0; i < (size_t)tw * th; i++) fwrite(px + i * 4, 1, 3, o);
        fprintf(stderr, "[HG] captured frame %llu, %ux%u\n", (unsigned long long)frames, tw, th);
    }
    if (o) fclose(o);
    free(px);
}
void qrx_hg_present(void)
{
    if (!tw || !current()) return;
    flush_batch();
    glFlush();
    if (layer) hand_over();
    frames++;
    last_present_us = now_us();
    capture_frame();
}
uint64_t qrx_hg_frames(void) { return frames; }
void qrx_hg_set_layer(void *l) { layer = l; }
bool qrx_hg_active(void) { uint64_t t = last_present_us; return t && now_us() - t < 500000u; }

static void sampler(HGTex *t)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t->wrap_s ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t->wrap_t ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}
uint32_t qrx_hg_texture(uint32_t id, unsigned w, unsigned h, const void *rgba, bool linear, bool wrap_s, bool wrap_t)
{
    if (ctx == EGL_NO_CONTEXT && !qrx_hg_open(640, 480)) return 0;
    if (!current()) return 0;
    if (!id) { if (next_texture >= MAX_TEXTURES) return 0; id = next_texture++; }
    if (id >= MAX_TEXTURES || !w || !h) return 0;
    flush_batch();
    HGTex *t = &textures[id];
    if (!t->tex) glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    t->linear = linear; t->wrap_s = wrap_s; t->wrap_t = wrap_t;
    sampler(t);
    cur_valid = false;
    return id;
}
void qrx_hg_texture_sub(uint32_t id, unsigned x, unsigned y, unsigned w, unsigned h, const void *rgba)
{
    if (id >= MAX_TEXTURES || !textures[id].tex || !w || !h || !current()) return;
    flush_batch();
    glBindTexture(GL_TEXTURE_2D, textures[id].tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    cur_valid = false;
}
void qrx_hg_texture_sampler(uint32_t id, bool linear, bool wrap_s, bool wrap_t)
{
    if (id >= MAX_TEXTURES || !textures[id].tex || !current()) return;
    flush_batch();
    HGTex *t = &textures[id];
    t->linear = linear; t->wrap_s = wrap_s; t->wrap_t = wrap_t;
    glBindTexture(GL_TEXTURE_2D, t->tex);
    sampler(t);
    cur_valid = false;
}
void qrx_hg_texture_free(uint32_t id)
{
    if (id >= MAX_TEXTURES || !textures[id].tex || !current()) return;
    flush_batch();
    glDeleteTextures(1, &textures[id].tex);
    memset(&textures[id], 0, sizeof textures[id]);
    cur_valid = false;
}
