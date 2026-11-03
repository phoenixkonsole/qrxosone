#include "qrxmedia.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* QRX3D v0.6, bounded command HLE. This is neither Voodoo PCI emulation nor
 * a binary-compatible glide2x.dll. The source is deliberately API-neutral.
 * Guest data is big endian and addressed by offsets within guest RAM only.
 * Rasterization is a correctness-only CPU fallback; Metal/Vulkan shaders TBD. */
static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int32_t sbe32(const uint8_t *p){return (int32_t)be32(p);}
static int valid_region(const QRXVM *v,uint32_t addr,uint64_t bytes){
    uint64_t end=(uint64_t)addr+bytes;
    return v->ram.data && !(addr&3u) && bytes>0 && end<=v->ram.size && end<=QRX_IO_BASE &&
        (end<=QRX_GFX_FRAMEBUFFER || addr >= (uint64_t)QRX_GFX_FRAMEBUFFER+(uint64_t)QRX_GFX_PITCH*QRX_GFX_HEIGHT);
}
static uint8_t clamp8(double n){if(n<0.0)return 0;if(n>255.0)return 255;return (uint8_t)(n+0.5);}
static void pixel(uint8_t *p,uint32_t c){p[0]=0;p[1]=(uint8_t)(c>>16);p[2]=(uint8_t)(c>>8);p[3]=(uint8_t)c;}
static uint32_t blend(uint32_t source,const uint8_t *dst,uint32_t a){
    uint32_t k=255-a;
    uint32_t r=(((source>>16)&255u)*a+(uint32_t)dst[1]*k+127u)/255u;
    uint32_t g=(((source>>8)&255u)*a+(uint32_t)dst[2]*k+127u)/255u;
    uint32_t b=((source&255u)*a+(uint32_t)dst[3]*k+127u)/255u;
    return (r<<16)|(g<<8)|b;
}
static uint32_t depth_alloc(QRXMediaDevice *m){
    if(!m->glide_depth) {
        size_t sz=(size_t)QRX_GFX_WIDTH*QRX_GFX_HEIGHT*sizeof(uint16_t);
        m->glide_depth=malloc(sz);
        if(m->glide_depth)memset(m->glide_depth,0xff,sz);
    }
    return m->glide_depth?QRX_3D_OK:QRX_3D_ERR_MEMORY;
}
static uint32_t clear(QRXVM *v){
    QRXMediaDevice *m=v->media;uint32_t a=m->glide_args[0],z=m->glide_args[1];
    if(z>65535u)return QRX_3D_ERR_ARGUMENT;
    if(m->accel) return qrx_accel_clear(m->accel,a,z)?QRX_3D_OK:QRX_3D_ERR_MEMORY;
    if(depth_alloc(m)!=QRX_3D_OK)return QRX_3D_ERR_MEMORY;
    uint8_t *p=v->ram.data+QRX_GFX_FRAMEBUFFER;
    for(size_t i=0;i<(size_t)QRX_GFX_WIDTH*QRX_GFX_HEIGHT;i++){
        pixel(p+i*4u,a);m->glide_depth[i]=(uint16_t)z;
    }
    return QRX_3D_OK;
}
typedef struct {double x,y,z,s,t;uint32_t rgba;} Vertex;
static void load_v(Vertex *v,const uint8_t *p){
    v->x=(double)sbe32(p)/65536.0;v->y=(double)sbe32(p+4)/65536.0;
    v->z=(double)sbe32(p+8)/65536.0;v->rgba=be32(p+12);
    v->s=(double)sbe32(p+16)/65536.0;v->t=(double)sbe32(p+20)/65536.0;
}
static uint32_t bind_texture(QRXVM *v){
    QRXMediaDevice *m=v->media;const uint32_t *a=m->glide_args;
    if(a[2]==0 && a[3]==0){m->glide_tex_addr=0;m->glide_tex_width=m->glide_tex_height=m->glide_tex_stride=0;return QRX_3D_OK;}
    if(!a[2]||!a[3]||a[2]>2048u||a[3]>2048u||a[1]<(uint64_t)a[2]*4u||a[1]>65536u)return QRX_3D_ERR_ARGUMENT;
    uint64_t bytes=(uint64_t)(a[3]-1u)*a[1]+(uint64_t)a[2]*4u;
    if(!valid_region(v,a[0],bytes))return QRX_3D_ERR_BOUNDS;
    m->glide_tex_addr=a[0];m->glide_tex_width=a[2];m->glide_tex_height=a[3];m->glide_tex_stride=a[1];return QRX_3D_OK;
}
static uint32_t draw(QRXVM *v){
    QRXMediaDevice *m=v->media;uint32_t addr=m->glide_args[0],flags=m->glide_args[1];
    if(flags & ~QRX_3D_SUPPORTED_FLAGS)return QRX_3D_ERR_ARGUMENT;
    if(!valid_region(v,addr,3u*24u))return QRX_3D_ERR_BOUNDS;
    if((flags & QRX_3D_TEXTURE) && !m->glide_tex_width)return QRX_3D_ERR_STATE;
    if(!m->accel && (flags & (QRX_3D_DEPTH_TEST|QRX_3D_DEPTH_WRITE)) && depth_alloc(m)!=QRX_3D_OK)return QRX_3D_ERR_MEMORY;
    Vertex a,b,c;load_v(&a,v->ram.data+addr);load_v(&b,v->ram.data+addr+24);load_v(&c,v->ram.data+addr+48);
    const Vertex* p[3]={&a,&b,&c};
    for(unsigned i=0;i<3;i++){
        if(p[i]->x < -8192 || p[i]->x > 8192 || p[i]->y < -8192 || p[i]->y > 8192 ||
           p[i]->z < 0.0 || p[i]->z > 1.0 || p[i]->s < -256 || p[i]->s > 256 || p[i]->t < -256 || p[i]->t > 256)
            return QRX_3D_ERR_ARGUMENT;
    }
    double area=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
    if(fabs(area)<0.000001)return QRX_3D_ERR_ARGUMENT;
    if((flags & QRX_3D_CULL_BACK) && area<=0.0)return QRX_3D_OK;
    double minx=fmin(a.x,fmin(b.x,c.x)),maxx=fmax(a.x,fmax(b.x,c.x));
    double miny=fmin(a.y,fmin(b.y,c.y)),maxy=fmax(a.y,fmax(b.y,c.y));
    int x0=(int)floor(fmax(0,minx)),y0=(int)floor(fmax(0,miny));
    int x1=(int)ceil(fmin(QRX_GFX_WIDTH,maxx)),y1=(int)ceil(fmin(QRX_GFX_HEIGHT,maxy));
    if(x0>=x1||y0>=y1)return QRX_3D_OK;
    if(m->accel) {
        QRXAccelVertex vertices[3] = {{0}};
        for(unsigned i=0;i<3;i++){
            const Vertex *src=p[i];QRXAccelVertex *d=&vertices[i];
            d->x=(float)src->x;d->y=(float)src->y;d->z=(float)src->z;
            d->u=(float)src->s;d->v=(float)src->t;
            d->r=(float)((src->rgba>>16)&255u)/255.f;
            d->g=(float)((src->rgba>>8)&255u)/255.f;
            d->b=(float)(src->rgba&255u)/255.f;
            d->a=(float)(src->rgba>>24)/255.f;
        }
        const uint8_t *tex=(flags & QRX_3D_TEXTURE)?v->ram.data+m->glide_tex_addr:NULL;
        return qrx_accel_triangle(m->accel,vertices,flags,tex,m->glide_tex_width,
             m->glide_tex_height,m->glide_tex_stride)?QRX_3D_OK:QRX_3D_ERR_MEMORY;
    }
    uint8_t *fb=v->ram.data+QRX_GFX_FRAMEBUFFER;
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
        double px=x+0.5,py=y+0.5;
        double u=((b.x-px)*(c.y-py)-(b.y-py)*(c.x-px))/area;
        double w=((a.x-px)*(b.y-py)-(a.y-py)*(b.x-px))/area;
        double t=1.0-u-w;
        if(u < -1.e-8 || w < -1.e-8 || t < -1.e-8)continue;
        uint32_t z=(uint32_t)fmin(65535.0,fmax(0.0,(u*a.z+t*b.z+w*c.z)*65535.0+0.5));
        size_t n=(size_t)y*QRX_GFX_WIDTH+(size_t)x;
        if((flags & QRX_3D_DEPTH_TEST) && z>=m->glide_depth[n])continue;
        uint32_t color=0,alpha=255;
        if(flags & QRX_3D_TEXTURE){
            double s=u*a.s+t*b.s+w*c.s,tex_t=u*a.t+t*b.t+w*c.t;
            int sx=(int)floor(fmin(1.0,fmax(0.0,s))*(m->glide_tex_width-1)+0.5);
            int sy=(int)floor(fmin(1.0,fmax(0.0,tex_t))*(m->glide_tex_height-1)+0.5);
            const uint8_t *tex=v->ram.data+(size_t)m->glide_tex_addr+(size_t)sy*m->glide_tex_stride+(size_t)sx*4u;
            alpha=tex[0];color=((uint32_t)tex[1]<<16)|((uint32_t)tex[2]<<8)|tex[3];
        }else{
            double cr=u*((a.rgba>>16)&255u)+t*((b.rgba>>16)&255u)+w*((c.rgba>>16)&255u);
            double cg=u*((a.rgba>>8)&255u)+t*((b.rgba>>8)&255u)+w*((c.rgba>>8)&255u);
            double cb=u*(a.rgba&255u)+t*(b.rgba&255u)+w*(c.rgba&255u);
            color=((uint32_t)clamp8(cr)<<16)|((uint32_t)clamp8(cg)<<8)|clamp8(cb);
            alpha=clamp8(u*(a.rgba>>24)+t*(b.rgba>>24)+w*(c.rgba>>24));
        }
        uint8_t *dst=fb+n*4u;
        if(flags & QRX_3D_ALPHA_BLEND)color=blend(color,dst,alpha);
        pixel(dst,color);
        if(flags & QRX_3D_DEPTH_WRITE)m->glide_depth[n]=(uint16_t)z;
    }
    return QRX_3D_OK;
}
static uint32_t exec(QRXVM *v,uint32_t cmd){
    if(!v->ram.data || (uint64_t)QRX_GFX_FRAMEBUFFER+(uint64_t)QRX_GFX_PITCH*QRX_GFX_HEIGHT>v->ram.size)return QRX_3D_ERR_MEMORY;
    switch(cmd){
        case QRX_3D_CMD_CLEAR:return clear(v);
        case QRX_3D_CMD_TRIANGLE:return draw(v);
        case QRX_3D_CMD_SWAP:return qrx_media_write(v,QRX_MEDIA_GFX_BASE+0x14,4,1)?QRX_3D_OK:QRX_3D_ERR_MEMORY;
        case QRX_3D_CMD_TEXTURE:return bind_texture(v);
        default:return QRX_3D_ERR_COMMAND;
    }
}
bool qrx_glide_read(QRXVM *v,uint32_t addr,unsigned width,uint32_t *value){
    if(!v||!v->media||!value||width!=4||(addr&3u)||addr<QRX_3D_BASE)return false;
    QRXMediaDevice *m=v->media;
    switch(addr-QRX_3D_BASE){
        case 0x00:*value=QRX_3D_MAGIC;return true;
        case 0x04:*value=QRX_3D_ABI_VERSION;return true;
        case 0x08:*value=QRX_3D_CAPS;return true;
        case 0x0c:*value=m->glide_error;return true;
        case 0x10:*value=m->glide_last_cmd;return true;
        case 0x14:*value=m->glide_success_count;return true;
        default:
            if(addr>=QRX_3D_BASE+0x20 && addr<QRX_3D_BASE+0x40){*value=m->glide_args[(addr-(QRX_3D_BASE+0x20))/4];return true;}
            return false;
    }
}
bool qrx_glide_write(QRXVM *v,uint32_t addr,unsigned width,uint32_t value){
    if(!v||!v->media||width!=4||(addr&3u)||addr<QRX_3D_BASE)return false;
    QRXMediaDevice *m=v->media;
    if(addr>=QRX_3D_BASE+0x20 && addr<QRX_3D_BASE+0x40){m->glide_args[(addr-(QRX_3D_BASE+0x20))/4]=value;return true;}
    if(addr==QRX_3D_BASE+0x10){m->glide_last_cmd=value;m->glide_error=exec(v,value);if(!m->glide_error)m->glide_success_count++;return true;}
    return false;
}
void qrx_glide_shutdown(QRXMediaDevice *m){if(m){free(m->glide_depth);m->glide_depth=NULL;}}
