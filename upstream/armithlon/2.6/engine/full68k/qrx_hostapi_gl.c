/* OpenGL 1.1 for x86 programs on the host GPU: fixed function, immediate mode and
 * client arrays, textures, the matrix stacks. Lighting, display lists, and anything
 * newer are absent; calls to those stop the program with an "unimplemented" message. */
#include "qrx_hostapi.h"
#include "qrx_hostgpu.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define GLF(n) static uint32_t n(x86cpu *c,const uint32_t *a)
#define UNUSED_ARGS (void)c;(void)a
enum { GL_INVALID_ENUM=0x500,GL_INVALID_OPERATION=0x502 };
typedef struct { float m[16]; } Mat;
static const Mat identity={{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
static Mat stack_mv[32],stack_pr[8],stack_tx[8];
static int top_mv,top_pr,top_tx,matrix_mode;
static QRXHGState st;
static float cur_color[4]={1,1,1,1},cur_uv[2];
static bool begun;
static uint32_t prim;
#define IMM_MAX 65536u
static QRXHGVertex *imm;
static unsigned imm_n;
static QRXHGVertex *tris;
static unsigned tris_cap;
static uint32_t gl_error;
static float clear_rgba[4]={0,0,0,0},clear_z=1.f;
static bool tex2d_on;
typedef struct { uint32_t hg; int w,h; bool linear,wrap_s,wrap_t; } GLTex;
#define GL_TEX_MAX 4096u
static GLTex gltex[GL_TEX_MAX];
static uint32_t bound_tex,next_name=1;
static int unpack_align=4;
static int vp_x,vp_y,vp_w,vp_h;
static unsigned win_w=640,win_h=480;
static void ensure(void){
    if(!imm){
        imm=malloc(IMM_MAX*sizeof(QRXHGVertex));
        tris_cap=IMM_MAX*3u;
        tris=malloc(tris_cap*sizeof(QRXHGVertex));
        for(int i=0;i<32;i++)stack_mv[i]=identity;
        for(int i=0;i<8;i++){stack_pr[i]=identity;stack_tx[i]=identity;}
        qrx_hg_state_default(&st);
        st.depth_write=true;
        qrx_hg_open(win_w,win_h);
        vp_w=(int)win_w;vp_h=(int)win_h;
    }
}
static Mat *cur_matrix(void){
    ensure();
    return matrix_mode==1?&stack_pr[top_pr]:matrix_mode==2?&stack_tx[top_tx]:&stack_mv[top_mv];
}
static void mat_mul(Mat *r,const Mat *a,const Mat *b){
    Mat t;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++){
        float v=0;for(int k=0;k<4;k++)v+=a->m[k*4+j]*b->m[i*4+k];
        t.m[i*4+j]=v;
    }
    *r=t;
}
static void apply_matrix(const Mat *m){Mat *c=cur_matrix();mat_mul(c,c,m);}
static inline void xform(const Mat *m,const float in[4],float out[4]){
    for(int j=0;j<4;j++)out[j]=m->m[j]*in[0]+m->m[4+j]*in[1]+m->m[8+j]*in[2]+m->m[12+j]*in[3];
}
/* One vertex into hardware space: GL's z runs -1..1, Metal's 0..1. */
static void make_vertex(QRXHGVertex *v,const float p[4],const float col[4],const float uv[2],const Mat *mvp,const Mat *tx){
    float o[4];
    xform(mvp,p,o);
    v->x=o[0];v->y=o[1];v->z=(o[2]+o[3])*0.5f;v->w=o[3];
    v->r=col[0];v->g=col[1];v->b=col[2];v->a=col[3];
    float t[4]={uv[0],uv[1],0,1},u[4];
    xform(tx,t,u);
    v->u=u[0];v->v=u[1];
}
static void sync_state(void){
    st.vx=vp_x;st.vw=vp_w;st.vh=vp_h;
    st.vy=(int)win_h-vp_y-vp_h;
    st.texture=(tex2d_on&&bound_tex<GL_TEX_MAX)?gltex[bound_tex].hg:0;
}
/* Turns imm[] into a plain triangle list according to the primitive type. */
static void flush_prim(uint32_t mode,const QRXHGVertex *v,unsigned n){
    if(n<3)return;
    unsigned out=0;
    QRXHGVertex *d=tris;
#define T(a,b,cc) do{ if(out+3<=tris_cap){d[out++]=v[a];d[out++]=v[b];d[out++]=v[cc];} }while(0)
    switch(mode){
    case 4: for(unsigned i=0;i+2<n;i+=3)T(i,i+1,i+2);break;
    case 5: for(unsigned i=0;i+2<n;i++){if(i&1)T(i+1,i,i+2);else T(i,i+1,i+2);}break;
    case 6: case 9: for(unsigned i=1;i+1<n;i++)T(0,i,i+1);break;
    case 7: for(unsigned i=0;i+3<n;i+=4){T(i,i+1,i+2);T(i,i+2,i+3);}break;
    case 8: for(unsigned i=0;i+3<n;i+=2){T(i,i+1,i+3);T(i,i+3,i+2);}break;
    default: return; /* points and lines are not drawn */
    }
#undef T
    sync_state();
    qrx_hg_triangles(&st,d,out);
}
static void emit_vertex(float x,float y,float z,float w){
    ensure();
    if(!begun||imm_n>=IMM_MAX)return;
    Mat mvp;
    mat_mul(&mvp,&stack_pr[top_pr],&stack_mv[top_mv]);
    float p[4]={x,y,z,w};
    make_vertex(&imm[imm_n++],p,cur_color,cur_uv,&mvp,&stack_tx[top_tx]);
}
#define F(i) api_float(a[i])
GLF(gl_Begin){UNUSED_ARGS;ensure();prim=a[0];begun=true;imm_n=0;return 0;}
GLF(gl_End){UNUSED_ARGS;ensure();begun=false;flush_prim(prim,imm,imm_n);imm_n=0;return 0;}
GLF(gl_Vertex2f){UNUSED_ARGS;emit_vertex(F(0),F(1),0,1);return 0;}
GLF(gl_Vertex3f){UNUSED_ARGS;emit_vertex(F(0),F(1),F(2),1);return 0;}
GLF(gl_Vertex4f){UNUSED_ARGS;emit_vertex(F(0),F(1),F(2),F(3));return 0;}
GLF(gl_Vertex2i){UNUSED_ARGS;emit_vertex((float)(int32_t)a[0],(float)(int32_t)a[1],0,1);return 0;}
GLF(gl_Vertex3i){UNUSED_ARGS;emit_vertex((float)(int32_t)a[0],(float)(int32_t)a[1],(float)(int32_t)a[2],1);return 0;}
GLF(gl_Vertex2d){UNUSED_ARGS;emit_vertex((float)api_double(a),(float)api_double(a+2),0,1);return 0;}
GLF(gl_Vertex3d){UNUSED_ARGS;emit_vertex((float)api_double(a),(float)api_double(a+2),(float)api_double(a+4),1);return 0;}
static float rdf(x86cpu *c,uint32_t p){return api_float(x86_rd32(c,p));}
GLF(gl_Vertex2fv){emit_vertex(rdf(c,a[0]),rdf(c,a[0]+4),0,1);return 0;}
GLF(gl_Vertex3fv){emit_vertex(rdf(c,a[0]),rdf(c,a[0]+4),rdf(c,a[0]+8),1);return 0;}
GLF(gl_Vertex4fv){emit_vertex(rdf(c,a[0]),rdf(c,a[0]+4),rdf(c,a[0]+8),rdf(c,a[0]+12));return 0;}
static void set_color(float r,float g,float b,float al){cur_color[0]=r;cur_color[1]=g;cur_color[2]=b;cur_color[3]=al;}
GLF(gl_Color3f){UNUSED_ARGS;set_color(F(0),F(1),F(2),1);return 0;}
GLF(gl_Color4f){UNUSED_ARGS;set_color(F(0),F(1),F(2),F(3));return 0;}
GLF(gl_Color3d){UNUSED_ARGS;set_color((float)api_double(a),(float)api_double(a+2),(float)api_double(a+4),1);return 0;}
GLF(gl_Color3ub){UNUSED_ARGS;set_color((a[0]&255u)/255.f,(a[1]&255u)/255.f,(a[2]&255u)/255.f,1);return 0;}
GLF(gl_Color4ub){UNUSED_ARGS;set_color((a[0]&255u)/255.f,(a[1]&255u)/255.f,(a[2]&255u)/255.f,(a[3]&255u)/255.f);return 0;}
GLF(gl_Color3fv){set_color(rdf(c,a[0]),rdf(c,a[0]+4),rdf(c,a[0]+8),1);return 0;}
GLF(gl_Color4fv){set_color(rdf(c,a[0]),rdf(c,a[0]+4),rdf(c,a[0]+8),rdf(c,a[0]+12));return 0;}
GLF(gl_Color4ubv){uint32_t v=x86_rd32(c,a[0]);set_color((v&255u)/255.f,((v>>8)&255u)/255.f,((v>>16)&255u)/255.f,(v>>24)/255.f);return 0;}
GLF(gl_TexCoord2f){UNUSED_ARGS;cur_uv[0]=F(0);cur_uv[1]=F(1);return 0;}
GLF(gl_TexCoord2d){UNUSED_ARGS;cur_uv[0]=(float)api_double(a);cur_uv[1]=(float)api_double(a+2);return 0;}
GLF(gl_TexCoord2fv){cur_uv[0]=rdf(c,a[0]);cur_uv[1]=rdf(c,a[0]+4);return 0;}
GLF(gl_noop){UNUSED_ARGS;return 0;}
static void set_cap(uint32_t cap,bool on){
    ensure();
    switch(cap){
    case 0x0B71:st.depth_test=on;break;
    case 0x0BE2:st.blend=on;break;
    case 0x0BC0:st.alpha_test=on;break;
    case 0x0DE1:tex2d_on=on;break;
    case 0x0B44:st.cull=on?(st.cull?st.cull:HG_CULL_BACK):HG_CULL_NONE;break;
    case 0x0B60:st.fog=on;break;
    default:break;
    }
}
GLF(gl_Enable){UNUSED_ARGS;set_cap(a[0],true);return 0;}
GLF(gl_Disable){UNUSED_ARGS;set_cap(a[0],false);return 0;}
static uint8_t blend_factor(uint32_t f){
    switch(f){
    case 0:return HG_BLEND_ZERO;case 1:return HG_BLEND_ONE;
    case 0x300:return HG_BLEND_SRC_COLOR;case 0x301:return HG_BLEND_INV_SRC_COLOR;
    case 0x302:return HG_BLEND_SRC_ALPHA;case 0x303:return HG_BLEND_INV_SRC_ALPHA;
    case 0x304:return HG_BLEND_DST_ALPHA;case 0x305:return HG_BLEND_INV_DST_ALPHA;
    case 0x306:return HG_BLEND_DST_COLOR;case 0x307:return HG_BLEND_INV_DST_COLOR;
    default:return HG_BLEND_ONE;
    }
}
GLF(gl_BlendFunc){UNUSED_ARGS;ensure();st.src=blend_factor(a[0]);st.dst=blend_factor(a[1]);return 0;}
static uint8_t cmp_func(uint32_t f){return (uint8_t)(f&7u);} /* GL_NEVER..GL_ALWAYS = 0x200..0x207, same order */
GLF(gl_DepthFunc){UNUSED_ARGS;ensure();st.depth_func=cmp_func(a[0]);return 0;}
GLF(gl_DepthMask){UNUSED_ARGS;ensure();st.depth_write=(a[0]&255u)!=0;return 0;}
GLF(gl_AlphaFunc){UNUSED_ARGS;ensure();st.alpha_func=cmp_func(a[0]);st.alpha_ref=F(1);return 0;}
GLF(gl_CullFace){UNUSED_ARGS;ensure();st.cull=a[0]==0x404?HG_CULL_FRONT:HG_CULL_BACK;return 0;}
GLF(gl_FrontFace){UNUSED_ARGS;ensure();st.front_ccw=a[0]!=0x900;return 0;}
GLF(gl_ColorMask){UNUSED_ARGS;ensure();st.color_write=(a[0]|a[1]|a[2]|a[3])&255u;return 0;}
GLF(gl_ClearColor){UNUSED_ARGS;for(int i=0;i<4;i++)clear_rgba[i]=F(i);return 0;}
GLF(gl_ClearDepth){UNUSED_ARGS;clear_z=(float)api_double(a);return 0;}
GLF(gl_Clear){UNUSED_ARGS;ensure();qrx_hg_clear((a[0]&0x4000u)!=0,(a[0]&0x100u)!=0,clear_rgba[0],clear_rgba[1],clear_rgba[2],clear_rgba[3],clear_z);return 0;}
GLF(gl_Viewport){UNUSED_ARGS;ensure();vp_x=(int)a[0];vp_y=(int)a[1];vp_w=(int)a[2];vp_h=(int)a[3];return 0;}
GLF(gl_MatrixMode){UNUSED_ARGS;matrix_mode=a[0]==0x1701?1:a[0]==0x1702?2:0;return 0;}
GLF(gl_LoadIdentity){UNUSED_ARGS;*cur_matrix()=identity;return 0;}
static void rd_matf(x86cpu *c,uint32_t p,Mat *m){for(unsigned i=0;i<16;i++)m->m[i]=rdf(c,p+i*4u);}
static void rd_matd(x86cpu *c,uint32_t p,Mat *m){
    for(unsigned i=0;i<16;i++){uint32_t w[2]={x86_rd32(c,p+i*8u),x86_rd32(c,p+i*8u+4u)};m->m[i]=(float)api_double(w);}
}
GLF(gl_LoadMatrixf){rd_matf(c,a[0],cur_matrix());return 0;}
GLF(gl_LoadMatrixd){rd_matd(c,a[0],cur_matrix());return 0;}
GLF(gl_MultMatrixf){Mat m;rd_matf(c,a[0],&m);apply_matrix(&m);return 0;}
GLF(gl_MultMatrixd){Mat m;rd_matd(c,a[0],&m);apply_matrix(&m);return 0;}
GLF(gl_Translatef){UNUSED_ARGS;Mat m=identity;m.m[12]=F(0);m.m[13]=F(1);m.m[14]=F(2);apply_matrix(&m);return 0;}
GLF(gl_Translated){UNUSED_ARGS;Mat m=identity;m.m[12]=(float)api_double(a);m.m[13]=(float)api_double(a+2);m.m[14]=(float)api_double(a+4);apply_matrix(&m);return 0;}
GLF(gl_Scalef){UNUSED_ARGS;Mat m=identity;m.m[0]=F(0);m.m[5]=F(1);m.m[10]=F(2);apply_matrix(&m);return 0;}
static void rotate(float deg,float x,float y,float z){
    float l=sqrtf(x*x+y*y+z*z);
    if(l==0)return;
    x/=l;y/=l;z/=l;
    float r=deg*(float)M_PI/180.f,s=sinf(r),co=cosf(r),t=1-co;
    Mat m=identity;
    m.m[0]=x*x*t+co;m.m[4]=x*y*t-z*s;m.m[8]=x*z*t+y*s;
    m.m[1]=y*x*t+z*s;m.m[5]=y*y*t+co;m.m[9]=y*z*t-x*s;
    m.m[2]=z*x*t-y*s;m.m[6]=z*y*t+x*s;m.m[10]=z*z*t+co;
    apply_matrix(&m);
}
GLF(gl_Rotatef){UNUSED_ARGS;rotate(F(0),F(1),F(2),F(3));return 0;}
GLF(gl_Rotated){UNUSED_ARGS;rotate((float)api_double(a),(float)api_double(a+2),(float)api_double(a+4),(float)api_double(a+6));return 0;}
GLF(gl_Ortho){
    UNUSED_ARGS;
    double l=api_double(a),r=api_double(a+2),b=api_double(a+4),t=api_double(a+6),n=api_double(a+8),f=api_double(a+10);
    Mat m=identity;
    m.m[0]=(float)(2/(r-l));m.m[5]=(float)(2/(t-b));m.m[10]=(float)(-2/(f-n));
    m.m[12]=(float)(-(r+l)/(r-l));m.m[13]=(float)(-(t+b)/(t-b));m.m[14]=(float)(-(f+n)/(f-n));
    apply_matrix(&m);
    return 0;
}
GLF(gl_Frustum){
    UNUSED_ARGS;
    double l=api_double(a),r=api_double(a+2),b=api_double(a+4),t=api_double(a+6),n=api_double(a+8),f=api_double(a+10);
    Mat m;memset(&m,0,sizeof(m));
    m.m[0]=(float)(2*n/(r-l));m.m[5]=(float)(2*n/(t-b));
    m.m[8]=(float)((r+l)/(r-l));m.m[9]=(float)((t+b)/(t-b));m.m[10]=(float)(-(f+n)/(f-n));m.m[11]=-1;
    m.m[14]=(float)(-2*f*n/(f-n));
    apply_matrix(&m);
    return 0;
}
GLF(gl_PushMatrix){
    UNUSED_ARGS;ensure();
    if(matrix_mode==1){if(top_pr<7){stack_pr[top_pr+1]=stack_pr[top_pr];top_pr++;}}
    else if(matrix_mode==2){if(top_tx<7){stack_tx[top_tx+1]=stack_tx[top_tx];top_tx++;}}
    else if(top_mv<31){stack_mv[top_mv+1]=stack_mv[top_mv];top_mv++;}
    return 0;
}
GLF(gl_PopMatrix){
    UNUSED_ARGS;ensure();
    if(matrix_mode==1){if(top_pr>0)top_pr--;}else if(matrix_mode==2){if(top_tx>0)top_tx--;}else if(top_mv>0)top_mv--;
    return 0;
}
GLF(gl_GenTextures){for(uint32_t i=0;i<a[0];i++)x86_wr32(c,a[1]+i*4u,next_name++<GL_TEX_MAX?next_name-1u:0u);return 0;}
GLF(gl_DeleteTextures){
    for(uint32_t i=0;i<a[0];i++){
        uint32_t n=x86_rd32(c,a[1]+i*4u);
        if(n<GL_TEX_MAX&&gltex[n].hg){qrx_hg_texture_free(gltex[n].hg);memset(&gltex[n],0,sizeof(gltex[n]));}
    }
    return 0;
}
GLF(gl_BindTexture){
    UNUSED_ARGS;ensure();
    bound_tex=a[1]<GL_TEX_MAX?a[1]:0;
    /* New names start with GL's defaults: repeat in both directions. */
    if(!gltex[bound_tex].hg){gltex[bound_tex].wrap_s=gltex[bound_tex].wrap_t=true;gltex[bound_tex].linear=true;}
    return 0;
}
/* Any supported client format into RGBA8. Returns a malloc'd block. */
static uint8_t *to_rgba(x86cpu *c,uint32_t src,int w,int h,uint32_t format,uint32_t type){
    unsigned comps=format==0x1908?4u:format==0x1907?3u:format==0x80E1?4u:format==0x80E0?3u:format==0x190A?2u:1u;
    if(type!=0x1401){gl_error=GL_INVALID_ENUM;return NULL;}
    size_t row=(size_t)w*comps;
    row=(row+(size_t)unpack_align-1u)&~((size_t)unpack_align-1u);
    uint8_t *out=malloc((size_t)w*h*4u);
    if(!out)return NULL;
    for(int y=0;y<h;y++){
        uint32_t p=src+(uint32_t)((size_t)y*row);
        uint8_t *d=out+(size_t)y*w*4u;
        for(int x=0;x<w;x++,d+=4,p+=comps){
            uint8_t v0=x86_rd8(c,p);
            switch(format){
            case 0x1908: d[0]=v0;d[1]=x86_rd8(c,p+1);d[2]=x86_rd8(c,p+2);d[3]=x86_rd8(c,p+3);break;
            case 0x1907: d[0]=v0;d[1]=x86_rd8(c,p+1);d[2]=x86_rd8(c,p+2);d[3]=255;break;
            case 0x80E1: d[2]=v0;d[1]=x86_rd8(c,p+1);d[0]=x86_rd8(c,p+2);d[3]=x86_rd8(c,p+3);break;
            case 0x80E0: d[2]=v0;d[1]=x86_rd8(c,p+1);d[0]=x86_rd8(c,p+2);d[3]=255;break;
            case 0x1909: d[0]=d[1]=d[2]=v0;d[3]=255;break;
            case 0x190A: d[0]=d[1]=d[2]=v0;d[3]=x86_rd8(c,p+1);break;
            case 0x1906: d[0]=d[1]=d[2]=255;d[3]=v0;break;
            default: d[0]=d[1]=d[2]=v0;d[3]=v0;break;
            }
        }
    }
    return out;
}
GLF(gl_TexImage2D){
    ensure();
    /* target, level, internal, w, h, border, format, type, pixels. Mip levels above 0
     * are dropped: the renderer samples the base level. */
    if(a[0]!=0x0DE1||a[1]!=0||bound_tex>=GL_TEX_MAX)return 0;
    int w=(int)a[3],h=(int)a[4];
    if(w<=0||h<=0||w>4096||h>4096)return 0;
    uint8_t *px=a[8]?to_rgba(c,a[8],w,h,a[6],a[7]):NULL;
    GLTex *t=&gltex[bound_tex];
    t->hg=qrx_hg_texture(t->hg,(unsigned)w,(unsigned)h,px,t->linear,t->wrap_s,t->wrap_t);
    t->w=w;t->h=h;
    free(px);
    return 0;
}
GLF(gl_TexSubImage2D){
    ensure();
    if(a[0]!=0x0DE1||a[1]!=0||bound_tex>=GL_TEX_MAX||!gltex[bound_tex].hg)return 0;
    int w=(int)a[4],h=(int)a[5];
    if(w<=0||h<=0)return 0;
    uint8_t *px=to_rgba(c,a[8],w,h,a[6],a[7]);
    if(px)qrx_hg_texture_sub(gltex[bound_tex].hg,a[2],a[3],(unsigned)w,(unsigned)h,px);
    free(px);
    return 0;
}
static void tex_param(uint32_t name,uint32_t v){
    if(bound_tex>=GL_TEX_MAX)return;
    GLTex *t=&gltex[bound_tex];
    if(name==0x2800||name==0x2801)t->linear=v==0x2601||v==0x2703||v==0x2701;
    else if(name==0x2802)t->wrap_s=v==0x2901;
    else if(name==0x2803)t->wrap_t=v==0x2901;
    else return;
    if(t->hg)qrx_hg_texture_sampler(t->hg,t->linear,t->wrap_s,t->wrap_t);
}
GLF(gl_TexParameteri){UNUSED_ARGS;ensure();tex_param(a[1],a[2]);return 0;}
GLF(gl_TexParameterf){UNUSED_ARGS;ensure();tex_param(a[1],(uint32_t)F(2));return 0;}
static void tex_env(uint32_t name,uint32_t v){
    if(name!=0x2200)return;
    st.env=v==0x1E01?HG_ENV_REPLACE:v==0x2101?HG_ENV_DECAL:HG_ENV_MODULATE;
}
GLF(gl_TexEnvi){UNUSED_ARGS;ensure();tex_env(a[1],a[2]);return 0;}
GLF(gl_TexEnvf){UNUSED_ARGS;ensure();tex_env(a[1],(uint32_t)F(2));return 0;}
GLF(gl_PixelStorei){UNUSED_ARGS;if(a[0]==0x0CF5&&a[1]>0)unpack_align=(int)a[1];return 0;}
GLF(gl_GetError){UNUSED_ARGS;uint32_t e=gl_error;gl_error=0;return e;}
static uint32_t put_string(x86cpu *c,unsigned slot,const char *s){
    uint32_t p=qrx_hostapi_scratch+slot*256u;
    for(unsigned i=0;;i++){x86_wr8(c,p+i,(uint8_t)s[i]);if(!s[i])break;}
    return p;
}
GLF(gl_GetString){
    switch(a[0]){
    case 0x1F00:return put_string(c,0,"QRX");
    case 0x1F01:return put_string(c,1,"ARMithlon host GPU");
    case 0x1F02:return put_string(c,2,"1.1.0");
    case 0x1F03:return put_string(c,3,"");
    default:gl_error=GL_INVALID_ENUM;return 0;
    }
}
GLF(gl_GetIntegerv){
    int32_t v=0;
    switch(a[0]){
    case 0x0D33:v=4096;break;       /* MAX_TEXTURE_SIZE */
    case 0x0D31:case 0x0D36:v=32;break;
    case 0x0D32:v=8;break;
    case 0x0D52:case 0x0D53:case 0x0D54:case 0x0D55:v=8;break;
    case 0x0D56:v=24;break;
    default:break;
    }
    if(a[0]==0x0BA2){x86_wr32(c,a[1],(uint32_t)vp_x);x86_wr32(c,a[1]+4,(uint32_t)vp_y);x86_wr32(c,a[1]+8,(uint32_t)vp_w);x86_wr32(c,a[1]+12,(uint32_t)vp_h);return 0;}
    x86_wr32(c,a[1],(uint32_t)v);
    return 0;
}
GLF(gl_GetFloatv){
    const Mat *m=a[0]==0x0BA6?&stack_mv[top_mv]:a[0]==0x0BA7?&stack_pr[top_pr]:NULL;
    if(m){for(unsigned i=0;i<16;i++){uint32_t u;memcpy(&u,&m->m[i],4);x86_wr32(c,a[1]+i*4u,u);}}
    else x86_wr32(c,a[1],0);
    return 0;
}
/* client arrays */
typedef struct { bool on; int size; uint32_t type,stride,ptr; } Arr;
static Arr arr_v,arr_c,arr_t;
static Arr *client_arr(uint32_t cap){return cap==0x8074?&arr_v:cap==0x8076?&arr_c:cap==0x8078?&arr_t:NULL;}
GLF(gl_EnableClientState){UNUSED_ARGS;Arr *r=client_arr(a[0]);if(r)r->on=true;return 0;}
GLF(gl_DisableClientState){UNUSED_ARGS;Arr *r=client_arr(a[0]);if(r)r->on=false;return 0;}
static void set_arr(Arr *r,const uint32_t *a){r->size=(int)a[0];r->type=a[1];r->stride=a[2];r->ptr=a[3];}
GLF(gl_VertexPointer){UNUSED_ARGS;set_arr(&arr_v,a);return 0;}
GLF(gl_ColorPointer){UNUSED_ARGS;set_arr(&arr_c,a);return 0;}
GLF(gl_TexCoordPointer){UNUSED_ARGS;set_arr(&arr_t,a);return 0;}
static float arr_comp(x86cpu *c,const Arr *r,uint32_t i,int k,float def){
    if(k>=r->size)return def;
    unsigned size=r->type==0x1401||r->type==0x1400?1u:r->type==0x1402||r->type==0x1403?2u:r->type==0x140A?8u:4u;
    uint32_t stride=r->stride?r->stride:(uint32_t)r->size*size;
    uint32_t p=r->ptr+i*stride+(uint32_t)k*size;
    switch(r->type){
    case 0x1406:return api_float(x86_rd32(c,p));
    case 0x140A:{uint32_t w[2]={x86_rd32(c,p),x86_rd32(c,p+4)};return (float)api_double(w);}
    case 0x1401:return x86_rd8(c,p)/255.f;
    case 0x1402:return (float)(int16_t)x86_rd16(c,p);
    case 0x1404:return (float)(int32_t)x86_rd32(c,p);
    default:return 0;
    }
}
static void fetch_array_vertex(x86cpu *c,uint32_t i,const Mat *mvp,const Mat *tx,QRXHGVertex *out){
    float p[4]={arr_comp(c,&arr_v,i,0,0),arr_comp(c,&arr_v,i,1,0),arr_comp(c,&arr_v,i,2,0),arr_comp(c,&arr_v,i,3,1)};
    float col[4]={cur_color[0],cur_color[1],cur_color[2],cur_color[3]},uv[2]={cur_uv[0],cur_uv[1]};
    if(arr_c.on){col[0]=arr_comp(c,&arr_c,i,0,1);col[1]=arr_comp(c,&arr_c,i,1,1);col[2]=arr_comp(c,&arr_c,i,2,1);col[3]=arr_comp(c,&arr_c,i,3,1);}
    if(arr_t.on){uv[0]=arr_comp(c,&arr_t,i,0,0);uv[1]=arr_comp(c,&arr_t,i,1,0);}
    make_vertex(out,p,col,uv,mvp,tx);
}
GLF(gl_DrawArrays){
    ensure();
    Mat mvp;mat_mul(&mvp,&stack_pr[top_pr],&stack_mv[top_mv]);
    uint32_t n=a[2]>IMM_MAX?IMM_MAX:a[2];
    for(uint32_t i=0;i<n;i++)fetch_array_vertex(c,a[1]+i,&mvp,&stack_tx[top_tx],&imm[i]);
    flush_prim(a[0],imm,n);
    return 0;
}
GLF(gl_DrawElements){
    ensure();
    Mat mvp;mat_mul(&mvp,&stack_pr[top_pr],&stack_mv[top_mv]);
    uint32_t n=a[1]>IMM_MAX?IMM_MAX:a[1];
    for(uint32_t i=0;i<n;i++){
        uint32_t idx=a[2]==0x1401?x86_rd8(c,a[3]+i):a[2]==0x1403?x86_rd16(c,a[3]+i*2u):x86_rd32(c,a[3]+i*4u);
        fetch_array_vertex(c,idx,&mvp,&stack_tx[top_tx],&imm[i]);
    }
    flush_prim(a[0],imm,n);
    return 0;
}
GLF(gl_ArrayElement){
    ensure();
    if(!begun||imm_n>=IMM_MAX)return 0;
    Mat mvp;mat_mul(&mvp,&stack_pr[top_pr],&stack_mv[top_mv]);
    fetch_array_vertex(c,a[0],&mvp,&stack_tx[top_tx],&imm[imm_n++]);
    return 0;
}
/* window system */
GLF(wgl_CreateContext){UNUSED_ARGS;ensure();return 0x7C000001u;}
GLF(wgl_true){UNUSED_ARGS;return 1;}
GLF(wgl_GetCurrent){UNUSED_ARGS;return imm?0x7C000001u:0;}
GLF(gl_SwapBuffers){UNUSED_ARGS;ensure();qrx_hg_present();return 1;}
GLF(gl_ChoosePixelFormat){UNUSED_ARGS;return 1;}
GLF(gl_DescribePixelFormat){
    /* PIXELFORMATDESCRIPTOR: double buffered RGBA, 32 color, 24 depth */
    if(a[3]){
        for(uint32_t i=0;i<a[2]&&i<40;i++)x86_wr8(c,a[3]+i,0);
        x86_wr16(c,a[3],40);x86_wr16(c,a[3]+2,1);x86_wr32(c,a[3]+4,0x25);
        x86_wr8(c,a[3]+9,32);x86_wr8(c,a[3]+23,24);
    }
    return 1;
}
#define API(n,na,f) {n,na,f}
const QRXHostApi qrx_api_gl[]={
    API("glBegin",1,gl_Begin),API("glEnd",0,gl_End),API("glVertex2f",2,gl_Vertex2f),API("glVertex3f",3,gl_Vertex3f),API("glVertex4f",4,gl_Vertex4f),
    API("glVertex2i",2,gl_Vertex2i),API("glVertex3i",3,gl_Vertex3i),API("glVertex2d",4,gl_Vertex2d),API("glVertex3d",6,gl_Vertex3d),
    API("glVertex2fv",1,gl_Vertex2fv),API("glVertex3fv",1,gl_Vertex3fv),API("glVertex4fv",1,gl_Vertex4fv),
    API("glColor3f",3,gl_Color3f),API("glColor4f",4,gl_Color4f),API("glColor3d",6,gl_Color3d),API("glColor3ub",3,gl_Color3ub),API("glColor4ub",4,gl_Color4ub),
    API("glColor3fv",1,gl_Color3fv),API("glColor4fv",1,gl_Color4fv),API("glColor4ubv",1,gl_Color4ubv),
    API("glTexCoord2f",2,gl_TexCoord2f),API("glTexCoord2d",4,gl_TexCoord2d),API("glTexCoord2fv",1,gl_TexCoord2fv),
    API("glNormal3f",3,gl_noop),API("glNormal3fv",1,gl_noop),API("glShadeModel",1,gl_noop),API("glHint",2,gl_noop),API("glFlush",0,gl_noop),API("glFinish",0,gl_noop),
    API("glPolygonMode",2,gl_noop),API("glDrawBuffer",1,gl_noop),API("glReadBuffer",1,gl_noop),API("glDepthRange",4,gl_noop),API("glScissor",4,gl_noop),
    API("glLineWidth",1,gl_noop),API("glPointSize",1,gl_noop),API("glPolygonOffset",2,gl_noop),API("glTexGeni",3,gl_noop),
    API("glFogf",2,gl_noop),API("glFogi",2,gl_noop),API("glFogfv",2,gl_noop),API("glLightfv",3,gl_noop),API("glMaterialfv",3,gl_noop),API("glLightModelfv",2,gl_noop),
    API("glEnable",1,gl_Enable),API("glDisable",1,gl_Disable),API("glBlendFunc",2,gl_BlendFunc),API("glDepthFunc",1,gl_DepthFunc),API("glDepthMask",1,gl_DepthMask),
    API("glAlphaFunc",2,gl_AlphaFunc),API("glCullFace",1,gl_CullFace),API("glFrontFace",1,gl_FrontFace),API("glColorMask",4,gl_ColorMask),
    API("glClearColor",4,gl_ClearColor),API("glClearDepth",2,gl_ClearDepth),API("glClear",1,gl_Clear),API("glViewport",4,gl_Viewport),
    API("glMatrixMode",1,gl_MatrixMode),API("glLoadIdentity",0,gl_LoadIdentity),API("glLoadMatrixf",1,gl_LoadMatrixf),API("glLoadMatrixd",1,gl_LoadMatrixd),
    API("glMultMatrixf",1,gl_MultMatrixf),API("glMultMatrixd",1,gl_MultMatrixd),API("glTranslatef",3,gl_Translatef),API("glTranslated",6,gl_Translated),
    API("glScalef",3,gl_Scalef),API("glRotatef",4,gl_Rotatef),API("glRotated",8,gl_Rotated),API("glOrtho",12,gl_Ortho),API("glFrustum",12,gl_Frustum),
    API("glPushMatrix",0,gl_PushMatrix),API("glPopMatrix",0,gl_PopMatrix),
    API("glGenTextures",2,gl_GenTextures),API("glDeleteTextures",2,gl_DeleteTextures),API("glBindTexture",2,gl_BindTexture),
    API("glTexImage2D",9,gl_TexImage2D),API("glTexSubImage2D",9,gl_TexSubImage2D),API("glTexParameteri",3,gl_TexParameteri),API("glTexParameterf",3,gl_TexParameterf),
    API("glTexEnvi",3,gl_TexEnvi),API("glTexEnvf",3,gl_TexEnvf),API("glPixelStorei",2,gl_PixelStorei),API("glPixelStoref",2,gl_noop),
    API("glGetError",0,gl_GetError),API("glGetString",1,gl_GetString),API("glGetIntegerv",2,gl_GetIntegerv),API("glGetFloatv",2,gl_GetFloatv),
    API("glEnableClientState",1,gl_EnableClientState),API("glDisableClientState",1,gl_DisableClientState),
    API("glVertexPointer",4,gl_VertexPointer),API("glColorPointer",4,gl_ColorPointer),API("glTexCoordPointer",4,gl_TexCoordPointer),
    API("glDrawArrays",3,gl_DrawArrays),API("glDrawElements",4,gl_DrawElements),API("glArrayElement",1,gl_ArrayElement),
    API("wglCreateContext",1,wgl_CreateContext),API("wglMakeCurrent",2,wgl_true),API("wglDeleteContext",1,wgl_true),API("wglGetCurrentContext",0,wgl_GetCurrent),
    API("wglGetCurrentDC",0,wgl_GetCurrent),API("wglGetProcAddress",1,gl_noop),API("wglSwapBuffers",1,gl_SwapBuffers),API("SwapBuffers",1,gl_SwapBuffers),
    API("ChoosePixelFormat",2,gl_ChoosePixelFormat),API("SetPixelFormat",3,wgl_true),API("DescribePixelFormat",4,gl_DescribePixelFormat),API("GetPixelFormat",1,gl_ChoosePixelFormat),
    {NULL,0,NULL}
};
