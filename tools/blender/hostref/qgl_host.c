/* The Mac reference's GL: qgl_api.h on the Mac's own OpenGL 2.1, with the window's framebuffer an FBO
 * that stands for framebuffer 0, as the door does it (full68k/qrx_gl68k.c). Blender's direct calls of
 * glDrawBuffer, glReadBuffer and glGet* land here and go on to OpenGL.framework. */
#define GL_SILENCE_DEPRECATION
#include "qgl_api.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	CGLContextObj cgl;
	GLuint fbo, rb_color, rb_depth;
	int w, h, started;
	GLuint draw_fb, read_fb;
	GLenum draw_buf, read_buf;
} Ctx;

static Ctx ctxs[32];
static Ctx *cur;
static CGLContextObj share;
static void *gl;

static void *real(const char *name)
{
	if (!gl)
		gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
	return dlsym(gl, name);
}
#define REAL(ret, name, args) static ret (*r_##name) args; if (!r_##name) r_##name = real(#name)

int qgl_available(void) { return 1; }

unsigned qgl_create_context(unsigned sh)
{
	unsigned i;
	for (i = 1; i < 32 && ctxs[i].cgl; i++) {}
	if (i == 32)
		return 0;
	CGLPixelFormatAttribute at[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, kCGLPFAAccelerated,
	                                kCGLPFAColorSize, 24, kCGLPFAAlphaSize, 8, kCGLPFADepthSize, 24, kCGLPFAStencilSize, 8, 0};
	CGLPixelFormatObj pf;
	GLint n;
	if (CGLChoosePixelFormat(at, &pf, &n) != kCGLNoError)
		return 0;
	CGLContextObj c = NULL;
	CGLCreateContext(pf, sh && ctxs[sh].cgl ? ctxs[sh].cgl : share, &c);
	CGLReleasePixelFormat(pf);
	if (!c)
		return 0;
	if (!share)
		share = c;
	memset(&ctxs[i], 0, sizeof ctxs[i]);
	ctxs[i].cgl = c;
	ctxs[i].draw_buf = ctxs[i].read_buf = GL_BACK;
	return i;
}

void qgl_destroy_context(unsigned id)
{
	if (id >= 32 || !ctxs[id].cgl)
		return;
	if (cur == &ctxs[id]) {
		CGLSetCurrentContext(NULL);
		cur = NULL;
	}
	if (ctxs[id].cgl != share)
		CGLDestroyContext(ctxs[id].cgl);
	memset(&ctxs[id], 0, sizeof ctxs[id]);
}

static void fbo_size(Ctx *c, int w, int h)
{
	if (c->fbo && c->w == w && c->h == h)
		return;
	if (!c->fbo) {
		glGenFramebuffersEXT(1, &c->fbo);
		glGenRenderbuffersEXT(1, &c->rb_color);
		glGenRenderbuffersEXT(1, &c->rb_depth);
	}
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->rb_color);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->rb_depth);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
	GLint old = 0;
	REAL(void, glGetIntegerv, (GLenum, GLint *));
	r_glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &old);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->fbo);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, c->rb_color);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->rb_depth);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->rb_depth);
	if (old && (GLuint)old != c->fbo)
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)old);
	c->w = w;
	c->h = h;
}

int qgl_make_current(unsigned id, int w, int h)
{
	if (!id || id >= 32 || !ctxs[id].cgl) {
		CGLSetCurrentContext(NULL);
		cur = NULL;
		return id == 0;
	}
	Ctx *c = &ctxs[id];
	CGLSetCurrentContext(c->cgl);
	cur = c;
	fbo_size(c, w < 1 ? 1 : w, h < 1 ? 1 : h);
	if (!c->started) {
		c->started = 1;
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->fbo);
		REAL(void, glDrawBuffer, (GLenum));
		REAL(void, glReadBuffer, (GLenum));
		r_glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
		r_glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
		glViewport(0, 0, c->w, c->h);
		glScissor(0, 0, c->w, c->h);
	}
	return 1;
}

void qgl_read_frame(void *dst, int bpr, int w, int h)
{
	Ctx *c = cur;
	if (!c || w > c->w || h > c->h)
		return;
	REAL(void, glReadBuffer, (GLenum));
	REAL(void, glGetIntegerv, (GLenum, GLint *));
	GLint rfb = 0, rbuf = 0, align = 4, row = 0, pb = 0;
	r_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &rfb);
	r_glGetIntegerv(GL_READ_BUFFER, &rbuf);
	r_glGetIntegerv(GL_PACK_ALIGNMENT, &align);
	r_glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
	r_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, c->fbo);
	r_glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	if (pb)
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	unsigned char *tmp = malloc((size_t)w * h * 4);
	glReadPixels(0, c->h - h, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, tmp);
	/* A R G B bytes, top row first, as the door delivers them to the guest. */
	for (int y = 0; y < h; y++)
		memcpy((char *)dst + (size_t)y * bpr, tmp + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
	free(tmp);
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, (GLuint)rfb);
	r_glReadBuffer((GLenum)rbuf);
	glPixelStorei(GL_PACK_ALIGNMENT, align);
	glPixelStorei(GL_PACK_ROW_LENGTH, row);
	if (pb)
		glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pb);
}

static GLenum map_buf(GLenum m, GLuint fb)
{
	if (fb)
		return m;
	switch (m) {
	case GL_FRONT: case GL_BACK: case GL_FRONT_LEFT: case GL_BACK_LEFT: case GL_LEFT: case GL_FRONT_AND_BACK:
		return GL_COLOR_ATTACHMENT0_EXT;
	default:
		return m;
	}
}

static void bind_fb(GLenum target, GLuint fb)
{
	REAL(void, glDrawBuffer, (GLenum));
	REAL(void, glReadBuffer, (GLenum));
	if (!cur) {
		glBindFramebufferEXT(target, fb);
		return;
	}
	if (target == GL_FRAMEBUFFER_EXT || target == GL_DRAW_FRAMEBUFFER_EXT) cur->draw_fb = fb;
	if (target == GL_FRAMEBUFFER_EXT || target == GL_READ_FRAMEBUFFER_EXT) cur->read_fb = fb;
	glBindFramebufferEXT(target, fb ? fb : cur->fbo);
	if (!fb && (target == GL_FRAMEBUFFER_EXT || target == GL_DRAW_FRAMEBUFFER_EXT))
		r_glDrawBuffer(cur->draw_buf == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT);
	if (!fb && (target == GL_FRAMEBUFFER_EXT || target == GL_READ_FRAMEBUFFER_EXT))
		r_glReadBuffer(cur->read_buf == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT);
}
static void w_glBindFramebuffer(GLenum t, GLuint fb) { bind_fb(t, fb); }

void glDrawBuffer(GLenum m)
{
	REAL(void, glDrawBuffer, (GLenum));
	if (cur && !cur->draw_fb) cur->draw_buf = m;
	r_glDrawBuffer(cur ? map_buf(m, cur->draw_fb) : m);
}

void glReadBuffer(GLenum m)
{
	REAL(void, glReadBuffer, (GLenum));
	if (cur && !cur->read_fb) cur->read_buf = m;
	r_glReadBuffer(cur ? map_buf(m, cur->read_fb) : m);
}

static void w_glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	GLenum b[16];
	for (GLsizei i = 0; i < n && i < 16; i++)
		b[i] = cur ? map_buf(bufs[i], cur->draw_fb) : bufs[i];
	glDrawBuffersARB(n, b);
}

static int own(GLenum p, double *v)
{
	if (!cur)
		return 0;
	switch (p) {
	case GL_FRAMEBUFFER_BINDING_EXT: *v = cur->draw_fb; return 1;
	case GL_READ_FRAMEBUFFER_BINDING_EXT: *v = cur->read_fb; return 1;
	case GL_DOUBLEBUFFER: *v = 1; return 1;
	case GL_DRAW_BUFFER: if (cur->draw_fb) return 0; *v = cur->draw_buf; return 1;
	case GL_READ_BUFFER: if (cur->read_fb) return 0; *v = cur->read_buf; return 1;
	default: return 0;
	}
}

void glGetIntegerv(GLenum p, GLint *v)
{
	REAL(void, glGetIntegerv, (GLenum, GLint *));
	double d;
	if (own(p, &d)) *v = (GLint)d; else r_glGetIntegerv(p, v);
}
void glGetFloatv(GLenum p, GLfloat *v)
{
	REAL(void, glGetFloatv, (GLenum, GLfloat *));
	double d;
	if (own(p, &d)) *v = (GLfloat)d; else r_glGetFloatv(p, v);
}
void glGetDoublev(GLenum p, GLdouble *v)
{
	REAL(void, glGetDoublev, (GLenum, GLdouble *));
	double d;
	if (own(p, &d)) *v = d; else r_glGetDoublev(p, v);
}
void glGetBooleanv(GLenum p, GLboolean *v)
{
	REAL(void, glGetBooleanv, (GLenum, GLboolean *));
	double d;
	if (own(p, &d)) *v = d != 0; else r_glGetBooleanv(p, v);
}

void *qgl_GetProcAddress(const char *name)
{
	if (!strcmp(name, "glBindFramebuffer") || !strcmp(name, "glBindFramebufferEXT"))
		return (void *)w_glBindFramebuffer;
	if (!strcmp(name, "glDrawBuffers") || !strcmp(name, "glDrawBuffersARB"))
		return (void *)w_glDrawBuffers;
	if (!strcmp(name, "glDrawBuffer") || !strcmp(name, "glReadBuffer") || !strncmp(name, "glGet", 5)) {
		void *own_fn = dlsym(RTLD_MAIN_ONLY, name);
		if (own_fn)
			return own_fn;
	}
	return real(name);
}

/* QRX_REF_TRACE: projections and text placements, to compare with the door's QRX_GL68K_TRACE. */
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	REAL(void, glOrtho, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble));
	if (getenv("QRX_REF_TRACE"))
		fprintf(stderr, "[REF] glOrtho %g %g %g %g\n", l, r, b, t);
	r_glOrtho(l, r, b, t, n, f);
}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
	REAL(void, glTranslatef, (GLfloat, GLfloat, GLfloat));
	if (getenv("QRX_REF_TRACE"))
		fprintf(stderr, "[REF] glTranslatef %g %g %g\n", x, y, z);
	r_glTranslatef(x, y, z);
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	REAL(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
	if (getenv("QRX_REF_TRACE"))
		fprintf(stderr, "[REF] glViewport %d %d %d %d\n", x, y, w, h);
	r_glViewport(x, y, w, h);
}
