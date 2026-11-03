/* ARMITHLON 0.9: strictly scoped OpenGL 1.x guest HLE for QRX86's native
 * Windows PE interpreter. No shader/texture/external driver ABI support.
 * Uses the same QRXAccelStream as the 68k QRXGPU; sends triangles to Metal
 * via bounded Q86C Unix socket. Off unless QRX86_HOST_GL was set at build. */
#include "qrxwin.h"
#ifdef QRX86_HOST_GL
#include "qrx_gl_compat.h"
#include "qrx86_ipc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define GLFN(name) static uint32_t name(qwin *w,uint32_t *a)
static QRXAccelStream stream;
static QRXGLContext ctx;
static int initialized;
static uint32_t context_handle=1;
static float gflt(uint32_t w){float f;memcpy(&f,&w,4);return f;}
static double gdbl(const uint32_t *a,int index){uint64_t b=(uint64_t)a[index*2] | ((uint64_t)a[index*2+1]<<32);double d;memcpy(&d,&b,8);return d;}
static int ready(void){
    if(initialized)return 1;
    if(!qrx_accel_init(&stream))return 0;
    qrx_gl_init(&ctx,&stream);
    initialized=1;return 1;
}
static uint32_t flush_frame(void){
    if(!ready()||!qrx_gl_flush(&ctx))return 0;
    QRXAccelFrame *f=qrx_accel_take_latest(&stream);
    if(!f)return 0;
    const char *path=getenv("ARMITHLON_QRX86_SOCKET");
    int ok=f->count==0 || (path&&*path&&qrx86_ipc_send_commands(path,f));
    qrx_accel_frame_release(f);
    return ok?1u:0u;
}
GLFN(gl_begin){(void)w;if(ready())qrx_gl_begin(&ctx,a[0]);return 0;}
GLFN(gl_end){(void)w;(void)a;if(ready())qrx_gl_end(&ctx);return 0;}
GLFN(gl_vertex2){(void)w;if(ready())qrx_gl_vertex(&ctx,gflt(a[0]),gflt(a[1]),0);return 0;}
GLFN(gl_vertex3){(void)w;if(ready())qrx_gl_vertex(&ctx,gflt(a[0]),gflt(a[1]),gflt(a[2]));return 0;}
GLFN(gl_color3){(void)w;if(ready())qrx_gl_color(&ctx,gflt(a[0]),gflt(a[1]),gflt(a[2]),1);return 0;}
GLFN(gl_color4){(void)w;if(ready())qrx_gl_color(&ctx,gflt(a[0]),gflt(a[1]),gflt(a[2]),gflt(a[3]));return 0;}
GLFN(gl_clearcolor){(void)w;if(ready())qrx_gl_clear_color(&ctx,gflt(a[0]),gflt(a[1]),gflt(a[2]),gflt(a[3]));return 0;}
GLFN(gl_clear){(void)w;if(ready())qrx_gl_clear(&ctx,a[0]);return 0;}
GLFN(gl_enable){(void)w;if(ready())qrx_gl_enable(&ctx,a[0],true);return 0;}
GLFN(gl_disable){(void)w;if(ready())qrx_gl_enable(&ctx,a[0],false);return 0;}
GLFN(gl_ortho){(void)w;if(ready())qrx_gl_ortho(&ctx,gdbl(a,0),gdbl(a,1),gdbl(a,2),gdbl(a,3),gdbl(a,4),gdbl(a,5));return 0;}
GLFN(gl_get_error){(void)w;(void)a;return ready()?qrx_gl_error(&ctx):QRX_GL_OUT_OF_MEMORY;}
GLFN(gl_flush){(void)w;(void)a;(void)flush_frame();return 0;}
GLFN(wgl_create){(void)w;(void)a;return ready()?context_handle:0;}
GLFN(wgl_make){(void)w;return (a[1]==0||a[1]==context_handle)?1u:0u;}
GLFN(wgl_delete){(void)w;return a[0]==context_handle?1u:0u;}
GLFN(wgl_getproc){char n[96];qw_rd_str(w,a[0],n,sizeof(n));return qw_find_hle("opengl32",n)?qw_thunk(w,"opengl32",n):0;}
GLFN(gdi_choosepix){(void)w;(void)a;return 1;}
GLFN(gdi_setpix){(void)w;(void)a;return 1;}
GLFN(gdi_getpix){(void)w;(void)a;return 1;}
GLFN(gdi_swap){(void)w;(void)a;return flush_frame();}
const qhle qw_gl[]={
 {"opengl32","glBegin",-1,gl_begin}, {"opengl32","glEnd",-1,gl_end},
 {"opengl32","glVertex2f",-1,gl_vertex2},{"opengl32","glVertex3f",-1,gl_vertex3},
 {"opengl32","glColor3f",-1,gl_color3},{"opengl32","glColor4f",-1,gl_color4},
 {"opengl32","glClearColor",-1,gl_clearcolor},{"opengl32","glClear",-1,gl_clear},
 {"opengl32","glEnable",-1,gl_enable},{"opengl32","glDisable",-1,gl_disable},
 {"opengl32","glOrtho",-1,gl_ortho},{"opengl32","glGetError",-1,gl_get_error},
 {"opengl32","glFlush",-1,gl_flush},{"opengl32","glFinish",-1,gl_flush},
 {"opengl32","wglCreateContext",1,wgl_create},{"opengl32","wglMakeCurrent",2,wgl_make},
 {"opengl32","wglDeleteContext",1,wgl_delete},{"opengl32","wglGetProcAddress",1,wgl_getproc},
 {"gdi32","ChoosePixelFormat",2,gdi_choosepix},{"gdi32","SetPixelFormat",3,gdi_setpix},
 {"gdi32","GetPixelFormat",1,gdi_getpix},{"gdi32","SwapBuffers",1,gdi_swap},
 {NULL,NULL,0,NULL}
};
#else
/* This translation unit must only be used in native QRX86 builds. */
#error QRX86_HOST_GL required
#endif
