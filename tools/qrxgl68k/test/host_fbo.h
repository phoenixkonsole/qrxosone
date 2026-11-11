/* For host_main.m with -DREF_FBO: framebuffer 0 is an FBO, as in the door, to tell rasterization
 * differences between a window and an FBO apart from the door's own. */
#ifdef REF_FBO
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
extern GLuint ref_fbo;
static inline void ref_bind(GLenum t, GLuint f) { glBindFramebufferEXT(t, f ? f : ref_fbo); if (!f) { glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT); } }
static inline void ref_drawbuf(GLenum m) { glDrawBuffer(m == GL_BACK ? GL_COLOR_ATTACHMENT0_EXT : m); }
static inline void ref_geti(GLenum p, GLint *v) { glGetIntegerv(p, v); if (p == GL_FRAMEBUFFER_BINDING_EXT && (GLuint)*v == ref_fbo) *v = 0; }
#define glBindFramebufferEXT ref_bind
#define glDrawBuffer ref_drawbuf
#define glGetIntegerv ref_geti
#endif
