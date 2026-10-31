#define _POSIX_C_SOURCE 200809L
#include "qrx86_ipc.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <math.h>

#define QRX86_FRAME_MAGIC UINT32_C(0x51383646) /* Q86F */
#define QRX86_COMMAND_MAGIC UINT32_C(0x51383643) /* Q86C */
#define QRX86_COMMAND_BYTES 124u
#define QRX86_MAX_WIDTH 800u
#define QRX86_MAX_HEIGHT 450u
static void be32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static uint32_t rd32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static bool all_io(int fd,void *data,size_t n,bool write_mode){
    uint8_t *p=data;
    while(n){
#ifdef MSG_NOSIGNAL
        ssize_t k=write_mode?send(fd,p,n,MSG_NOSIGNAL):recv(fd,p,n,0);
#else
        ssize_t k=write_mode?send(fd,p,n,0):recv(fd,p,n,0);
#endif
        if(k<0 && errno==EINTR)continue;
        if(k<=0)return false;
        p+=(size_t)k;n-=(size_t)k;
    }
    return true;
}
bool qrx86_ipc_send(const char *path,const uint32_t *pixels,uint32_t w,uint32_t h){
    if(!path||!pixels||!w||!h||w>QRX86_MAX_WIDTH||h>QRX86_MAX_HEIGHT || strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return false;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return false;
#ifdef SO_NOSIGPIPE
    int no_sigpipe=1;
    setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
    struct sockaddr_un addr={0};addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,path,sizeof(addr.sun_path)-1);
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))!=0){close(fd);return false;}
    const size_t bytes=(size_t)w*h*4;
    uint8_t *buf=malloc(bytes+16);
    if(!buf){close(fd);return false;}
    be32(buf,QRX86_FRAME_MAGIC);be32(buf+4,w);be32(buf+8,h);be32(buf+12,(uint32_t)bytes);
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
        uint32_t c=pixels[(size_t)y*w+x];
        uint8_t *p=buf+16+((size_t)y*w+x)*4;
        p[0]=255;p[1]=(uint8_t)(c>>16);p[2]=(uint8_t)(c>>8);p[3]=(uint8_t)c;
    }
    bool ok=all_io(fd,buf,bytes+16,true);free(buf);close(fd);return ok;
}
/* Fixed-size big-endian header followed by per-command records:
 * kind,flags,color,depth (4*4), then 3*(x,y,z,u,v,r,g,b,a) IEEE754 floats.
 * No opaque guest address or OS handle is ever accepted from this protocol. */
bool qrx86_ipc_send_commands(const char *path,const QRXAccelFrame *frame){
    if(!path||!frame||frame->count==0||frame->count>QRX_ACCEL_MAX_COMMANDS||
       strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return false;
    const size_t size=12+(size_t)frame->count*QRX86_COMMAND_BYTES;
    uint8_t *pkt=calloc(1,size);if(!pkt)return false;
    be32(pkt,QRX86_COMMAND_MAGIC);be32(pkt+4,frame->count);be32(pkt+8,(uint32_t)(size-12));
    bool good=true;
    for(uint32_t i=0;i<frame->count;i++){
        const QRXAccelCmd *c=&frame->commands[i];
        if(c->kind!=QRX_AC_CLEAR && c->kind!=QRX_AC_TRIANGLE){good=false;break;}
        if(c->kind==QRX_AC_TRIANGLE && (c->image||c->flags&~7u)){good=false;break;}
        uint8_t *p=pkt+12+i*QRX86_COMMAND_BYTES;
        be32(p,c->kind);be32(p+4,c->flags);be32(p+8,c->color);be32(p+12,c->depth);
        for(uint32_t v=0;v<3;v++){
            const float *f=&c->vert[v].x;
            for(uint32_t n=0;n<9;n++){
                if(c->kind==QRX_AC_TRIANGLE && (!isfinite(f[n])||fabsf(f[n])>1000000.f)){good=false;break;}
                uint32_t bits;memcpy(&bits,f+n,4);be32(p+16+v*36+n*4,bits);
            }
        }
        if(!good)break;
    }
    if(!good){free(pkt);return false;}
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0){free(pkt);return false;}
#ifdef SO_NOSIGPIPE
    int no_sigpipe=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
    struct sockaddr_un addr={0};addr.sun_family=AF_UNIX;strncpy(addr.sun_path,path,sizeof(addr.sun_path)-1);
    bool ok=(connect(fd,(struct sockaddr *)&addr,sizeof(addr))==0)&&all_io(fd,pkt,size,true);
    close(fd);free(pkt);return ok;
}
static bool parse_commands(int fd,const uint8_t header[12],QRXAccelStream *out){
    uint32_t count=rd32(header+4), size=rd32(header+8);
    if(!count||count>QRX_ACCEL_MAX_COMMANDS||size!=(uint64_t)count*QRX86_COMMAND_BYTES)return false;
    uint8_t *data=malloc(size);if(!data)return false;
    bool good=all_io(fd,data,size,false);
    /* Check every field before mutating the frame builder. */
    for(uint32_t i=0;good&&i<count;i++){
        const uint8_t *p=data+(size_t)i*QRX86_COMMAND_BYTES;
        uint32_t kind=rd32(p),flags=rd32(p+4);
        if((kind!=QRX_AC_CLEAR && kind!=QRX_AC_TRIANGLE)||(flags&~7u)){good=false;break;}
        if(kind==QRX_AC_TRIANGLE)for(uint32_t k=0;k<27;k++){
            uint32_t b=rd32(p+16+k*4);float v;memcpy(&v,&b,4);
            if(!isfinite(v)||fabsf(v)>1000000.f){good=false;break;}
        }
    }
    if(good)for(uint32_t i=0;i<count;i++){
        const uint8_t *p=data+(size_t)i*QRX86_COMMAND_BYTES;
        if(rd32(p)==QRX_AC_CLEAR){good=qrx_accel_clear(out,rd32(p+8),rd32(p+12));}
        else{
            QRXAccelVertex v[3];
            for(uint32_t j=0;j<3;j++)for(uint32_t k=0;k<9;k++){
                uint32_t bits=rd32(p+16+j*36+k*4);
                memcpy(((float *)&v[j])+k,&bits,4);
            }
            good=qrx_accel_triangle(out,v,rd32(p+4),NULL,0,0,0);
        }
        if(!good)break;
    }
    if(good)good=qrx_accel_commit(out);
    free(data);return good;
}
bool qrx86_ipc_read_one(int fd,QRXAccelStream *out){
    uint8_t header[16]={0};if(!out||!all_io(fd,header,12,false))return false;
    if(rd32(header)==QRX86_COMMAND_MAGIC)return parse_commands(fd,header,out);
    if(!all_io(fd,header+12,4,false))return false;
    const uint32_t w=rd32(header+4),h=rd32(header+8),sz=rd32(header+12);
    if(rd32(header)!=QRX86_FRAME_MAGIC || !w||!h||w>QRX86_MAX_WIDTH||h>QRX86_MAX_HEIGHT||sz!=(uint64_t)w*h*4)return false;
    uint8_t *pixels=malloc(sz);
    if(!pixels)return false;
    bool ok=all_io(fd,pixels,sz,false);
    if(ok)ok=qrx_accel_clear(out,0,65535)&&qrx_accel_image(out,0,0,w,h,pixels,(size_t)w*4)&&qrx_accel_commit(out);
    free(pixels);
    return ok;
}
int qrx86_ipc_serve(const char *path,QRXAccelStream *out,const atomic_bool *running){
    if(!path||!out||!running||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
    struct sockaddr_un a={0};a.sun_family=AF_UNIX;
    strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    /* Refuse replacing someone else's socket: caller chooses an unused path. */
    if(bind(fd,(struct sockaddr *)&a,sizeof(a))!=0){close(fd);return -1;}
    chmod(path,0600);
    if(listen(fd,4)!=0){unlink(path);close(fd);return -1;}
    int result=0;
    while(atomic_load_explicit(running,memory_order_acquire)){
        fd_set rfds;FD_ZERO(&rfds);FD_SET(fd,&rfds);
        struct timeval tv={0,200000};
        int n=select(fd+1,&rfds,NULL,NULL,&tv);
        if(n<0){if(errno==EINTR)continue;result=-1;break;}
        if(n==0)continue;
        int client=accept(fd,NULL,NULL);
        if(client<0){if(errno==EINTR)continue;result=-1;break;}
        struct timeval timeout={1,0};
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        (void)qrx86_ipc_read_one(client,out);close(client);
    }
    close(fd);unlink(path);return result;
}
