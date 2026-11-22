#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx86_input.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define PACKET_SIZE 32u
static void wr(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint32_t rd(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static bool valid(const QRX86InputEvent *e){
    if(!e||e->type<1||e->type>5)return false;
    if(e->type<=2)return e->code<=255u && e->character<=0xffffu;
    if(e->x>=800u || e->y>=450u)return false;
    return e->type==3 || (e->button>=1u && e->button<=3u);
}
static int address(struct sockaddr_un *a,const char *path){
    if(!a||!path||!path[0]||strlen(path)>=sizeof(a->sun_path))return 0;
    memset(a,0,sizeof(*a));a->sun_family=AF_UNIX;
    memcpy(a->sun_path,path,strlen(path)+1);
    return 1;
}
static int transfer(int fd,void *buffer,size_t size,int write_mode){
    uint8_t *p=(uint8_t *)buffer;
    while(size){
        ssize_t n;
#ifdef MSG_NOSIGNAL
        n=write_mode?send(fd,p,size,MSG_NOSIGNAL):recv(fd,p,size,0);
#else
        n=write_mode?send(fd,p,size,0):recv(fd,p,size,0);
#endif
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return 0;
        p+=(size_t)n;size-=(size_t)n;
    }
    return 1;
}
static int socket_open(void){
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
#ifdef SO_NOSIGPIPE
    if(fd>=0){int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));}
#endif
    return fd;
}
bool qrx86_input_listen(QRX86InputListener *l,const char *path){
    struct sockaddr_un a;
    if(!l||!address(&a,path))return false;
    l->fd=-1;l->path[0]=0;
    int fd=socket_open();if(fd<0)return false;
    /* Do not use unlink() to steal another application's socket. */
    if(bind(fd,(const struct sockaddr *)&a,sizeof(a))!=0){close(fd);return false;}
    (void)chmod(path,0600);
    if(listen(fd,16)!=0){unlink(path);close(fd);return false;}
    l->fd=fd;memcpy(l->path,path,strlen(path)+1);
    return true;
}
void qrx86_input_close(QRX86InputListener *l){
    if(!l)return;
    if(l->fd>=0){close(l->fd);l->fd=-1;}
    if(l->path[0]){unlink(l->path);l->path[0]=0;}
}
bool qrx86_input_send(const char *path,const QRX86InputEvent *e){
    struct sockaddr_un a;
    if(!valid(e)||!address(&a,path))return false;
    int fd=socket_open();if(fd<0)return false;
    if(connect(fd,(const struct sockaddr *)&a,sizeof(a))!=0){close(fd);return false;}
    uint8_t wire[PACKET_SIZE];
    wr(wire,QRX86_INPUT_MAGIC);wr(wire+4,QRX86_INPUT_VERSION);
    wr(wire+8,e->type);wr(wire+12,e->code);wr(wire+16,e->character);
    wr(wire+20,e->x);wr(wire+24,e->y);wr(wire+28,e->button);
    int ok=transfer(fd,wire,sizeof(wire),1);close(fd);return ok!=0;
}
int qrx86_input_wait(QRX86InputListener *l,QRX86InputEvent *e,uint32_t timeout_ms){
    if(!l||l->fd<0||!e)return -1;
    if(timeout_ms>250u)timeout_ms=250u;
    fd_set fds;FD_ZERO(&fds);FD_SET(l->fd,&fds);
    struct timeval tv={(time_t)(timeout_ms/1000u),(suseconds_t)((timeout_ms%1000u)*1000u)};
    int ready=select(l->fd+1,&fds,NULL,NULL,&tv);
    if(ready<0)return errno==EINTR?0:-1;
    if(!ready)return 0;
    int fd=accept(l->fd,NULL,NULL);if(fd<0)return errno==EINTR?0:-1;
    struct timeval deadline={0,150000};
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline));
    uint8_t wire[PACKET_SIZE];int ok=transfer(fd,wire,sizeof(wire),0);close(fd);
    if(!ok||rd(wire)!=QRX86_INPUT_MAGIC||rd(wire+4)!=QRX86_INPUT_VERSION)return 0;
    QRX86InputEvent result={rd(wire+8),rd(wire+12),rd(wire+16),rd(wire+20),rd(wire+24),rd(wire+28)};
    if(!valid(&result))return 0;
    *e=result;return 1;
}
