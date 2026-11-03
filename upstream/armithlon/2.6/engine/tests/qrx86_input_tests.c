#include "qrx86_input.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct{char path[100];QRX86InputEvent event;int sent;} Worker;
static void *produce(void *arg){Worker *w=arg;w->sent=qrx86_input_send(w->path,&w->event);return NULL;}
int main(void){
    char dir[]="/tmp/armithlon-input-XXXXXX"; assert(mkdtemp(dir));
    char path[104]; assert(snprintf(path,sizeof(path),"%s/in.sock",dir)<(int)sizeof(path));
    QRX86InputListener l={.fd=-1}; assert(qrx86_input_listen(&l,path));
    struct stat st;assert(stat(path,&st)==0 && (st.st_mode&0777)==0600);
    QRX86InputListener bad={.fd=-1};assert(!qrx86_input_listen(&bad,path));
    QRX86InputEvent out;assert(qrx86_input_wait(&l,&out,0)==0);
    Worker w={.event={QRX86_INPUT_MOUSE_DOWN,0,0,799,449,1}};
    strcpy(w.path,path);pthread_t t;assert(pthread_create(&t,NULL,produce,&w)==0);
    assert(qrx86_input_wait(&l,&out,200)==1);
    pthread_join(t,NULL);assert(w.sent==1);
    assert(out.type==QRX86_INPUT_MOUSE_DOWN && out.x==799 && out.y==449 && out.button==1);
    QRX86InputEvent invalid={QRX86_INPUT_MOUSE_MOVE,0,0,800,0,0};
    assert(!qrx86_input_send(path,&invalid));
    QRX86InputEvent key={QRX86_INPUT_KEY_DOWN,65,'a',0,0,0};
    assert(qrx86_input_send(path,&key));
    assert(qrx86_input_wait(&l,&out,200)==1);
    assert(out.code==65 && out.character=='a');
    qrx86_input_close(&l);assert(access(path,F_OK)!=0);
    assert(!qrx86_input_send(path,&key));rmdir(dir);
    puts("QRX86 bidirectional input protocol PASS");return 0;
}
