#include "qrx86_ipc.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
typedef struct {int fd;QRXAccelStream *out;bool ok;} ThreadArgs;
static void *receiver(void *p){ThreadArgs *a=p;a->ok=qrx86_ipc_read_one(a->fd,a->out);close(a->fd);return NULL;}
int main(void){
    int fds[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
    QRXAccelStream stream={0};assert(qrx_accel_init(&stream));
    ThreadArgs args={fds[1],&stream,false};pthread_t th;
    assert(pthread_create(&th,NULL,receiver,&args)==0);
    const uint8_t packet[]={
        'Q','8','6','F',0,0,0,2,0,0,0,1,0,0,0,8,
        0xff,0x11,0x22,0x33,0xff,0x44,0x55,0x66
    };
    assert(write(fds[0],packet,sizeof(packet))==(ssize_t)sizeof(packet));
    close(fds[0]);pthread_join(th,NULL);assert(args.ok);
    QRXAccelFrame *frame=qrx_accel_take_latest(&stream);
    assert(frame && frame->count==2 && frame->commands[1].image[0]==0x11 && frame->commands[1].image[4]==0x44);
    qrx_accel_frame_release(frame);
    /* Header validation: reject oversized/untrusted input before allocating. */
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
    const uint8_t invalid[]={ 'Q','8','6','F',0,0,0,1,0,0,1,0,0,0,0,4 };
    assert(write(fds[0],invalid,sizeof(invalid))==(ssize_t)sizeof(invalid));
    shutdown(fds[0],SHUT_WR);
    assert(!qrx86_ipc_read_one(fds[1],&stream));
    assert(!qrx_accel_take_latest(&stream));
    close(fds[0]);close(fds[1]);
    qrx_accel_destroy(&stream);
    puts("QRX86 host POSIX socket frame protocol, bounded decoder and Metal command transfer PASS");
    return 0;
}
