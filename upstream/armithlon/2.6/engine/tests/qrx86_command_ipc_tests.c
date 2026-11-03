#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx86_ipc.h"
#include "qrx_gl_compat.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
typedef struct {int srv;QRXAccelStream *stream;bool ok;} Receiver;
static void *handle(void *p){Receiver *r=p;int fd=accept(r->srv,NULL,NULL);r->ok=fd>=0 && qrx86_ipc_read_one(fd,r->stream);if(fd>=0)close(fd);return NULL;}
int main(void){
    QRXAccelStream in={0},out={0};assert(qrx_accel_init(&in));assert(qrx_accel_init(&out));
    QRXGLContext gl;qrx_gl_init(&gl,&in);
    qrx_gl_clear_color(&gl,0,0,0,1);qrx_gl_clear(&gl,QRX_GL_COLOR_BUFFER_BIT);
    qrx_gl_begin(&gl,QRX_GL_TRIANGLES);
    qrx_gl_vertex(&gl,-1,-1,0);qrx_gl_vertex(&gl,1,-1,0);qrx_gl_vertex(&gl,0,1,0);
    qrx_gl_end(&gl);assert(qrx_gl_flush(&gl));
    QRXAccelFrame *f=qrx_accel_take_latest(&in);assert(f&&f->count==2);
    char path[92];snprintf(path,sizeof(path),"/tmp/qrx-gl-%ld.sock",(long)getpid());
    int listener=socket(AF_UNIX,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_un a={0};a.sun_family=AF_UNIX;strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    assert(bind(listener,(struct sockaddr *)&a,sizeof(a))==0);assert(listen(listener,1)==0);
    Receiver receiver={listener,&out,false};pthread_t th;assert(pthread_create(&th,NULL,handle,&receiver)==0);
    assert(qrx86_ipc_send_commands(path,f));pthread_join(th,NULL);assert(receiver.ok);
    QRXAccelFrame *g=qrx_accel_take_latest(&out);assert(g && g->count==2);
    assert(g->commands[0].kind==QRX_AC_CLEAR);
    assert(g->commands[1].kind==QRX_AC_TRIANGLE);
    assert(g->commands[1].vert[0].y==450.f);
    qrx_accel_frame_release(f);qrx_accel_frame_release(g);
    close(listener);unlink(path);qrx_accel_destroy(&in);qrx_accel_destroy(&out);
    puts("QRX86 Q86C command stream -> QRXGPU socket: PASS");return 0;
}
