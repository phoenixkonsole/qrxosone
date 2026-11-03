#include "qrx_accel.h"
#include <stdlib.h>
#include <string.h>
static QRXAccelFrame *frame_new(void){return calloc(1,sizeof(QRXAccelFrame));}
void qrx_accel_frame_release(QRXAccelFrame *f){
    if(!f)return;
    for(unsigned i=0;i<f->count;i++)free(f->commands[i].image);
    free(f);
}
bool qrx_accel_init(QRXAccelStream *s){
    if(!s)return false;
    memset(s,0,sizeof(*s));
    if(pthread_mutex_init(&s->lock,NULL)!=0)return false;
    s->building=frame_new();
    if(!s->building){pthread_mutex_destroy(&s->lock);return false;}
    s->initialized=true;return true;
}
void qrx_accel_destroy(QRXAccelStream *s){
    if(!s||!s->initialized)return;
    pthread_mutex_lock(&s->lock);
    qrx_accel_frame_release(s->building);qrx_accel_frame_release(s->ready);
    s->building=s->ready=NULL;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);s->initialized=false;
}
static QRXAccelCmd *next(QRXAccelStream *s,size_t bytes){
    if(!s||!s->initialized||!s->building || s->building->count>=QRX_ACCEL_MAX_COMMANDS ||
       bytes>QRX_ACCEL_MAX_PAYLOAD-s->building->payload_bytes)return NULL;
    QRXAccelFrame *f=s->building;
    f->payload_bytes+=bytes;
    return &f->commands[f->count++];
}
bool qrx_accel_clear(QRXAccelStream *s,uint32_t color,uint32_t depth){
    QRXAccelCmd *c=next(s,0);if(!c)return false;
    c->kind=QRX_AC_CLEAR;c->color=color;c->depth=depth;return true;
}
bool qrx_accel_rect(QRXAccelStream *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t color){
    if(!w||!h||x>=800u||y>=450u||w>800u-x||h>450u-y)return false;
    QRXAccelCmd *c=next(s,0);if(!c)return false;
    c->kind=QRX_AC_RECT;c->x=x;c->y=y;c->w=w;c->h=h;c->color=color;return true;
}
static bool copy_rgba(QRXAccelCmd *c,const uint8_t *src,uint32_t w,uint32_t h,size_t stride){
    if(!src||!w||!h||w>2048||h>2048||stride<(size_t)w*4)return false;
    size_t n=(size_t)w*h*4;
    c->image=malloc(n);
    if(!c->image)return false;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
        const uint8_t *s=src+(size_t)y*stride+(size_t)x*4;
        uint8_t *d=c->image+((size_t)y*w+x)*4;
        d[0]=s[1];d[1]=s[2];d[2]=s[3];d[3]=s[0];
    }
    return true;
}
bool qrx_accel_image(QRXAccelStream *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint8_t *src,size_t rowbytes){
    if(!w||!h||x>=800||y>=450||w>800-x||h>450-y||w>2048||h>2048 || rowbytes<(size_t)w*4)return false;
    size_t sz=(size_t)w*h*4;
    QRXAccelCmd *c=next(s,sz);if(!c)return false;
    c->kind=QRX_AC_IMAGE;c->x=x;c->y=y;c->w=w;c->h=h;c->pitch=w*4;
    if(!copy_rgba(c,src,w,h,rowbytes)){
        s->building->count--;s->building->payload_bytes-=sz;return false;
    }
    return true;
}
bool qrx_accel_triangle(QRXAccelStream *s,const QRXAccelVertex v[3],uint32_t flags,
    const uint8_t *texture,uint32_t w,uint32_t h,uint32_t stride){
    if(!v)return false;
    size_t sz=texture?(size_t)w*h*4:0;
    if(texture && (!w||!h||w>2048||h>2048||stride<(uint64_t)w*4))return false;
    QRXAccelCmd *c=next(s,sz);if(!c)return false;
    c->kind=QRX_AC_TRIANGLE;c->flags=flags;memcpy(c->vert,v,sizeof(c->vert));
    if(texture){c->w=w;c->h=h;c->pitch=w*4;
        if(!copy_rgba(c,texture,w,h,stride)){
            s->building->count--;s->building->payload_bytes-=sz;return false;
        }
    }
    return true;
}
bool qrx_accel_commit(QRXAccelStream *s){
    if(!s||!s->initialized||!s->building)return false;
    QRXAccelFrame *replacement=frame_new();
    if(!replacement)return false;
    pthread_mutex_lock(&s->lock);
    QRXAccelFrame *old=s->ready;
    s->ready=s->building;
    s->ready->sequence=++s->submitted;
    s->building=replacement;
    if(old)s->dropped++;
    pthread_mutex_unlock(&s->lock);
    qrx_accel_frame_release(old);
    return true;
}
QRXAccelFrame *qrx_accel_take_latest(QRXAccelStream *s){
    if(!s||!s->initialized)return NULL;
    pthread_mutex_lock(&s->lock);
    QRXAccelFrame *f=s->ready;s->ready=NULL;
    pthread_mutex_unlock(&s->lock);
    return f;
}
