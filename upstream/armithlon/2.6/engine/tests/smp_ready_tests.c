#include "qrx_smp_ready.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#define JOBS 32
static QRXSmpReady q;
static atomic_uint counts[2];
static atomic_uint total;
static void *worker(void *ctx){
 unsigned cpu=(unsigned)(uintptr_t)ctx;
 for(unsigned spins=0;spins<1000000;spins++){
    QRXSmpWork w;
    if(qrx_smp_ready_take(&q,cpu,&w)){
        assert(w.affinity&(1u<<cpu));
        atomic_fetch_add(&counts[cpu],1);atomic_fetch_add(&total,w.payload);
    }else if(qrx_smp_ready_depth(&q)==0)break;
 }
 return NULL;
}
int main(void){
 qrx_smp_ready_init(&q);
 assert(qrx_smp_ready_submit(&q,(QRXSmpWork){.id=1,.affinity=2,.priority=20,.payload=100}));
 QRXSmpWork w={0};assert(!qrx_smp_ready_take(&q,1,&w));
 assert(!qrx_smp_ready_take(&q,0,&w));
 assert(qrx_smp_ready_depth(&q)==1);
 qrx_smp_ready_set_dual(&q,true);
 assert(qrx_smp_ready_take(&q,1,&w)&&w.id==1&&w.payload==100);
 assert(qrx_smp_ready_submit(&q,(QRXSmpWork){.id=42,.affinity=1,.priority=1,.payload=1}));
 assert(qrx_smp_ready_submit(&q,(QRXSmpWork){.id=43,.affinity=1,.priority=100,.payload=1}));
 assert(qrx_smp_ready_take(&q,0,&w)&&w.id==43);
 assert(qrx_smp_ready_take(&q,0,&w)&&w.id==42);
 for(unsigned i=1;i<=JOBS;i++){
    QRXSmpWork job={.id=i+1,.priority=(uint8_t)(i%7),.payload=1,.affinity=(i%4==0)?1u:(i%4==1)?2u:3u};
    assert(qrx_smp_ready_submit(&q,job));
 }
 pthread_t a,b;
 assert(!pthread_create(&a,NULL,worker,(void*)(uintptr_t)0));
 assert(!pthread_create(&b,NULL,worker,(void*)(uintptr_t)1));
 pthread_join(a,NULL);pthread_join(b,NULL);
 assert(qrx_smp_ready_depth(&q)==0);
 assert(atomic_load(&total)==JOBS);
 assert(atomic_load(&counts[0])>0&&atomic_load(&counts[1])>0);
 qrx_smp_ready_set_dual(&q,false);
 assert(qrx_smp_ready_submit(&q,(QRXSmpWork){.id=99,.affinity=3,.payload=9}));
 assert(!qrx_smp_ready_take(&q,1,&w));
 assert(qrx_smp_ready_take(&q,0,&w)&&w.id==99);
 puts("PASS QRX SMP ready-queue: affinity, priority, UP fallback, concurrent workers");
 return 0;
}
