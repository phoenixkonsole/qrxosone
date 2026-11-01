#ifndef QRX_GL_COMPAT_H
#define QRX_GL_COMPAT_H
/* Bounded OpenGL 1.x immediate-mode subset -> QRXAccelStream.
 * Not a replacement for OpenGL or an implementation of a full GL context.
 * All guest-facing inputs are validated and remain host-owned. */
#include "qrx_accel.h"
#include <stdint.h>
#include <stdbool.h>
#define QRX_GL_TRIANGLES 0x0004u
#define QRX_GL_QUADS 0x0007u
#define QRX_GL_COLOR_BUFFER_BIT 0x00004000u
#define QRX_GL_DEPTH_BUFFER_BIT 0x00000100u
#define QRX_GL_DEPTH_TEST 0x0B71u
#define QRX_GL_BLEND 0x0BE2u
#define QRX_GL_NO_ERROR 0u
#define QRX_GL_INVALID_ENUM 0x0500u
#define QRX_GL_INVALID_VALUE 0x0501u
#define QRX_GL_INVALID_OPERATION 0x0502u
#define QRX_GL_OUT_OF_MEMORY 0x0505u
typedef struct QRXGLContext {
    QRXAccelStream *out;
    QRXAccelVertex pending[4];
    float color[4],uv[2], clear[4];
    double bounds[6];
    uint32_t mode, count, error;
    uint32_t depth_test, blend;
    bool began;
} QRXGLContext;
void qrx_gl_init(QRXGLContext *g,QRXAccelStream *out);
void qrx_gl_begin(QRXGLContext *g,uint32_t mode);
void qrx_gl_end(QRXGLContext *g);
void qrx_gl_color(QRXGLContext *g,float r,float green,float b,float a);
void qrx_gl_vertex(QRXGLContext *g,float x,float y,float z);
void qrx_gl_texcoord(QRXGLContext *g,float u,float v);
void qrx_gl_clear_color(QRXGLContext *g,float r,float green,float b,float a);
void qrx_gl_clear(QRXGLContext *g,uint32_t mask);
void qrx_gl_enable(QRXGLContext *g,uint32_t capability,bool enable);
void qrx_gl_ortho(QRXGLContext *g,double l,double r,double b,double t,double n,double f);
uint32_t qrx_gl_error(QRXGLContext *g);
bool qrx_gl_flush(QRXGLContext *g);
#endif
