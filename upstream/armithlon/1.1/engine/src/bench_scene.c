/* SPDX-License-Identifier: MIT — identical deterministic workload for GPU APIs. */
#include "qrx_bench.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void qrx_bench_triangle(unsigned i, QRXAccelVertex v[3]) {
    const unsigned col=i%20u, row=i/20u;
    const float x=(float)(col*40u+2u), y=(float)(row*37u+2u);
    const float r=(float)((col*41u+row*17u+64u)%256u)/255.f;
    const float g=(float)((row*59u+col*7u+80u)%256u)/255.f;
    const float b=(float)((i*11u+125u)%256u)/255.f;
    const float z=0.35f+(float)(i%5u)*0.06f;
    v[0]=(QRXAccelVertex){x+3,y+33,z,0,1,r,g,b,1};
    v[1]=(QRXAccelVertex){x+36,y+33,z,1,1,r,g,b,1};
    v[2]=(QRXAccelVertex){x+18,y+2,z,0.5f,0,r,g,b,1};
}
QRXAccelFrame *qrx_bench_scene(void){
    QRXAccelStream s;
    if(!qrx_accel_init(&s)) return NULL;
    if(!qrx_accel_clear(&s,0x111827u,65535u))goto error;
    for(unsigned i=0;i<QRX_BENCH_TRIANGLES;i++){
        QRXAccelVertex v[3];qrx_bench_triangle(i,v);
        if(!qrx_accel_triangle(&s,v,3u,NULL,0,0,0))goto error;
    }
    /* Transparent coverage tests exercise ordering + blending, but no depth state. */
    for(unsigned i=0;i<16;i++){
        QRXAccelVertex v[3];qrx_bench_triangle(i*14u,v);
        for(int j=0;j<3;j++){
            v[j].x+=5.f;v[j].y+=5.f;
            v[j].r=1.f;v[j].g=0.68f;v[j].b=0.12f;v[j].a=0.42f;
        }
        if(!qrx_accel_triangle(&s,v,4u,NULL,0,0,0))goto error;
    }
    if(!qrx_accel_commit(&s))goto error;
    QRXAccelFrame *f=qrx_accel_take_latest(&s);
    qrx_accel_destroy(&s);
    return f;
error:qrx_accel_destroy(&s);return NULL;
}
static float edge(float ax,float ay,float bx,float by,float px,float py){
    return (px-ax)*(by-ay)-(py-ay)*(bx-ax);
}
static uint8_t byte(float f){
    if(f<=0)return 0;
    if(f>=1)return 255;
    return (uint8_t)(f*255.f+0.5f);
}
int qrx_bench_reference(const QRXAccelFrame *frame,uint8_t *out,size_t n){
    if(!frame||!out||n<(size_t)QRX_BENCH_W*QRX_BENCH_H*4||frame->count>QRX_ACCEL_MAX_COMMANDS)return -1;
    const size_t sz=(size_t)QRX_BENCH_W*QRX_BENCH_H;
    float *zbuf=malloc(sz*sizeof(*zbuf));if(!zbuf)return -1;
    for(size_t i=0;i<sz;i++)zbuf[i]=1.f;
    memset(out,0,n);
    for(unsigned c=0;c<frame->count;c++){
        const QRXAccelCmd *cmd=&frame->commands[c];
        if(cmd->kind==QRX_AC_CLEAR){
            for(size_t i=0;i<sz;i++){
                out[4*i+0]=(uint8_t)(cmd->color>>16);
                out[4*i+1]=(uint8_t)(cmd->color>>8);
                out[4*i+2]=(uint8_t)cmd->color;out[4*i+3]=255;
                zbuf[i]=(float)cmd->depth/65535.f;
            }
            continue;
        }
        if(cmd->kind!=QRX_AC_TRIANGLE || cmd->image){free(zbuf);return -2;}
        const QRXAccelVertex *v=cmd->vert;
        float a=edge(v[0].x,v[0].y,v[1].x,v[1].y,v[2].x,v[2].y);
        if(!isfinite(a)||fabsf(a)<1e-6f){free(zbuf);return -1;}
        float xlo=fminf(v[0].x,fminf(v[1].x,v[2].x));
        float xhi=fmaxf(v[0].x,fmaxf(v[1].x,v[2].x));
        float ylo=fminf(v[0].y,fminf(v[1].y,v[2].y));
        float yhi=fmaxf(v[0].y,fmaxf(v[1].y,v[2].y));
        int x0=(int)fmaxf(0,floorf(xlo)),x1=(int)fminf(QRX_BENCH_W,ceilf(xhi));
        int y0=(int)fmaxf(0,floorf(ylo)),y1=(int)fminf(QRX_BENCH_H,ceilf(yhi));
        for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
            const float px=x+0.5f,py=y+0.5f;
            float w0=edge(v[1].x,v[1].y,v[2].x,v[2].y,px,py)/a;
            float w1=edge(v[2].x,v[2].y,v[0].x,v[0].y,px,py)/a;
            float w2=1.f-w0-w1;
            if(w0<-0.00001f||w1<-0.00001f||w2<-0.00001f)continue;
            float z=w0*v[0].z+w1*v[1].z+w2*v[2].z;
            size_t o=(size_t)y*QRX_BENCH_W+(size_t)x;
            if((cmd->flags&1u)&&z>=zbuf[o])continue;
            float rgba[4]={0};
            for(unsigned k=0;k<4;k++){
                for(unsigned j=0;j<3;j++){
                    float val=k==0?v[j].r:k==1?v[j].g:k==2?v[j].b:v[j].a;
                    rgba[k]+=(j==0?w0:j==1?w1:w2)*val;
                }
            }
            float alpha=(cmd->flags&4u)?fmaxf(0,fminf(1,rgba[3])):1.f;
            for(unsigned k=0;k<3;k++)out[4*o+k]=byte(alpha*rgba[k]+(1-alpha)*(float)out[4*o+k]/255.f);
            out[4*o+3]=255;
            if(cmd->flags&2u)zbuf[o]=z;
        }
    }
    free(zbuf);return 0;
}
int qrx_bench_save_ppm(const char *path,const uint8_t *rgba){
    FILE *f=fopen(path,"wb");if(!f)return -1;
    int failed=fprintf(f,"P6\n%u %u\n255\n",QRX_BENCH_W,QRX_BENCH_H)<0;
    for(unsigned y=0;y<QRX_BENCH_H;y++)for(unsigned x=0;x<QRX_BENCH_W;x++){
        if(fwrite(rgba+((size_t)y*QRX_BENCH_W+x)*4,1,3,f)!=3)failed=1;
    }
    if(fclose(f))failed=1;
    return failed?-1:0;
}
uint64_t qrx_bench_hash(const uint8_t *data,size_t n){
    uint64_t h=UINT64_C(14695981039346656037);
    for(size_t i=0;i<n;i++){h^=data[i];h*=UINT64_C(1099511628211);}
    return h;
}
