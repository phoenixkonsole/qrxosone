#include "qrx_gl_compat.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
int main(void){
    QRXAccelStream stream={0};assert(qrx_accel_init(&stream));
    QRXGLContext gl;qrx_gl_init(&gl,&stream);
    qrx_gl_clear_color(&gl,0.1f,0.2f,0.3f,1.f);
    qrx_gl_clear(&gl,QRX_GL_COLOR_BUFFER_BIT|QRX_GL_DEPTH_BUFFER_BIT);
    qrx_gl_enable(&gl,QRX_GL_DEPTH_TEST,true);
    qrx_gl_enable(&gl,QRX_GL_BLEND,true);
    qrx_gl_begin(&gl,QRX_GL_TRIANGLES);
    qrx_gl_color(&gl,1,0,0,.5f);
    qrx_gl_vertex(&gl,-1,-1,0);
    qrx_gl_vertex(&gl,1,-1,0);
    qrx_gl_vertex(&gl,0,1,0);
    qrx_gl_end(&gl);
    qrx_gl_begin(&gl,QRX_GL_QUADS);
    qrx_gl_color(&gl,0,1,0,1);
    qrx_gl_vertex(&gl,-.5f,-.5f,0);
    qrx_gl_vertex(&gl,.5f,-.5f,0);
    qrx_gl_vertex(&gl,.5f,.5f,0);
    qrx_gl_vertex(&gl,-.5f,.5f,0);
    qrx_gl_end(&gl);
    assert(qrx_gl_error(&gl)==0);
    assert(qrx_gl_flush(&gl));
    QRXAccelFrame *f=qrx_accel_take_latest(&stream);
    assert(f && f->count==4); /* clear, triangle, quad => two triangles */
    assert(f->commands[0].kind==QRX_AC_CLEAR);
    assert(f->commands[1].flags==7);
    assert(fabsf(f->commands[1].vert[0].x)<.01f);
    assert(fabsf(f->commands[1].vert[0].y-450.f)<.01f);
    assert(f->commands[2].kind==QRX_AC_TRIANGLE && f->commands[3].kind==QRX_AC_TRIANGLE);
    qrx_accel_frame_release(f);
    qrx_gl_vertex(&gl,0,0,0);assert(qrx_gl_error(&gl)==QRX_GL_INVALID_OPERATION);
    qrx_gl_begin(&gl,0xffff);assert(qrx_gl_error(&gl)==QRX_GL_INVALID_ENUM);
    qrx_gl_ortho(&gl,0,0,0,1,0,1);assert(qrx_gl_error(&gl)==QRX_GL_INVALID_VALUE);
    qrx_accel_destroy(&stream);
    puts("QRXGL immediate-mode subset translated to QRXGPU: PASS");return 0;
}
