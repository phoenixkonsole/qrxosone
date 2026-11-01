#include "qrx_gl_compat.h"
#include <math.h>
#include <string.h>
static void err(QRXGLContext *g,uint32_t e){if(g&&g->error==QRX_GL_NO_ERROR)g->error=e;}
static float clip01(float f){return f<0.f?0.f:f>1.f?1.f:f;}
void qrx_gl_init(QRXGLContext *g,QRXAccelStream *out){
    if(!g)return;
    memset(g,0,sizeof(*g));g->out=out;
    for(int i=0;i<4;i++)g->color[i]=1.f;
    g->clear[3]=1.f;
    g->bounds[0]=-1.;g->bounds[1]=1.;g->bounds[2]=-1.;g->bounds[3]=1.;
    g->bounds[4]=-1.;g->bounds[5]=1.;
}
void qrx_gl_begin(QRXGLContext *g,uint32_t mode){
    if(!g)return;
    if(g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    if(mode!=QRX_GL_TRIANGLES && mode!=QRX_GL_QUADS){err(g,QRX_GL_INVALID_ENUM);return;}
    g->mode=mode;g->count=0;g->began=true;
}
void qrx_gl_color(QRXGLContext *g,float r,float green,float b,float a){
    if(!g)return;
    if(!isfinite(r)||!isfinite(green)||!isfinite(b)||!isfinite(a)){err(g,QRX_GL_INVALID_VALUE);return;}
    g->color[0]=clip01(r);g->color[1]=clip01(green);g->color[2]=clip01(b);g->color[3]=clip01(a);
}
void qrx_gl_texcoord(QRXGLContext *g,float u,float v){
    if(!g)return;
    if(!isfinite(u)||!isfinite(v)){err(g,QRX_GL_INVALID_VALUE);return;}
    g->uv[0]=u;g->uv[1]=v;
}
static void emit(QRXGLContext *g,const QRXAccelVertex *v){
    uint32_t flags=(g->depth_test?3u:0u)|(g->blend?4u:0u);
    if(!qrx_accel_triangle(g->out,v,flags,NULL,0,0,0))err(g,QRX_GL_OUT_OF_MEMORY);
}
void qrx_gl_vertex(QRXGLContext *g,float x,float y,float z){
    if(!g)return;
    if(!g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    if(!isfinite(x)||!isfinite(y)||!isfinite(z)){err(g,QRX_GL_INVALID_VALUE);return;}
    const double *p=g->bounds;
    double xx=((double)x-p[0])/(p[1]-p[0]);
    double yy=((double)y-p[2])/(p[3]-p[2]);
    double zz=((double)z-p[4])/(p[5]-p[4]);
    if(!isfinite(xx)||!isfinite(yy)||!isfinite(zz)||fabs(xx)>100000.||fabs(yy)>100000.){
        err(g,QRX_GL_INVALID_VALUE);return;
    }
    QRXAccelVertex *v=&g->pending[g->count++];
    *v=(QRXAccelVertex){(float)(xx*800.0),(float)((1.0-yy)*450.0),clip01((float)zz),
         g->uv[0],g->uv[1],g->color[0],g->color[1],g->color[2],g->color[3]};
    if(g->mode==QRX_GL_TRIANGLES && g->count==3){emit(g,g->pending);g->count=0;}
    else if(g->mode==QRX_GL_QUADS && g->count==4){
        QRXAccelVertex a[3]={g->pending[0],g->pending[1],g->pending[2]};emit(g,a);
        a[0]=g->pending[0];a[1]=g->pending[2];a[2]=g->pending[3];emit(g,a);g->count=0;
    }
}
void qrx_gl_end(QRXGLContext *g){
    if(!g)return;
    if(!g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    /* Incomplete primitive is intentionally discarded. */
    g->began=false;g->count=0;
}
void qrx_gl_clear_color(QRXGLContext *g,float r,float green,float b,float a){
    if(!g)return;
    if(g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    if(!isfinite(r)||!isfinite(green)||!isfinite(b)||!isfinite(a)){err(g,QRX_GL_INVALID_VALUE);return;}
    g->clear[0]=clip01(r);g->clear[1]=clip01(green);g->clear[2]=clip01(b);g->clear[3]=clip01(a);
}
void qrx_gl_clear(QRXGLContext *g,uint32_t mask){
    if(!g)return;
    if(g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    if(mask&~(QRX_GL_COLOR_BUFFER_BIT|QRX_GL_DEPTH_BUFFER_BIT)){err(g,QRX_GL_INVALID_VALUE);return;}
    if(mask==0)return;
    uint32_t rgb=((uint32_t)lrintf(g->clear[0]*255.f)<<16)|((uint32_t)lrintf(g->clear[1]*255.f)<<8)
                  |(uint32_t)lrintf(g->clear[2]*255.f);
    if(!qrx_accel_clear(g->out,rgb,65535))err(g,QRX_GL_OUT_OF_MEMORY);
}
void qrx_gl_enable(QRXGLContext *g,uint32_t cap,bool on){
    if(!g)return;
    if(g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    if(cap==QRX_GL_DEPTH_TEST)g->depth_test=on;
    else if(cap==QRX_GL_BLEND)g->blend=on;
    else err(g,QRX_GL_INVALID_ENUM);
}
void qrx_gl_ortho(QRXGLContext *g,double l,double r,double b,double t,double n,double f){
    if(!g)return;
    if(g->began){err(g,QRX_GL_INVALID_OPERATION);return;}
    if(!isfinite(l)||!isfinite(r)||!isfinite(b)||!isfinite(t)||!isfinite(n)||!isfinite(f)||
       l==r||b==t||n==f){err(g,QRX_GL_INVALID_VALUE);return;}
    const double bounds[]={l,r,b,t,n,f};memcpy(g->bounds,bounds,sizeof(bounds));
}
uint32_t qrx_gl_error(QRXGLContext *g){if(!g)return QRX_GL_INVALID_OPERATION;uint32_t e=g->error;g->error=0;return e;}
bool qrx_gl_flush(QRXGLContext *g){
    if(!g)return false;
    if(g->began){err(g,QRX_GL_INVALID_OPERATION);return false;}
    if(!qrx_accel_commit(g->out)){err(g,QRX_GL_OUT_OF_MEMORY);return false;}
    return true;
}
