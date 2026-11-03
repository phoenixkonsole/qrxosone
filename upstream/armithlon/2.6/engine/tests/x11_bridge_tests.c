#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx_x11_bridge.h"
#include "qrx86_ipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

typedef struct {int listener;QRXAccelStream *s;int good;} Receiver;
static void *read_frame(void *arg){
    Receiver *r=(Receiver*)arg;int fd=accept(r->listener,NULL,NULL);
    if(fd>=0){r->good=qrx86_ipc_read_one(fd,r->s);close(fd);}return NULL;
}
int main(void){
    uint32_t s[8]={0x00000000u,0x00ff0000u,0x0000ff00u,0x000000ffu,
                   0x00ffffffu,0x00ff00ffu,0x0000ffffu,0x00ffff00u};
    uint32_t d[8]={0},w=99,h=99;
    assert(qrx_x11_bridge_convert(s,4,2,d,8,&w,&h));
    assert(w==4&&h==2&&memcmp(s,d,sizeof(s))==0);
    /* Reject undersized buffers without writing or claiming valid dims. */
    d[0]=0xdeadcafe;
    assert(!qrx_x11_bridge_convert(s,4,2,d,7,&w,&h));
    assert(w==0&&h==0&&d[0]==0xdeadcafe);
    assert(!qrx_x11_bridge_convert(s,UINT32_MAX,2,d,8,&w,&h));
    assert(!qrx_x11_bridge_convert(NULL,4,2,d,8,&w,&h));
    uint32_t *large=calloc(1024u*768u,sizeof(uint32_t));
    uint32_t *output=calloc(800u*450u,sizeof(uint32_t));
    assert(large&&output);
    large[0]=0x00ff0011u;
    large[1024u*767u+1023u]=0x000000ffu;
    assert(qrx_x11_bridge_convert(large,1024,768,output,800u*450u,&w,&h));
    assert(w==600&&h==450); /* 4:3 kept, downscaled to 450 high */
    assert(output[0]==0x00ff0011u);
    assert(!qrx_x11_bridge_convert(large,1025,768,output,800u*450u,&w,&h));
    free(large);free(output);
    /* Actual local socket end-to-end: X11 pixels -> ARGB Q86F -> GPU frame. */
    char path[98];snprintf(path,sizeof(path),"/tmp/qrx-x11-%ld.sock",(long)getpid());
    int fd=socket(AF_UNIX,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_un addr={0};addr.sun_family=AF_UNIX;
    assert(strlen(path)<sizeof(addr.sun_path));strcpy(addr.sun_path,path);
    assert(bind(fd,(struct sockaddr*)&addr,sizeof(addr))==0);
    assert(listen(fd,1)==0);
    QRXAccelStream stream={0};assert(qrx_accel_init(&stream));
    Receiver receiver={fd,&stream,0};pthread_t th;
    assert(pthread_create(&th,NULL,read_frame,&receiver)==0);
    assert(setenv("ARMITHLON_QRX86_SOCKET",path,1)==0);
    assert(qrx_x11_bridge_present(s,4,2));
    pthread_join(th,NULL);assert(receiver.good);
    QRXAccelFrame *f=qrx_accel_take_latest(&stream);
    assert(f&&f->count==2&&f->commands[1].kind==QRX_AC_IMAGE);
    assert(f->commands[1].w==4&&f->commands[1].h==2);
    assert(f->commands[1].image[0]==0&&f->commands[1].image[3]==0xff);
    assert(f->commands[1].image[4]==0xff&&f->commands[1].image[5]==0);
    assert(f->commands[1].image[6]==0&&f->commands[1].image[7]==0xff);
    qrx_accel_frame_release(f);qrx_accel_destroy(&stream);
    close(fd);unlink(path);unsetenv("ARMITHLON_QRX86_SOCKET");
    assert(!qrx_x11_bridge_present(s,4,2));
    puts("QRX X11 -> native ARMITHLON socket -> Metal command stream PASS");
    return 0;
}
