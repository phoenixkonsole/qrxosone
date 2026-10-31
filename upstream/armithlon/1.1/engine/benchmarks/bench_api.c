/* SPDX-License-Identifier: MIT
 * ARMITHLON original, independently buildable API benchmark.
 * Encoder numbers are NOT GPU FPS. CPU numbers are NOT Metal/Vulkan FPS.
 */
#define _POSIX_C_SOURCE 200809L
#include "qrx_bench.h"
#include "qrx_gl_compat.h"
#include "qrxmedia.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <math.h>
#define RAMSZ (8u*1024u*1024u)
#define VBASE 0x18000u
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return 1000.0*t.tv_sec+t.tv_nsec/1e6;}
static void be32(uint8_t *d,uint32_t v){d[0]=v>>24;d[1]=v>>16;d[2]=v>>8;d[3]=v;}
static int emit_gl(QRXAccelStream *s){
    QRXGLContext g;qrx_gl_init(&g,s);
    qrx_gl_ortho(&g,0,800,450,0,0,1);
    qrx_gl_clear_color(&g,17.f/255,24.f/255,39.f/255,1);
    qrx_gl_clear(&g,QRX_GL_COLOR_BUFFER_BIT|QRX_GL_DEPTH_BUFFER_BIT);
    qrx_gl_enable(&g,QRX_GL_DEPTH_TEST,true);
    qrx_gl_begin(&g,QRX_GL_TRIANGLES);
    for(unsigned i=0;i<QRX_BENCH_TRIANGLES;i++){
        QRXAccelVertex v[3];qrx_bench_triangle(i,v);
        for(unsigned j=0;j<3;j++){
            qrx_gl_color(&g,v[j].r,v[j].g,v[j].b,1);
            qrx_gl_vertex(&g,v[j].x,v[j].y,v[j].z);
        }
    }
    qrx_gl_end(&g);
    qrx_gl_enable(&g,QRX_GL_DEPTH_TEST,false);
    qrx_gl_enable(&g,QRX_GL_BLEND,true);
    qrx_gl_begin(&g,QRX_GL_TRIANGLES);
    for(unsigned i=0;i<16;i++){
        QRXAccelVertex v[3];qrx_bench_triangle(i*14u,v);
        for(unsigned j=0;j<3;j++){
            qrx_gl_color(&g,1,.68f,.12f,.42f);
            qrx_gl_vertex(&g,v[j].x+5.f,v[j].y+5.f,v[j].z);
        }
    }
    qrx_gl_end(&g);
    return qrx_gl_error(&g)==0&&qrx_gl_flush(&g)?0:-1;
}
static int init_glide(QRXVM *v,QRXMediaDevice *m,QRXAccelStream *s,uint8_t **mem){
    *mem=calloc(1,RAMSZ);if(!*mem)return -1;
    qrx_vm_init(v,*mem,RAMSZ);
    qrx_media_init(m,NULL,NULL);
    qrx_vm_bind_media(v,m);m->accel=s;
    return 0;
}
static int glide_cmd(QRXVM *v,uint32_t command,uint32_t arg0,uint32_t arg1){
    uint32_t result=0;
    return qrx_vm_write(v,QRX_3D_BASE+0x20u,4,arg0)&&
           qrx_vm_write(v,QRX_3D_BASE+0x24u,4,arg1)&&
           qrx_vm_write(v,QRX_3D_BASE+0x10u,4,command)&&
           qrx_vm_read(v,QRX_3D_BASE+0x0cu,4,&result)&&result==QRX_3D_OK?0:-1;
}
static int emit_glide(QRXVM *v){
    if(glide_cmd(v,QRX_3D_CMD_CLEAR,0x111827u,65535u))return -1;
    for(unsigned i=0;i<QRX_BENCH_TRIANGLES+16u;i++){
        QRXAccelVertex verts[3];qrx_bench_triangle(i<QRX_BENCH_TRIANGLES?i:(i-QRX_BENCH_TRIANGLES)*14u,verts);
        const int overlay=i>=QRX_BENCH_TRIANGLES;
        for(unsigned k=0;k<3;k++){
            uint8_t *p=v->ram.data+VBASE+k*24u;
            if(overlay){verts[k].x+=5.f;verts[k].y+=5.f;}
            be32(p+0,(uint32_t)(int32_t)lrintf(verts[k].x*65536.f));
            be32(p+4,(uint32_t)(int32_t)lrintf(verts[k].y*65536.f));
            be32(p+8,(uint32_t)(int32_t)lrintf(verts[k].z*65536.f));
            unsigned a=overlay?107u:255u;
            unsigned r=overlay?255u:(unsigned)lrintf(verts[k].r*255.f);
            unsigned g=overlay?173u:(unsigned)lrintf(verts[k].g*255.f);
            unsigned b=overlay?31u:(unsigned)lrintf(verts[k].b*255.f);
            be32(p+12,(a<<24)|(r<<16)|(g<<8)|b);
            be32(p+16,0);be32(p+20,0);
        }
        if(glide_cmd(v,QRX_3D_CMD_TRIANGLE,VBASE,overlay?4u:3u))return -1;
    }
    return glide_cmd(v,QRX_3D_CMD_SWAP,0,0);
}
static int validate_api_frame(const QRXAccelFrame *f,const QRXAccelFrame *reference){
    if(!f||!reference||f->count!=reference->count)return -1;
    for(uint32_t i=0;i<f->count;i++){
        const QRXAccelCmd *a=&f->commands[i],*b=&reference->commands[i];
        if(a->kind!=b->kind){fprintf(stderr,"kind mismatch cmd=%u %u!=%u\n",i,a->kind,b->kind);return -1;}
        if(i==0){if(a->color!=b->color)return -1;continue;}
        for(unsigned k=0;k<3;k++){
            const float *v=&a->vert[k].x,*r=&b->vert[k].x;
            for(unsigned j=0;j<9;j++){
                if(j==3u||j==4u)continue; /* texcoords not part of untextured API workload */
                if(!isfinite(v[j]) || fabsf(v[j]-r[j])>0.012f){fprintf(stderr,"vertex mismatch cmd=%u vertex=%u element=%u got=%g expected=%g\n",i,k,j,v[j],r[j]);return -1;}
            }
        }
        /* OpenGL 1.x HLE does not currently implement depth-buffer write state,
         * so only the supported depth-test + alpha-blend bits are checked. */
        if((a->flags&5u)!=(b->flags&5u)){fprintf(stderr,"flag mismatch cmd=%u got=%u exp=%u\n",i,a->flags,b->flags);return -1;}
    }
    return 0;
}
static void usage(const char *p){fprintf(stderr,"usage: %s <cpu|opengl-hle|glide-mmio> [--frames N] [--warmup N] [--preview PATH]\n",p);}
int main(int argc,char **argv){
    if(argc<2){usage(argv[0]);return 2;}
    const char *api=argv[1],*preview=NULL;
    int frames=45,warmup=5;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--frames")&&i+1<argc)frames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--warmup")&&i+1<argc)warmup=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--preview")&&i+1<argc)preview=argv[++i];
        else {usage(argv[0]);return 2;}
    }
    if(frames<1||frames>2000||warmup<0||warmup>500){fprintf(stderr,"invalid frame count\n");return 2;}
    int cpu=!strcmp(api,"cpu"),gl=!strcmp(api,"opengl-hle"),glide=!strcmp(api,"glide-mmio");
    if(!cpu&&!gl&&!glide){usage(argv[0]);return 2;}
    QRXAccelStream stream={0};if(!qrx_accel_init(&stream))return 3;
    QRXVM vm={0};QRXMediaDevice media={0};uint8_t *guest=NULL;
    if(glide&&init_glide(&vm,&media,&stream,&guest)){qrx_accel_destroy(&stream);return 3;}
    QRXAccelFrame *baseline=qrx_bench_scene();
    const size_t bytes=(size_t)QRX_BENCH_W*QRX_BENCH_H*4u;
    uint8_t *pixels=malloc(bytes);
    if(!pixels||!baseline||baseline->count!=QRX_BENCH_COMMANDS||
       qrx_bench_reference(baseline,pixels,bytes)!=0){fprintf(stderr,"scene reference failed\n");return 3;}
    uint64_t hash=qrx_bench_hash(pixels,bytes);
    if(preview&&qrx_bench_save_ppm(preview,pixels)){fprintf(stderr,"could not write preview\n");return 3;}
    double begin=0,end=0;
    uint64_t count=0;
    for(int i=-warmup;i<frames;i++){
        if(i==0)begin=now_ms();
        if(cpu){if(qrx_bench_reference(baseline,pixels,bytes))return 4;count+=baseline->count;continue;}
        int rc=gl?emit_gl(&stream):emit_glide(&vm);
        if(rc){fprintf(stderr,"graphics translation error on frame %d\n",i);return 4;}
        QRXAccelFrame *f=qrx_accel_take_latest(&stream);
        if(validate_api_frame(f,baseline)){fprintf(stderr,"geometry/command mismatch frame %d got %u expected %u\n",i,f?f->count:0,QRX_BENCH_COMMANDS);return 4;}
        count+=f->count;
        qrx_accel_frame_release(f);
    }
    end=now_ms();
    const double ms=end-begin;
    printf("{\"api\":\"%s\",\"kind\":\"%s\",\"width\":800,\"height\":450,"
           "\"triangles_per_frame\":256,\"commands_per_frame\":%u,\"frames\":%d,"
           "\"warmup\":%d,\"elapsed_ms\":%.3f,\"fps\":%.3f,\"commands_per_s\":%.1f,"
           "\"reference_fnv64\":\"%016" PRIx64 "\",\"status\":\"PASS\"}\n",
           api,cpu?"cpu_reference":"command_translation_only",QRX_BENCH_COMMANDS,frames,
           warmup,ms,frames*1000./(ms>0?ms:1),frames*QRX_BENCH_COMMANDS*1000./(ms>0?ms:1),hash);
    if(glide){qrx_glide_shutdown(&media);qrx_vm_destroy(&vm);free(guest);}
    qrx_accel_frame_release(baseline);
    qrx_accel_destroy(&stream);
    free(pixels);
    return 0;
}
