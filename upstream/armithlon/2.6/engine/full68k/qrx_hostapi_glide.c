#include <math.h>
/* 3dfx Glide 2.x and 3.x for x86 programs on the host GPU. Vertices come in screen
 * space; the combine units reduce to texture on/off and modulate/replace. */
#include "qrx_hostapi.h"
#include "qrx_hostgpu.h"
#include <stdlib.h>
#include <string.h>
#define GF(n) static uint32_t n(x86cpu *c,const uint32_t *a)
#define NOARGS (void)c;(void)a
static QRXHGState st;
static unsigned win_w=640,win_h=480;
static bool origin_lower,glide3;
static bool stw_tmu0,w_buffer;      /* the texture unit has its own 1/w; depth comes from 1/w */
/* Vertex layout: Glide 2's fixed struct until grVertexLayout says otherwise. */
static struct { int xy,z,q,a,rgb,pargb,st0,q0; } lay={0,24,32,28,12,-1,36,44};
static bool tex_on,const_color,const_alpha;
static float constant[4]={1,1,1,1};
static float clip_scale_s=1/256.f,clip_scale_t=1/256.f;
static const unsigned res_w[]={320,320,400,512,640,640,640,640,800,960,856,512,1024,1280,1600};
static const unsigned res_h[]={200,240,256,384,200,350,400,480,600,720,480,256,768,1024,1200};
GF(gr_ok){NOARGS;return 1;}
GF(gr_noop){NOARGS;return 0;}
GF(grSstWinOpen){
    NOARGS;
    unsigned r=a[1]<15?a[1]:7;
    win_w=res_w[r];win_h=res_h[r];
    origin_lower=a[4]==1;
    qrx_hg_state_default(&st);
    st.front_ccw=false;
    return qrx_hg_open(win_w,win_h)?1u:0u;
}
GF(grSstQueryHardware){
    /* GrHwConfiguration: num_sst, then a Voodoo2-like SST: 12 MB frame, 2 TMUs */
    for(uint32_t i=0;i<64;i+=4)x86_wr32(c,a[0]+i,0);
    x86_wr32(c,a[0],1);x86_wr32(c,a[0]+4,1);
    x86_wr32(c,a[0]+8,4);x86_wr32(c,a[0]+12,1);x86_wr32(c,a[0]+16,2);
    return 1;
}
GF(grSstQueryBoards){x86_wr32(c,a[0],1);return 1;}
GF(grSstScreenWidth){NOARGS;return win_w;}
GF(grSstScreenHeight){NOARGS;return win_h;}
GF(grSstOrigin){NOARGS;origin_lower=a[0]==1;return 0;}
GF(grBufferClear){
    NOARGS;
    float z=(a[2]&0xFFFFu)/65535.f;
    qrx_hg_clear(true,true,((a[0]>>16)&255u)/255.f,((a[0]>>8)&255u)/255.f,(a[0]&255u)/255.f,(a[1]&255u)/255.f,z);
    return 0;
}
GF(grBufferSwap){NOARGS;qrx_hg_present();return 0;}
static inline float vf(x86cpu *c,uint32_t p,int off,float def){return off<0?def:api_float(x86_rd32(c,p+(uint32_t)off));}
static void make(x86cpu *c,uint32_t p,QRXHGVertex *v){
    float x=vf(c,p,lay.xy,0),y=api_float(x86_rd32(c,p+(uint32_t)lay.xy+4u));
    /* The chip keeps 12 bits of pixel position. Glide 2 programs add 786432 to snap a float to
       its grid, as the 3Dfx guide tells them to, and rely on that. */
    if(x>=4096.f||x<=-4096.f)x-=4096.f*floorf(x/4096.f);
    if(y>=4096.f||y<=-4096.f)y-=4096.f*floorf(y/4096.f);
    float oow=vf(c,p,lay.q,1),z=vf(c,p,lay.z,0)/65535.f;
    if(oow<=0)oow=1;
    float w=1.f/oow;
    /* A W buffer does not look at ooz, which programs then leave unset. Nearer is smaller. */
    if(w_buffer)z=oow<1.f?1.f-oow:0.f;
    float nx=2.f*x/(float)win_w-1.f,ny=2.f*y/(float)win_h-1.f;
    if(!origin_lower)ny=-ny;
    v->x=nx*w;v->y=ny*w;v->z=z*w;v->w=w;
    if(const_color){v->r=constant[0];v->g=constant[1];v->b=constant[2];}
    else if(lay.pargb>=0){
        uint32_t col=x86_rd32(c,p+(uint32_t)lay.pargb);
        v->r=((col>>16)&255u)/255.f;v->g=((col>>8)&255u)/255.f;v->b=(col&255u)/255.f;v->a=(col>>24)/255.f;
    }else{
        v->r=vf(c,p,lay.rgb,255)/255.f;
        v->g=lay.rgb<0?1.f:api_float(x86_rd32(c,p+(uint32_t)lay.rgb+4u))/255.f;
        v->b=lay.rgb<0?1.f:api_float(x86_rd32(c,p+(uint32_t)lay.rgb+8u))/255.f;
    }
    if(lay.pargb<0)v->a=vf(c,p,lay.a,255)/255.f;
    if(const_alpha)v->a=constant[3];
    /* In Glide 2 the unit's own 1/w counts only after grHints said so. */
    float tq=glide3||stw_tmu0?vf(c,p,lay.q0,oow):oow;
    if(tq<=0)tq=oow;
    v->u=vf(c,p,lay.st0,0)/tq*clip_scale_s;
    v->v=(lay.st0<0?0.f:api_float(x86_rd32(c,p+(uint32_t)lay.st0+4u)))/tq*clip_scale_t;
}
static uint32_t cur_tex;
static void draw(const QRXHGVertex *v,unsigned n){
    st.texture=tex_on?cur_tex:0;
    qrx_hg_triangles(&st,v,n);
}
GF(grDrawTriangle){
    QRXHGVertex v[3];
    make(c,a[0],&v[0]);make(c,a[1],&v[1]);make(c,a[2],&v[2]);
    draw(v,3);
    return 0;
}
/* A fan over `n` pointers or contiguous vertices. */
static QRXHGVertex fan[3u*4096u];
GF(grDrawPlanarPolygonVertexList){
    uint32_t n=a[0];
    if(n<3||n>4096)return 0;
    /* list is an array of Glide 2 vertices with stride = struct size */
    const uint32_t stride=60;
    QRXHGVertex first,prev,cur;
    make(c,a[1],&first);make(c,a[1]+stride,&prev);
    unsigned o=0;
    for(uint32_t i=2;i<n;i++){
        make(c,a[1]+i*stride,&cur);
        fan[o++]=first;fan[o++]=prev;fan[o++]=cur;
        prev=cur;
    }
    draw(fan,o);
    return 0;
}
/* Glide 3 arrays. mode: 3 strip? use Glide values: POINTS 0, LINE_STRIP 1, LINES 2,
 * POLYGON 3, TRIANGLE_STRIP 4, TRIANGLE_FAN 5, TRIANGLES 6 */
static void draw_array(x86cpu *c,uint32_t mode,uint32_t n,uint32_t ptrs,uint32_t base,uint32_t stride){
    if(n<3)return;
    if(n>4096)n=4096;
    static QRXHGVertex tmp[4096];
    for(uint32_t i=0;i<n;i++)make(c,ptrs?x86_rd32(c,ptrs+i*4u):base+i*stride,&tmp[i]);
    unsigned o=0;
    if(mode==6)for(uint32_t i=0;i+2<n;i+=3){fan[o++]=tmp[i];fan[o++]=tmp[i+1];fan[o++]=tmp[i+2];}
    else if(mode==4)for(uint32_t i=0;i+2<n;i++){
        fan[o++]=tmp[i&1?i+1:i];fan[o++]=tmp[i&1?i:i+1];fan[o++]=tmp[i+2];
    }
    else if(mode==5||mode==3)for(uint32_t i=1;i+1<n;i++){fan[o++]=tmp[0];fan[o++]=tmp[i];fan[o++]=tmp[i+1];}
    draw(fan,o);
}
GF(grDrawVertexArray){draw_array(c,a[0],a[1],a[2],0,0);return 0;}
GF(grDrawVertexArrayContiguous){draw_array(c,a[0],a[1],0,a[2],a[3]);return 0;}
GF(grVertexLayout){
    NOARGS;
    if(!glide3){
        glide3=true;
        lay.xy=lay.z=lay.q=lay.a=lay.rgb=lay.pargb=lay.st0=lay.q0=-1;
        lay.xy=0;
    }
    int off=a[2]?(int)a[1]:-1;
    switch(a[0]){
    case 0x01:lay.xy=off;break;case 0x02:lay.z=off;break;case 0x03:case 0x04:lay.q=off;break;
    case 0x10:lay.a=off;break;case 0x30:lay.rgb=off;break;case 0x40:lay.pargb=off;break;
    case 0x50:lay.st0=off;break;case 0x51:lay.q0=off;break;
    default:break;
    }
    return 0;
}
GF(grDepthBufferMode){NOARGS;st.depth_test=a[0]!=0;w_buffer=a[0]==2||a[0]==4;return 0;}
/* The shader fades linearly over w. A fog table says how much fog there is at 64 fixed values of
 * w: from it come where the fog begins and where it is full. */
GF(grFogMode){NOARGS;st.fog=(a[0]&0xFFu)==2u;return 0;}
GF(grFogColorValue){
    NOARGS;
    st.fog_rgb[0]=((a[0]>>16)&255u)/255.f;st.fog_rgb[1]=((a[0]>>8)&255u)/255.f;st.fog_rgb[2]=(a[0]&255u)/255.f;
    return 0;
}
static float fog_index_to_w(unsigned i){return ldexpf(1.f,3+(int)(i>>2))/(float)(8u-(i&3u));}
GF(grFogTable){
    float start=-1.f,end=-1.f;
    for(unsigned i=0;i<64u;i++){
        unsigned v=x86_rd8(c,a[0]+i);
        if(v<8u)start=fog_index_to_w(i);
        if(v>=248u&&end<0.f)end=fog_index_to_w(i);
    }
    if(end<0.f)end=fog_index_to_w(63);
    if(start<0.f)start=fog_index_to_w(0);
    if(start>=end)start=end*0.5f;
    st.fog_start=start;st.fog_end=end;
    return 0;
}
GF(grHints){NOARGS;if(a[0]==0)stw_tmu0=(a[1]&2u)!=0;return 0;}     /* GR_HINT_STWHINT, W_DIFF_TMU0 */
GF(grDepthBufferFunction){NOARGS;st.depth_func=(uint8_t)(a[0]&7u);return 0;}
GF(grDepthMask){NOARGS;st.depth_write=a[0]!=0;return 0;}
GF(grColorMask){NOARGS;st.color_write=a[0]!=0;return 0;}
GF(grCullMode){
    NOARGS;
    /* NEGATIVE culls negative signed area, as seen in the screen coordinates. */
    st.cull=a[0]==0?HG_CULL_NONE:a[0]==1?HG_CULL_BACK:HG_CULL_FRONT;
    return 0;
}
static uint8_t blend(uint32_t f){
    switch(f){
    case 0:return HG_BLEND_ZERO;case 1:return HG_BLEND_SRC_ALPHA;case 2:return HG_BLEND_SRC_COLOR;
    case 3:return HG_BLEND_DST_ALPHA;case 4:return HG_BLEND_ONE;case 5:return HG_BLEND_INV_SRC_ALPHA;
    case 6:return HG_BLEND_INV_SRC_COLOR;case 7:return HG_BLEND_INV_DST_ALPHA;default:return HG_BLEND_ONE;
    }
}
GF(grAlphaBlendFunction){
    NOARGS;
    st.src=blend(a[0]);st.dst=blend(a[1]);
    st.blend=!(st.src==HG_BLEND_ONE&&st.dst==HG_BLEND_ZERO);
    return 0;
}
GF(grAlphaTestFunction){NOARGS;st.alpha_func=(uint8_t)(a[0]&7u);st.alpha_test=a[0]!=7;return 0;}
GF(grAlphaTestReferenceValue){NOARGS;st.alpha_ref=(a[0]&255u)/255.f;return 0;}
GF(grConstantColorValue){
    NOARGS;
    constant[0]=((a[0]>>16)&255u)/255.f;constant[1]=((a[0]>>8)&255u)/255.f;constant[2]=(a[0]&255u)/255.f;constant[3]=(a[0]>>24)/255.f;
    return 0;
}
/* func, factor, local, other, invert. other 1 = texture; local 1 = constant colour. */
GF(grColorCombine){
    NOARGS;
    tex_on=a[3]==1&&a[0]!=0&&a[0]!=1;
    const_color=a[2]==1;
    st.env=(a[1]==8)?HG_ENV_REPLACE:HG_ENV_MODULATE;
    if(a[0]==1){tex_on=false;}
    return 0;
}
GF(grAlphaCombine){NOARGS;const_alpha=a[2]==1;return 0;}
/* Texture memory is a key space: start address -> GPU texture. */
#define SLOTS 8192u
typedef struct { uint32_t start; uint32_t hg; unsigned w,h; bool used; } Slot;
static Slot slots[SLOTS];
static uint32_t palette[256];
static void tex_dims(int large,int aspect,unsigned *w,unsigned *h){
    int ls,ar;
    if(glide3){ls=large;ar=aspect;}       /* LOD_LOG2_x = x, ASPECT_LOG2 signed */
    else{ls=8-large;ar=3-aspect;}          /* LOD_256=0, ASPECT_1x1=3 */
    unsigned base=1u<<(ls<0?0:ls>11?11:ls);
    if(ar>=0){*w=base;*h=base>>ar;}else{*w=base>>-ar;*h=base;}
    if(!*w)*w=1;
    if(!*h)*h=1;
}
static unsigned texel_bytes(uint32_t fmt){return fmt<8?1u:2u;}
static void decode(x86cpu *c,uint32_t src,unsigned w,unsigned h,uint32_t fmt,uint8_t *d){
    for(uint32_t i=0;i<w*h;i++,d+=4){
        uint32_t v=texel_bytes(fmt)==1?x86_rd8(c,src+i):x86_rd16(c,src+i*2u);
        uint32_t r=255,g=255,b=255,al=255;
        switch(fmt){
        case 0:r=(v>>5)*255/7;g=((v>>2)&7)*255/7;b=(v&3)*255/3;break;
        case 2:al=v;break;
        case 3:r=g=b=v;break;
        case 4:r=g=b=(v&15)*17;al=(v>>4)*17;break;
        case 5:{uint32_t p=palette[v&255];r=(p>>16)&255;g=(p>>8)&255;b=p&255;break;}
        case 8:al=v>>8;r=((v>>5)&7)*255/7;g=((v>>2)&7)*255/7;b=(v&3)*255/3;break;
        case 0xA:r=(v>>11)*255/31;g=((v>>5)&63)*255/63;b=(v&31)*255/31;break;
        case 0xB:al=(v>>15)?255:0;r=((v>>10)&31)*255/31;g=((v>>5)&31)*255/31;b=(v&31)*255/31;break;
        case 0xC:al=(v>>12)*17;r=((v>>8)&15)*17;g=((v>>4)&15)*17;b=(v&15)*17;break;
        case 0xD:r=g=b=v&255;al=v>>8;break;
        case 0xE:{uint32_t p=palette[v&255];r=(p>>16)&255;g=(p>>8)&255;b=p&255;al=v>>8;break;}
        default:break;
        }
        d[0]=(uint8_t)r;d[1]=(uint8_t)g;d[2]=(uint8_t)b;d[3]=(uint8_t)al;
    }
}
/* GrTexInfo: small, large, aspect, format, data; each a dword */
GF(grTexDownloadMipMap){
    uint32_t start=a[1],info=a[3];
    int large=(int)x86_rd32(c,info+4),aspect=(int)x86_rd32(c,info+8);
    uint32_t fmt=x86_rd32(c,info+12),data=x86_rd32(c,info+16);
    unsigned w,h;
    tex_dims(large,aspect,&w,&h);
    Slot *s=&slots[(start>>3)%SLOTS];
    uint8_t *px=malloc((size_t)w*h*4u);
    if(!px)return 0;
    decode(c,data,w,h,fmt,px);
    s->hg=qrx_hg_texture(s->used?s->hg:0,w,h,px,true,true,true);
    s->start=start;s->w=w;s->h=h;s->used=true;
    free(px);
    return 0;
}
GF(grTexSource){
    uint32_t start=a[1],info=a[3];
    int large=(int)x86_rd32(c,info+4),aspect=(int)x86_rd32(c,info+8);
    unsigned w,h;
    tex_dims(large,aspect,&w,&h);
    Slot *s=&slots[(start>>3)%SLOTS];
    cur_tex=s->used?s->hg:0;
    /* s and t run to 256 along the long side, whatever the texture's real size. */
    unsigned longest=w>h?w:h;
    clip_scale_s=(float)longest/(float)w/256.f;
    clip_scale_t=(float)longest/(float)h/256.f;
    return 0;
}
GF(grTexTextureMemRequired){
    int large=(int)x86_rd32(c,a[1]+4),aspect=(int)x86_rd32(c,a[1]+8);
    uint32_t fmt=x86_rd32(c,a[1]+12);
    unsigned w,h;
    tex_dims(large,aspect,&w,&h);
    uint32_t total=0;
    while(w>1||h>1){total+=w*h*texel_bytes(fmt);w=w>1?w/2:1;h=h>1?h/2:1;}
    total+=texel_bytes(fmt);
    return (total+7u)&~7u;
}
GF(grTexMaxAddress){NOARGS;return 64u*1024u*1024u;}
/* The Gu calls of Glide 2: the library keeps the texture description behind a number. */
#define GU_MAX 4096u
typedef struct { uint32_t fmt; unsigned w,h; uint32_t hg; } GuTex;
static GuTex gu_tex[GU_MAX];
static uint32_t gu_count;
GF(guTexMemReset){NOARGS;gu_count=0;return 0;}
GF(guTexAllocateMemory){
    /* tmu, even/odd, width, height, format, mipmap, small lod, large lod, aspect, ... */
    NOARGS;
    if(gu_count>=GU_MAX)return 0xFFFFFFFFu;
    GuTex *t=&gu_tex[gu_count];
    t->fmt=a[4];
    tex_dims((int)a[7],(int)a[8],&t->w,&t->h);
    return gu_count++;
}
GF(guTexDownloadMipMap){
    if(a[0]>=gu_count)return 0;
    GuTex *t=&gu_tex[a[0]];
    uint8_t *px=malloc((size_t)t->w*t->h*4u);
    if(!px)return 0;
    decode(c,a[1],t->w,t->h,t->fmt,px);
    t->hg=qrx_hg_texture(t->hg,t->w,t->h,px,true,true,true);
    free(px);
    return 0;
}
GF(guTexSource){
    NOARGS;
    if(a[0]>=gu_count)return 0;
    const GuTex *t=&gu_tex[a[0]];
    cur_tex=t->hg;
    unsigned longest=t->w>t->h?t->w:t->h;
    clip_scale_s=(float)longest/(float)t->w/256.f;
    clip_scale_t=(float)longest/(float)t->h/256.f;
    return 0;
}
GF(grTexDownloadTablePartial){
    /* tmu, type, data, first, last */
    for(uint32_t i=a[3];i<=a[4]&&i<256u;i++)palette[i]=x86_rd32(c,a[2]+(i-a[3])*4u);
    return 0;
}
GF(grTriStats){if(a[0])x86_wr32(c,a[0],0);if(a[1])x86_wr32(c,a[1],0);return 0;}
GF(grTexDownloadTable){
    /* Glide 2: (tmu,type,data), Glide 3: (type,data). Palette: 256 ARGB dwords. */
    uint32_t data=glide3?a[1]:a[2];
    for(uint32_t i=0;i<256;i++)palette[i]=x86_rd32(c,data+i*4u);
    return 0;
}
GF(grGet){
    uint32_t v=0;
    switch(a[0]){
    case 0x01:v=16;break;           /* BITS_DEPTH */
    case 0x0B:v=1;break;            /* NUM_BOARDS */
    case 0x0E:v=1;break;
    case 0x10:v=2;break;            /* NUM_TMU */
    case 0x15:v=2;break;            /* MAX_TEXTURE_SIZE */
    default:break;
    }
    if(a[0]==0x13||a[0]==0x14)v=256;
    if(a[2])x86_wr32(c,a[2],v);
    return 4;
}
#define API(n,na,f) {n,na,f}
const QRXHostApi qrx_api_glide[]={
    API("grErrorSetCallback",1,gr_noop),API("grHints",2,grHints),API("grSstIdle",0,gr_noop),API("grBufferNumPending",0,gr_noop),API("grTriStats",2,grTriStats),API("grResetTriStats",0,gr_noop),
    API("grSstResetPerfStats",0,gr_noop),API("grSstPerfStats",1,gr_noop),API("grFogTable",1,grFogTable),API("grTexCombineFunction",2,gr_noop),API("grDrawLine",2,gr_noop),
    API("grLfbLock",6,gr_noop),API("grLfbUnlock",2,gr_noop),                     /* FXFALSE: no access to the frame buffer */
    API("guTexMemReset",0,guTexMemReset),API("guTexAllocateMemory",15,guTexAllocateMemory),API("guTexDownloadMipMap",3,guTexDownloadMipMap),API("guTexSource",1,guTexSource),
    API("guDrawTriangleWithClip",3,grDrawTriangle),API("grTexDownloadTablePartial",5,grTexDownloadTablePartial),
    API("grGlideInit",0,gr_noop),API("grGlideShutdown",0,gr_noop),API("grSstSelect",1,gr_noop),API("grSstWinClose",0,gr_noop),API("grSstWinOpen",7,grSstWinOpen),
    API("grSstQueryHardware",1,grSstQueryHardware),API("grSstQueryBoards",1,grSstQueryBoards),API("grSstScreenWidth",0,grSstScreenWidth),API("grSstScreenHeight",0,grSstScreenHeight),
    API("grSstOrigin",1,grSstOrigin),API("grBufferClear",3,grBufferClear),API("grBufferSwap",1,grBufferSwap),API("grRenderBuffer",1,gr_noop),API("grFinish",0,gr_noop),API("grFlush",0,gr_noop),
    API("grDrawTriangle",3,grDrawTriangle),API("grDrawPlanarPolygonVertexList",2,grDrawPlanarPolygonVertexList),API("grDrawPolygonVertexList",2,grDrawPlanarPolygonVertexList),
    API("grDrawVertexArray",3,grDrawVertexArray),API("grDrawVertexArrayContiguous",4,grDrawVertexArrayContiguous),API("grVertexLayout",3,grVertexLayout),
    API("grCoordinateSpace",1,gr_noop),API("grDepthBufferMode",1,grDepthBufferMode),API("grDepthBufferFunction",1,grDepthBufferFunction),API("grDepthMask",1,grDepthMask),
    API("grColorMask",2,grColorMask),API("grCullMode",1,grCullMode),API("grAlphaBlendFunction",4,grAlphaBlendFunction),API("grAlphaTestFunction",1,grAlphaTestFunction),
    API("grAlphaTestReferenceValue",1,grAlphaTestReferenceValue),API("grConstantColorValue",1,grConstantColorValue),API("grColorCombine",5,grColorCombine),API("grAlphaCombine",5,grAlphaCombine),
    API("grTexCombine",7,gr_noop),API("grTexFilterMode",3,gr_noop),API("grTexClampMode",3,gr_noop),API("grTexMipMapMode",3,gr_noop),API("grTexLodBiasValue",2,gr_noop),
    API("grTexDownloadMipMap",4,grTexDownloadMipMap),API("grTexSource",4,grTexSource),API("grTexTextureMemRequired",2,grTexTextureMemRequired),API("grTexMinAddress",1,gr_noop),
    API("grTexMaxAddress",1,grTexMaxAddress),API("grTexDownloadTable",3,grTexDownloadTable),API("grGet",3,grGet),API("grClipWindow",4,gr_noop),API("grFogMode",1,grFogMode),API("grFogColorValue",1,grFogColorValue),
    API("grChromakeyMode",1,gr_noop),API("grChromakeyValue",1,gr_noop),API("grDitherMode",1,gr_noop),API("grLfbConstantAlpha",1,gr_noop),API("grLfbConstantDepth",1,gr_noop),
    API("grDepthBiasLevel",1,gr_noop),API("grEnable",1,gr_noop),API("grDisable",1,gr_noop),API("grGlideGetVersion",1,gr_ok),API("grSstControl",1,gr_ok),API("grSstVRetraceOn",0,gr_ok),API("grSstIsBusy",0,gr_noop),
    {NULL,0,NULL}
};
