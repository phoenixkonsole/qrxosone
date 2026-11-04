/* Direct3D 8 and 9 for x86 programs on the host GPU: the fixed-function pipeline,
 * without lighting, shaders, cube or volume textures and DXT. COM objects live in x86
 * memory; their vtables point into a thunk range only the host door ever sees. A
 * method the host lacks returns D3DERR_INVALIDCALL with the right stack cleanup. */
#include "qrx_hostapi.h"
#include "qrx_hostgpu.h"
#include <stdlib.h>
#include <string.h>
#define DF(n) static uint32_t n(x86cpu *c,const uint32_t *a)
#define NOARGS (void)c;(void)a
#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
typedef struct { const char *name; int nargs; } VMethod;
#include "qrx_d3d_vtables.inc"

/* ---- x86 memory the host owns, obtained through the guest. A function that needs
 * memory asks for it and returns; the door then re-runs the same call. */
static uint32_t alloc_ready,alloc_ready_size;
uint32_t qrx_hostapi_alloc_request;
void qrx_hostapi_alloc_result(uint32_t addr,uint32_t size){alloc_ready=addr;alloc_ready_size=size;}
static uint32_t host_alloc(uint32_t size){
    if(alloc_ready&&alloc_ready_size==size){uint32_t p=alloc_ready;alloc_ready=0;return p;}
    qrx_hostapi_alloc_request=size;
    return 0;
}
#define NEED(var,size) uint32_t var=host_alloc(size);if(!var)return 0

/* ---- vtables */
#define THUNK_BASE 0x7F000000u
#define HOST_THUNK_FIRST 2600u
#define HOST_THUNK_LAST 4095u
const QRXHostApi *qrx_d3d_thunks[HOST_THUNK_LAST+1u];
static uint32_t next_thunk=HOST_THUNK_FIRST;
typedef struct { const char *iface,*method; QRXHostApiFn fn; } Impl;
static const Impl impls[];
static const QRXHostApi *make_entry(const char *iface,const VMethod *m){
    QRXHostApi *e=malloc(sizeof(*e));
    QRXHostApiFn fn=NULL;
    extern uint32_t qrx_d3d_unimpl(x86cpu*,const uint32_t*);
    for(const Impl *i=impls;i->iface;i++)
        if(!strcmp(i->method,m->name)&&(!i->iface[0]||!strcmp(i->iface,iface))){fn=i->fn;break;}
    e->name=m->name;e->nargs=m->nargs;e->fn=fn?fn:qrx_d3d_unimpl;
    return e;
}
typedef struct { const char *name; const VMethod *m; uint32_t vt; unsigned count; } VTable;
static VTable vtables[]={
    {"D3D9",vt_IDirect3D9,0,0},{"Dev9",vt_IDirect3DDevice9,0,0},{"VB9",vt_IDirect3DVertexBuffer9,0,0},{"IB9",vt_IDirect3DIndexBuffer9,0,0},{"Tex9",vt_IDirect3DTexture9,0,0},{"Surf9",vt_IDirect3DSurface9,0,0},
    {"D3D8",vt_IDirect3D8,0,0},{"Dev8",vt_IDirect3DDevice8,0,0},{"VB8",vt_IDirect3DVertexBuffer8,0,0},{"IB8",vt_IDirect3DIndexBuffer8,0,0},{"Tex8",vt_IDirect3DTexture8,0,0},{"Surf8",vt_IDirect3DSurface8,0,0},
};
enum { VT_D3D9,VT_DEV9,VT_VB9,VT_IB9,VT_TEX9,VT_SURF9,VT_D3D8,VT_DEV8,VT_VB8,VT_IB8,VT_TEX8,VT_SURF8 };
/* An object is { vtable, handle, refs }. The first request of an interface also
 * allocates its vtable, which then persists. */
static uint32_t ensure_vtable(x86cpu *c,unsigned which){
    VTable *v=&vtables[which];
    if(v->vt)return v->vt;
    unsigned n=0;
    while(v->m[n].name)n++;
    uint32_t mem=host_alloc(n*4u);
    if(!mem)return 0;
    for(unsigned i=0;i<n;i++){
        if(next_thunk>HOST_THUNK_LAST)break;
        qrx_d3d_thunks[next_thunk]=make_entry(v->name,&v->m[i]);
        x86_wr32(c,mem+i*4u,THUNK_BASE+next_thunk);
        next_thunk++;
    }
    v->vt=mem;v->count=n;
    return mem;
}
/* For function pointers handed out at run time, such as vkGetInstanceProcAddr's. */
uint32_t qrx_hostapi_thunk_for(const QRXHostApi *f){
    for(uint32_t i=HOST_THUNK_FIRST;i<next_thunk;i++)if(qrx_d3d_thunks[i]==f)return THUNK_BASE+i;
    if(next_thunk>HOST_THUNK_LAST)return 0;
    qrx_d3d_thunks[next_thunk]=f;
    return THUNK_BASE+next_thunk++;
}
uint32_t qrx_hostapi_host_alloc(uint32_t size){return host_alloc(size);}
static uint32_t new_object(x86cpu *c,unsigned which,uint32_t handle){
    uint32_t vt=ensure_vtable(c,which);
    if(!vt)return 0;
    uint32_t o=host_alloc(16);
    if(!o)return 0;
    x86_wr32(c,o,vt);x86_wr32(c,o+4,handle);x86_wr32(c,o+8,1);
    return o;
}
uint32_t qrx_d3d_unimpl(x86cpu *c,const uint32_t *a){NOARGS;return D3DERR_INVALIDCALL;}
#define HANDLE(obj) x86_rd32(c,(obj)+4u)

/* ---- device state */
typedef struct { float m[16]; } M4;
#define M_ID_INIT {{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}}
static M4 m_world=M_ID_INIT,m_view=M_ID_INIT,m_proj=M_ID_INIT;
static QRXHGState st;
static bool st_ready;
static unsigned bb_w=640,bb_h=480;
static uint32_t fvf;
static bool is8;
#define OBJ_MAX 16384u
typedef struct {
    int kind;                 /* 1 vertex buffer, 2 index buffer, 3 texture */
    uint32_t mem,size,format,w,h,pitch;
    uint32_t hg; bool linear,wrap_s,wrap_t;
} D3DObj;
static D3DObj objs[OBJ_MAX];
static uint32_t next_obj=1;
static uint32_t stream_vb,stream_stride,stream_offset,index_ib;
static int32_t base_vertex;
static uint32_t cur_tex_handle;
static bool tex_stage_disable,tex_stage_replace;
static void ensure_state(void){if(!st_ready){qrx_hg_state_default(&st);st.front_ccw=false;st.cull=HG_CULL_BACK;st.depth_test=true;st_ready=true;}}
/* D3D multiplies row vectors: v' = v * World * View * Proj. */
static void mul(M4 *r,const M4 *a,const M4 *b){
    M4 t;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++){
        float v=0;for(int k=0;k<4;k++)v+=a->m[i*4+k]*b->m[k*4+j];
        t.m[i*4+j]=v;
    }
    *r=t;
}
/* ---- IDirect3D8/9 */
DF(com_AddRef){uint32_t r=x86_rd32(c,a[0]+8)+1u;x86_wr32(c,a[0]+8,r);return r;}
DF(com_Release){uint32_t r=x86_rd32(c,a[0]+8);if(r)r--;x86_wr32(c,a[0]+8,r);return r;}
DF(com_QI){NOARGS;if(a[2])x86_wr32(c,a[2],0);return 0x80004002u;}
DF(d3d_one){NOARGS;return 1;}
DF(d3d_ok){NOARGS;return D3D_OK;}
DF(d3d_not_available){NOARGS;return D3DERR_NOTAVAILABLE;}
DF(GetAdapterIdentifier){
    /* drivers, description strings, then ids: enough to be all zero but a name */
    uint32_t out=is8?a[3]:a[3];
    for(uint32_t i=0;i<1100;i+=4)x86_wr32(c,out+i,0);
    static const char n[]="QRX ARMithlon host GPU";
    for(uint32_t i=0;i<sizeof(n);i++){x86_wr8(c,out+i,(uint8_t)n[i]);x86_wr8(c,out+512+i,(uint8_t)n[i]);}
    return D3D_OK;
}
static const unsigned modes[][2]={{640,480},{800,600},{1024,768},{1280,720}};
DF(GetAdapterModeCount){NOARGS;return 4;}
#define D3DFMT_X8R8G8B8 22u
DF(EnumAdapterModes9){
    /* this, adapter, format, mode, out */
    unsigned m=a[3]<4?a[3]:0;
    x86_wr32(c,a[4],modes[m][0]);x86_wr32(c,a[4]+4,modes[m][1]);x86_wr32(c,a[4]+8,60);x86_wr32(c,a[4]+12,D3DFMT_X8R8G8B8);
    return D3D_OK;
}
DF(EnumAdapterModes8){
    unsigned m=a[2]<4?a[2]:0;
    x86_wr32(c,a[3],modes[m][0]);x86_wr32(c,a[3]+4,modes[m][1]);x86_wr32(c,a[3]+8,60);x86_wr32(c,a[3]+12,D3DFMT_X8R8G8B8);
    return D3D_OK;
}
DF(GetAdapterDisplayMode){
    x86_wr32(c,a[2],640);x86_wr32(c,a[2]+4,480);x86_wr32(c,a[2]+8,60);x86_wr32(c,a[2]+12,D3DFMT_X8R8G8B8);
    return D3D_OK;
}
static void fill_caps(x86cpu *c,uint32_t caps){
    for(uint32_t i=0;i<304;i+=4)x86_wr32(c,caps+i,0);
    x86_wr32(c,caps,1);                       /* D3DDEVTYPE_HAL */
    x86_wr32(c,caps+28,0x00000040u);          /* DEVCAPS: HWRASTERIZATION */
    x86_wr32(c,caps+36+16,0x00000020u|0x00000002u);
}
DF(GetDeviceCapsD3D){fill_caps(c,a[3]);return D3D_OK;}
DF(GetDeviceCapsDev){fill_caps(c,a[1]);return D3D_OK;}
DF(CreateDevice){
    /* this, adapter, type, hwnd, flags, presentparams, out */
    uint32_t pp=a[5];
    bb_w=x86_rd32(c,pp);bb_h=x86_rd32(c,pp+4);
    if(!bb_w)bb_w=640;
    if(!bb_h)bb_h=480;
    is8=x86_rd32(c,a[0])==vtables[VT_D3D8].vt;
    uint32_t dev=new_object(c,is8?VT_DEV8:VT_DEV9,0);
    if(!dev)return 0;
    ensure_state();
    if(!qrx_hg_open(bb_w,bb_h))return D3DERR_NOTAVAILABLE;
    x86_wr32(c,a[6],dev);
    return D3D_OK;
}
DF(Direct3DCreate9){
    NOARGS;is8=false;
    return new_object(c,VT_D3D9,0);
}
DF(Direct3DCreate8){
    NOARGS;is8=true;
    return new_object(c,VT_D3D8,0);
}
/* ---- device */
DF(BeginEnd){NOARGS;return D3D_OK;}
DF(Present){NOARGS;qrx_hg_present();return D3D_OK;}
DF(Clear){
    /* this, count, rects, flags, color, z, stencil */
    NOARGS;
    uint32_t col=a[4];
    qrx_hg_clear((a[3]&1u)!=0,(a[3]&2u)!=0,((col>>16)&255u)/255.f,((col>>8)&255u)/255.f,(col&255u)/255.f,(col>>24)/255.f,api_float(a[5]));
    return D3D_OK;
}
DF(SetViewport){
    ensure_state();
    st.vx=(int)x86_rd32(c,a[1]);st.vy=(int)x86_rd32(c,a[1]+4);st.vw=(int)x86_rd32(c,a[1]+8);st.vh=(int)x86_rd32(c,a[1]+12);
    return D3D_OK;
}
DF(SetTransform){
    M4 m;
    for(uint32_t i=0;i<16;i++)m.m[i]=api_float(x86_rd32(c,a[2]+i*4u));
    if(a[1]==2)m_view=m;
    else if(a[1]==3)m_proj=m;
    else if(a[1]==256)m_world=m;
    return D3D_OK;
}
static uint8_t blend_of(uint32_t v){
    switch(v){
    case 1:return HG_BLEND_ZERO;case 2:return HG_BLEND_ONE;case 3:return HG_BLEND_SRC_COLOR;case 4:return HG_BLEND_INV_SRC_COLOR;
    case 5:return HG_BLEND_SRC_ALPHA;case 6:return HG_BLEND_INV_SRC_ALPHA;case 7:return HG_BLEND_DST_ALPHA;case 8:return HG_BLEND_INV_DST_ALPHA;
    case 9:return HG_BLEND_DST_COLOR;case 10:return HG_BLEND_INV_DST_COLOR;default:return HG_BLEND_ONE;
    }
}
DF(SetRenderState){
    NOARGS;ensure_state();
    uint32_t v=a[2];
    switch(a[1]){
    case 7:st.depth_test=v!=0;break;
    case 14:st.depth_write=v!=0;break;
    case 15:st.alpha_test=v!=0;break;
    case 19:st.src=blend_of(v);break;
    case 20:st.dst=blend_of(v);break;
    case 22:st.cull=v==1?HG_CULL_NONE:v==3?HG_CULL_BACK:HG_CULL_FRONT;break; /* front is clockwise */
    case 23:st.depth_func=(uint8_t)((v-1u)&7u);break;
    case 24:st.alpha_ref=(v&255u)/255.f;break;
    case 25:st.alpha_func=(uint8_t)((v-1u)&7u);break;
    case 27:st.blend=v!=0;break;
    default:break;
    }
    return D3D_OK;
}
static void sampler_state(uint32_t type,uint32_t v,bool stage_numbers){
    D3DObj *o=cur_tex_handle&&cur_tex_handle<OBJ_MAX?&objs[cur_tex_handle]:NULL;
    if(!o||!o->hg)return;
    uint32_t addr_u=stage_numbers?13u:1u,addr_v=stage_numbers?14u:2u,mag=stage_numbers?16u:5u;
    if(type==addr_u)o->wrap_s=v==1;
    else if(type==addr_v)o->wrap_t=v==1;
    else if(type==mag)o->linear=v>=2;
    else return;
    qrx_hg_texture_sampler(o->hg,o->linear,o->wrap_s,o->wrap_t);
}
DF(SetTextureStageState){
    NOARGS;ensure_state();
    if(a[1]!=0)return D3D_OK;
    if(a[2]==1){tex_stage_disable=a[3]==1;tex_stage_replace=a[3]==2;st.env=tex_stage_replace?HG_ENV_REPLACE:HG_ENV_MODULATE;}
    if(is8)sampler_state(a[2],a[3],true);
    return D3D_OK;
}
DF(SetSamplerState){NOARGS;if(a[1]==0)sampler_state(a[2],a[3],false);return D3D_OK;}
DF(SetTexture){
    if(a[1]!=0)return D3D_OK;
    cur_tex_handle=a[2]?HANDLE(a[2]):0;
    return D3D_OK;
}
DF(SetFVF){NOARGS;fvf=a[1];return D3D_OK;}
DF(GetFVF){x86_wr32(c,a[1],fvf);return D3D_OK;}
DF(SetVertexShader8){NOARGS;if(a[1]<0x10000u)fvf=a[1];return D3D_OK;}
DF(TestCooperativeLevel){NOARGS;return D3D_OK;}
/* ---- geometry */
static unsigned fvf_stride(uint32_t f){
    unsigned s=0;
    switch(f&0xEu){case 2:s=12;break;case 4:s=16;break;default:s=12+4u*(((f&0xEu)-6u)/2u+1u);break;}
    if(f&0x10u)s+=12;
    if(f&0x20u)s+=4;
    if(f&0x40u)s+=4;
    if(f&0x80u)s+=4;
    s+=8u*((f>>8)&15u);
    return s;
}
static void read_vertex(x86cpu *c,uint32_t p,const M4 *wvp,QRXHGVertex *v){
    float x=api_float(x86_rd32(c,p)),y=api_float(x86_rd32(c,p+4)),z=api_float(x86_rd32(c,p+8));
    uint32_t q=p+12;
    if((fvf&0xEu)==4){
        float rhw=api_float(x86_rd32(c,q));q+=4;
        float w=rhw>0?1.f/rhw:1.f;
        float vw=st.vw>0?(float)st.vw:(float)bb_w,vh=st.vh>0?(float)st.vh:(float)bb_h;
        v->x=(2.f*x/vw-1.f)*w;v->y=(1.f-2.f*y/vh)*w;v->z=z*w;v->w=w;
    }else{
        const float *m=wvp->m;
        v->x=x*m[0]+y*m[4]+z*m[8]+m[12];
        v->y=x*m[1]+y*m[5]+z*m[9]+m[13];
        v->z=x*m[2]+y*m[6]+z*m[10]+m[14];
        v->w=x*m[3]+y*m[7]+z*m[11]+m[15];
    }
    if(fvf&0x10u)q+=12;
    if(fvf&0x20u)q+=4;
    v->r=v->g=v->b=v->a=1.f;
    if(fvf&0x40u){
        uint32_t col=x86_rd32(c,q);q+=4;
        v->r=((col>>16)&255u)/255.f;v->g=((col>>8)&255u)/255.f;v->b=(col&255u)/255.f;v->a=(col>>24)/255.f;
    }
    if(fvf&0x80u)q+=4;
    v->u=v->v=0;
    if((fvf>>8)&15u){v->u=api_float(x86_rd32(c,q));v->v=api_float(x86_rd32(c,q+4));}
}
#define MAX_PRIM_VERTS 196608u
static QRXHGVertex *scratch_tri;
static uint32_t legacy_tex_override;
static bool legacy_tex_active;
/* type: 4 list, 5 strip, 6 fan. Vertices come from a reader. */
static uint32_t index_count(uint32_t type,uint32_t prims){
    switch(type){case 4:return prims*3;case 5:case 6:return prims+2;default:return 0;}
}
static void draw_verts(x86cpu *c,uint32_t type,uint32_t prims,uint32_t base,uint32_t stride,uint32_t indices,bool idx32,uint32_t first){
    ensure_state();
    if(!scratch_tri)scratch_tri=malloc(MAX_PRIM_VERTS*sizeof(QRXHGVertex));
    M4 wv,wvp;
    mul(&wv,&m_world,&m_view);mul(&wvp,&wv,&m_proj);
    uint32_t n=index_count(type,prims);
    if(!n||prims*3u>MAX_PRIM_VERTS)return;
    uint32_t out=0;
    QRXHGVertex a,b,d;
#define FETCH(i,dst) do{ \
        uint32_t ii=(i); \
        if(indices)ii=idx32?x86_rd32(c,indices+ii*4u):x86_rd16(c,indices+ii*2u); \
        else ii+=first; \
        read_vertex(c,base+ii*stride,&wvp,&(dst)); }while(0)
    if(type==4){
        for(uint32_t i=0;i<n;i++)FETCH(i,scratch_tri[out++]);
    }else if(type==5){
        FETCH(0,a);FETCH(1,b);
        for(uint32_t i=2;i<n;i++){
            FETCH(i,d);
            if(i&1u){scratch_tri[out++]=b;scratch_tri[out++]=a;scratch_tri[out++]=d;}
            else{scratch_tri[out++]=a;scratch_tri[out++]=b;scratch_tri[out++]=d;}
            a=b;b=d;
        }
    }else{
        FETCH(0,a);FETCH(1,b);
        for(uint32_t i=2;i<n;i++){
            FETCH(i,d);
            scratch_tri[out++]=a;scratch_tri[out++]=b;scratch_tri[out++]=d;
            b=d;
        }
    }
#undef FETCH
    st.texture=0;
    if(legacy_tex_active){if(!tex_stage_disable)st.texture=legacy_tex_override;}
    else if(cur_tex_handle&&cur_tex_handle<OBJ_MAX&&!tex_stage_disable)st.texture=objs[cur_tex_handle].hg;
    qrx_hg_triangles(&st,scratch_tri,out);
}
/* For Direct3D 5 to 7: a chosen texture and 16-bit indices, with the array as vertex base. */
static void draw_verts_tex(x86cpu *c,uint32_t type,uint32_t prims,uint32_t verts,uint32_t stride,uint32_t idx,uint32_t tex){
    legacy_tex_override=tex;legacy_tex_active=true;
    draw_verts(c,type,prims,verts,stride,idx,false,0);
    legacy_tex_active=false;
}
DF(DrawPrimitiveUP){draw_verts(c,a[1],a[2],a[3],a[4],0,false,0);return D3D_OK;}
DF(DrawIndexedPrimitiveUP9){
    /* this, type, minindex, numverts, prims, indices, fmt, vertices, stride */
    draw_verts(c,a[1],a[4],a[7],a[8],a[5],a[6]==102,0);
    return D3D_OK;
}
DF(DrawPrimitive){
    if(!stream_vb)return D3DERR_INVALIDCALL;
    draw_verts(c,a[1],a[3],objs[stream_vb].mem+stream_offset,stream_stride,0,false,a[2]);
    return D3D_OK;
}
static void draw_indexed(x86cpu *c,uint32_t type,int32_t base_v,uint32_t start,uint32_t prims){
    if(!stream_vb||!index_ib)return;
    D3DObj *ib=&objs[index_ib];
    bool idx32=ib->format==102;
    draw_verts(c,type,prims,objs[stream_vb].mem+stream_offset+(uint32_t)base_v*stream_stride,stream_stride,ib->mem+start*(idx32?4u:2u),idx32,0);
}
DF(DrawIndexedPrimitive9){draw_indexed(c,a[1],(int32_t)a[2],a[5],a[6]);return D3D_OK;}
DF(DrawIndexedPrimitive8){draw_indexed(c,a[1],base_vertex,a[4],a[5]);return D3D_OK;}
DF(SetStreamSource9){
    if(a[1]!=0)return D3D_OK;
    stream_vb=a[2]?HANDLE(a[2]):0;stream_offset=a[3];stream_stride=a[4];
    return D3D_OK;
}
DF(SetStreamSource8){
    if(a[1]!=0)return D3D_OK;
    stream_vb=a[2]?HANDLE(a[2]):0;stream_offset=0;stream_stride=a[3];
    return D3D_OK;
}
DF(SetIndices9){index_ib=a[1]?HANDLE(a[1]):0;return D3D_OK;}
DF(SetIndices8){index_ib=a[1]?HANDLE(a[1]):0;base_vertex=(int32_t)a[2];return D3D_OK;}
/* ---- resources */
static uint32_t next_handle(void){return next_obj<OBJ_MAX?next_obj++:0;}
static uint32_t pending_mem,pending_size;
static uint32_t alloc_block(uint32_t size){
    if(pending_mem&&pending_size==size)return pending_mem;
    uint32_t m=host_alloc(size);
    if(m){pending_mem=m;pending_size=size;}
    return m;
}
static uint32_t create_buffer(x86cpu *c,uint32_t size,uint32_t format,unsigned vt,int kind,uint32_t out){
    uint32_t mem=alloc_block(size?size:16u);
    if(!mem)return 0;
    /* new_object may still ask for memory: pending_mem stays for the re-run */
    uint32_t h=next_obj;
    uint32_t o=new_object(c,vt,h);
    if(!o)return 0;
    next_obj++;
    objs[h]=(D3DObj){.kind=kind,.mem=mem,.size=size,.format=format};
    pending_mem=0;
    x86_wr32(c,out,o);
    return D3D_OK;
}
DF(CreateVertexBuffer9){return create_buffer(c,a[1],0,VT_VB9,1,a[5]);}
DF(CreateVertexBuffer8){return create_buffer(c,a[1],0,VT_VB8,1,a[5]);}
DF(CreateIndexBuffer9){return create_buffer(c,a[1],a[3],VT_IB9,2,a[5]);}
DF(CreateIndexBuffer8){return create_buffer(c,a[1],a[3],VT_IB8,2,a[5]);}
DF(Buffer_Lock9){
    /* this, offset, size, out, flags */
    D3DObj *o=&objs[HANDLE(a[0])];
    x86_wr32(c,a[3],o->mem+a[1]);
    return D3D_OK;
}
static unsigned fmt_bytes(uint32_t f){
    switch(f){case 21:case 22:return 4;case 23:case 25:case 26:return 2;case 50:return 1;default:return 0;}
}
static uint32_t create_texture(x86cpu *c,uint32_t w,uint32_t h,uint32_t fmt,unsigned vt,uint32_t out){
    unsigned bytes=fmt_bytes(fmt);
    if(!bytes||!w||!h)return D3DERR_INVALIDCALL;
    uint32_t mem=alloc_block(w*h*bytes);
    if(!mem)return 0;
    uint32_t handle=next_obj;
    uint32_t o=new_object(c,vt,handle);
    if(!o)return 0;
    next_obj++;
    objs[handle]=(D3DObj){.kind=3,.mem=mem,.size=w*h*bytes,.format=fmt,.w=w,.h=h,.pitch=w*bytes,.linear=true,.wrap_s=true,.wrap_t=true};
    objs[handle].hg=qrx_hg_texture(0,w,h,NULL,true,true,true);
    pending_mem=0;
    x86_wr32(c,out,o);
    return D3D_OK;
}
/* this,w,h,levels,usage,format,pool,out[,shared] */
DF(CreateTexture){return create_texture(c,a[1],a[2],a[5],is8?VT_TEX8:VT_TEX9,a[7]);}
DF(Texture_LockRect){
    /* this, level, out{pitch,bits}, rect, flags. Only level 0 reaches the GPU: lower
     * levels share its memory, which holds enough room for them. */
    D3DObj *o=&objs[HANDLE(a[0])];
    x86_wr32(c,a[2],o->pitch);x86_wr32(c,a[2]+4,o->mem);
    return D3D_OK;
}
DF(Texture_UnlockRect){
    if(a[1]!=0)return D3D_OK;
    D3DObj *o=&objs[HANDLE(a[0])];
    uint8_t *px=malloc((size_t)o->w*o->h*4u);
    if(!px)return D3D_OK;
    for(uint32_t i=0;i<o->w*o->h;i++){
        uint8_t *d=px+(size_t)i*4u;
        uint32_t r=255,g=255,b=255,al=255;
        switch(o->format){
        case 21:case 22:{uint32_t v=x86_rd32(c,o->mem+i*4u);b=v&255u;g=(v>>8)&255u;r=(v>>16)&255u;al=o->format==21?v>>24:255u;break;}
        case 23:{uint32_t v=x86_rd16(c,o->mem+i*2u);r=(v>>11)*255/31;g=((v>>5)&63)*255/63;b=(v&31)*255/31;break;}
        case 25:{uint32_t v=x86_rd16(c,o->mem+i*2u);al=(v>>15)?255:0;r=((v>>10)&31)*255/31;g=((v>>5)&31)*255/31;b=(v&31)*255/31;break;}
        case 26:{uint32_t v=x86_rd16(c,o->mem+i*2u);al=(v>>12)*17;r=((v>>8)&15)*17;g=((v>>4)&15)*17;b=(v&15)*17;break;}
        case 50:r=g=b=x86_rd8(c,o->mem+i);break;
        default:break;
        }
        d[0]=(uint8_t)r;d[1]=(uint8_t)g;d[2]=(uint8_t)b;d[3]=(uint8_t)al;
    }
    qrx_hg_texture(o->hg,o->w,o->h,px,o->linear,o->wrap_s,o->wrap_t);
    free(px);
    return D3D_OK;
}
/* ---- which method runs what. An empty interface name matches them all. */

/* ---- Direct3D 5, 6, 7: called through thunks from objects that live in the guest. */
#define D3DL_ARGS x86cpu *c,const uint32_t *a
typedef struct { uint32_t surface,gen; uint32_t hg; } LegacyTex;
#define LTEX_MAX 2048u
static LegacyTex ltex[LTEX_MAX];
static uint32_t ltex_n;
static uint8_t *ltex_buf;
static bool lg_colorkey_on;
static unsigned lg_tex_w,lg_tex_h;
static inline unsigned mask_shift(uint32_t m){return m?(unsigned)__builtin_ctz(m):0u;}
static inline uint32_t mask_expand(uint32_t v,uint32_t m){
    if(!m)return 255u;
    unsigned sh=mask_shift(m);
    uint32_t top=m>>sh,val=(v&m)>>sh;
    return top?(val*255u+top/2u)/top:255u;
}
/* A DirectDraw surface as a host texture, uploaded again whenever its generation moved. */
static uint32_t legacy_texture(x86cpu *c,uint32_t surface){
    if(!surface)return 0;
    uint32_t gen=x86_rd32(c,surface+28);
    LegacyTex *t=NULL;
    for(uint32_t i=0;i<ltex_n;i++)if(ltex[i].surface==surface){t=&ltex[i];break;}
    if(!t){
        if(ltex_n>=LTEX_MAX)return 0;
        t=&ltex[ltex_n++];t->surface=surface;t->hg=0;t->gen=gen-1u;
    }
    if(t->hg&&t->gen==gen)return t->hg;
    uint32_t w=x86_rd32(c,surface+8),h=x86_rd32(c,surface+12),pitch=x86_rd32(c,surface+16),bpp=x86_rd32(c,surface+20),mem=x86_rd32(c,surface+24);
    if(!w||!h||w>4096||h>4096||!mem)return 0;
    uint32_t rm=x86_rd32(c,surface+32),gm=x86_rd32(c,surface+36),bm=x86_rd32(c,surface+40),am=x86_rd32(c,surface+44);
    uint32_t ck_on=x86_rd32(c,surface+48),ck=x86_rd32(c,surface+52),pal=x86_rd32(c,surface+56);
    if(!rm&&!gm&&!bm){
        if(bpp==16){rm=0xF800;gm=0x07E0;bm=0x001F;}
        else{rm=0xFF0000;gm=0xFF00;bm=0xFF;}
    }
    static size_t cap;
    if(w*h*4u>cap){free(ltex_buf);cap=w*h*4u;ltex_buf=malloc(cap);}
    if(!ltex_buf)return 0;
    uint8_t *d=ltex_buf;
    for(uint32_t y=0;y<h;y++){
        uint32_t row=mem+y*pitch;
        for(uint32_t x=0;x<w;x++,d+=4){
            uint32_t px;
            if(bpp==8){
                uint32_t i=x86_rd8(c,row+x);
                uint32_t e=pal?x86_rd32(c,pal+i*4u):i*0x010101u;
                d[0]=(uint8_t)(e>>16);d[1]=(uint8_t)(e>>8);d[2]=(uint8_t)e;
                d[3]=ck_on&&i==ck?0:255;
                continue;
            }
            px=bpp==16?x86_rd16(c,row+x*2u):bpp==32?x86_rd32(c,row+x*4u):(uint32_t)x86_rd8(c,row+x*3u)|(uint32_t)x86_rd8(c,row+x*3u+1u)<<8|(uint32_t)x86_rd8(c,row+x*3u+2u)<<16;
            d[0]=(uint8_t)mask_expand(px,rm);d[1]=(uint8_t)mask_expand(px,gm);d[2]=(uint8_t)mask_expand(px,bm);
            d[3]=am?(uint8_t)mask_expand(px,am):255;
            if(ck_on&&px==ck)d[3]=0;
        }
    }
    t->hg=qrx_hg_texture(t->hg,w,h,ltex_buf,true,true,true);
    t->gen=gen;
    return t->hg;
}
static uint32_t legacy_cur;
static void legacy_bind(x86cpu *c,uint32_t surface){legacy_cur=surface;(void)c;}
DF(L_Open){NOARGS;bb_w=a[0];bb_h=a[1];qrx_hg_open(bb_w,bb_h);ensure_state();is8=false;return D3D_OK;}
DF(L_Present){NOARGS;qrx_hg_present();return D3D_OK;}
DF(L_Scene){NOARGS;return D3D_OK;}
DF(L_Viewport){NOARGS;ensure_state();st.vx=(int)a[0];st.vy=(int)a[1];st.vw=(int)a[2];st.vh=(int)a[3];return D3D_OK;}
DF(L_SetTransform){
    /* D3DTRANSFORMSTATE_WORLD 1, VIEW 2, PROJECTION 3 */
    M4 m;
    for(uint32_t i=0;i<16;i++)m.m[i]=api_float(x86_rd32(c,a[2]+i*4u));
    if(a[1]==2)m_view=m;
    else if(a[1]==3)m_proj=m;
    else if(a[1]==1||a[1]==256)m_world=m;
    return D3D_OK;
}
DF(L_SetRenderState){
    ensure_state();
    uint32_t v=a[2];
    switch(a[1]){
    case 1:legacy_bind(c,v);break;                        /* TEXTUREHANDLE */
    case 21:st.env=v==1?HG_ENV_REPLACE:v==2?HG_ENV_MODULATE:v==3?HG_ENV_DECAL:HG_ENV_MODULATE;break; /* TEXTUREMAPBLEND */
    case 41:lg_colorkey_on=v!=0;break;                    /* COLORKEYENABLE */
    case 7:st.depth_test=v!=0;break;
    case 14:st.depth_write=v!=0;break;
    case 15:st.alpha_test=v!=0;break;
    case 19:st.src=blend_of(v);break;
    case 20:st.dst=blend_of(v);break;
    case 22:st.cull=v==1?HG_CULL_NONE:v==3?HG_CULL_BACK:HG_CULL_FRONT;break;
    case 23:st.depth_func=(uint8_t)((v-1u)&7u);break;
    case 24:st.alpha_ref=(v&255u)/255.f;break;
    case 25:st.alpha_func=(uint8_t)((v-1u)&7u);break;
    case 27:st.blend=v!=0;break;
    default:break;
    }
    return D3D_OK;
}
DF(L_SetTexture3){if(a[1]==0)legacy_bind(c,a[2]?x86_rd32(c,a[2]+8):0);return D3D_OK;}
DF(L_SetTexture7){if(a[1]==0)legacy_bind(c,a[2]);return D3D_OK;}
DF(L_TextureStage){
    NOARGS;ensure_state();
    if(a[1]==0&&a[2]==1){tex_stage_disable=a[3]==1;tex_stage_replace=a[3]==2;st.env=tex_stage_replace?HG_ENV_REPLACE:HG_ENV_MODULATE;}
    return D3D_OK;
}
static void legacy_draw(x86cpu *c,uint32_t type,uint32_t f,uint32_t verts,uint32_t stride,uint32_t nverts,uint32_t idx,uint32_t nidx){
    if(type<4||type>6)return;                 /* points and lines are not drawn */
    uint32_t n=idx?nidx:nverts;
    uint32_t prims=type==4?n/3u:n>=2u?n-2u:0u;
    if(!prims)return;
    fvf=f;
    if(!stride)stride=fvf_stride(f);
    uint32_t saved=cur_tex_handle;
    cur_tex_handle=0;
    ensure_state();
    uint32_t tex=legacy_texture(c,legacy_cur);
    bool at=st.alpha_test;uint8_t af=st.alpha_func;float ar=st.alpha_ref;
    if(lg_colorkey_on&&tex){st.alpha_test=true;st.alpha_func=HG_CMP_GREATER;st.alpha_ref=0;}
    int vx=st.vx,vy=st.vy,vw=st.vw,vh=st.vh;
    /* Transformed vertices are screen coordinates: the viewport does not apply. */
    if((f&0xEu)==4){st.vx=st.vy=st.vw=st.vh=0;}
    draw_verts_tex(c,type,prims,verts,stride,idx,tex);
    st.vx=vx;st.vy=vy;st.vw=vw;st.vh=vh;
    st.alpha_test=at;st.alpha_func=af;st.alpha_ref=ar;
    cur_tex_handle=saved;
}
static uint32_t vtype_fvf(uint32_t t){return t==3?0x1C4u:t==2?0x1E2u:0x112u;}
DF(L_Draw2){legacy_draw(c,a[1],vtype_fvf(a[2]),a[3],0,a[4],0,0);return D3D_OK;}
DF(L_DrawIndexed2){legacy_draw(c,a[1],vtype_fvf(a[2]),a[3],0,a[4],a[5],a[6]);return D3D_OK;}
DF(L_DrawFVF){legacy_draw(c,a[1],a[2],a[3],0,a[4],0,0);return D3D_OK;}
DF(L_DrawIndexedFVF){legacy_draw(c,a[1],a[2],a[3],0,a[4],a[5],a[6]);return D3D_OK;}
/* Vertex buffers: +8 memory, +12 fvf, +20 stride */
DF(L_DrawVB){
    uint32_t vb=a[2],stride=x86_rd32(c,vb+20);
    legacy_draw(c,a[1],x86_rd32(c,vb+12),x86_rd32(c,vb+8)+a[3]*stride,stride,a[4],0,0);
    return D3D_OK;
}
DF(L_DrawIndexedVB3){legacy_draw(c,a[1],x86_rd32(c,a[2]+12),x86_rd32(c,a[2]+8),x86_rd32(c,a[2]+20),0,a[3],a[4]);return D3D_OK;}
DF(L_DrawIndexedVB7){
    uint32_t vb=a[2],stride=x86_rd32(c,vb+20);
    legacy_draw(c,a[1],x86_rd32(c,vb+12),x86_rd32(c,vb+8)+a[3]*stride,stride,a[4],a[5],a[6]);
    return D3D_OK;
}
static const Impl impls[]={
    {"","QueryInterface",com_QI},{"","AddRef",com_AddRef},{"","Release",com_Release},
    {"D3D9","GetAdapterCount",d3d_one},{"D3D8","GetAdapterCount",d3d_one},{"D3D9","GetAdapterIdentifier",GetAdapterIdentifier},{"D3D8","GetAdapterIdentifier",GetAdapterIdentifier},
    {"D3D9","GetAdapterModeCount",GetAdapterModeCount},{"D3D8","GetAdapterModeCount",GetAdapterModeCount},{"D3D9","EnumAdapterModes",EnumAdapterModes9},{"D3D8","EnumAdapterModes",EnumAdapterModes8},
    {"D3D9","GetAdapterDisplayMode",GetAdapterDisplayMode},{"D3D8","GetAdapterDisplayMode",GetAdapterDisplayMode},
    {"D3D9","CheckDeviceType",d3d_ok},{"D3D8","CheckDeviceType",d3d_ok},{"D3D9","CheckDeviceFormat",d3d_ok},{"D3D8","CheckDeviceFormat",d3d_ok},
    {"D3D9","CheckDepthStencilMatch",d3d_ok},{"D3D8","CheckDepthStencilMatch",d3d_ok},{"D3D9","CheckDeviceMultiSampleType",d3d_not_available},{"D3D8","CheckDeviceMultiSampleType",d3d_not_available},
    {"D3D9","GetDeviceCaps",GetDeviceCapsD3D},{"D3D8","GetDeviceCaps",GetDeviceCapsD3D},{"Dev9","GetDeviceCaps",GetDeviceCapsDev},{"Dev8","GetDeviceCaps",GetDeviceCapsDev},
    {"D3D9","CreateDevice",CreateDevice},{"D3D8","CreateDevice",CreateDevice},
    {"","BeginScene",BeginEnd},{"","EndScene",BeginEnd},{"","Present",Present},{"","Clear",Clear},{"","SetViewport",SetViewport},{"","SetTransform",SetTransform},
    {"","SetRenderState",SetRenderState},{"","SetTextureStageState",SetTextureStageState},{"","SetSamplerState",SetSamplerState},{"","SetTexture",SetTexture},
    {"","SetFVF",SetFVF},{"","GetFVF",GetFVF},{"Dev8","SetVertexShader",SetVertexShader8},{"Dev9","SetVertexShader",d3d_ok},{"","TestCooperativeLevel",TestCooperativeLevel},
    {"","DrawPrimitiveUP",DrawPrimitiveUP},{"Dev9","DrawIndexedPrimitiveUP",DrawIndexedPrimitiveUP9},{"","DrawPrimitive",DrawPrimitive},
    {"Dev9","DrawIndexedPrimitive",DrawIndexedPrimitive9},{"Dev8","DrawIndexedPrimitive",DrawIndexedPrimitive8},
    {"Dev9","SetStreamSource",SetStreamSource9},{"Dev8","SetStreamSource",SetStreamSource8},{"Dev9","SetIndices",SetIndices9},{"Dev8","SetIndices",SetIndices8},
    {"Dev9","CreateVertexBuffer",CreateVertexBuffer9},{"Dev8","CreateVertexBuffer",CreateVertexBuffer8},{"Dev9","CreateIndexBuffer",CreateIndexBuffer9},{"Dev8","CreateIndexBuffer",CreateIndexBuffer8},
    {"","CreateTexture",CreateTexture},{"VB9","Lock",Buffer_Lock9},{"IB9","Lock",Buffer_Lock9},{"VB8","Lock",Buffer_Lock9},{"IB8","Lock",Buffer_Lock9},{"VB9","Unlock",d3d_ok},{"IB9","Unlock",d3d_ok},{"VB8","Unlock",d3d_ok},{"IB8","Unlock",d3d_ok},
    {"","LockRect",Texture_LockRect},{"","UnlockRect",Texture_UnlockRect},{"","PreLoad",d3d_ok},{"","SetLight",d3d_ok},{"","LightEnable",d3d_ok},{"","SetMaterial",d3d_ok},
    {NULL,NULL,NULL}
};
const QRXHostApi qrx_api_d3d[]={
    {"D3DL_Open",2,L_Open},{"D3DL_Present",1,L_Present},{"D3DL_Scene",1,L_Scene},{"D3DL_Viewport",4,L_Viewport},{"D3DL_Clear",7,Clear},
    {"D3DL_SetTransform",3,L_SetTransform},{"D3DL_SetRenderState",3,L_SetRenderState},{"D3DL_SetTexture3",3,L_SetTexture3},{"D3DL_SetTexture7",3,L_SetTexture7},
    {"D3DL_TextureStage",4,L_TextureStage},{"D3DL_Draw2",6,L_Draw2},{"D3DL_DrawIndexed2",8,L_DrawIndexed2},{"D3DL_DrawFVF",6,L_DrawFVF},
    {"D3DL_DrawIndexedFVF",8,L_DrawIndexedFVF},{"D3DL_DrawVB",6,L_DrawVB},{"D3DL_DrawIndexedVB3",6,L_DrawIndexedVB3},{"D3DL_DrawIndexedVB7",8,L_DrawIndexedVB7},
    {"Direct3DCreate9",1,Direct3DCreate9},{"Direct3DCreate8",1,Direct3DCreate8},
    {NULL,0,NULL}
};
