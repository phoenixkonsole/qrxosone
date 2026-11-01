#include "qrx_smp_ready.h"
#include <string.h>
static void enter(QRXSmpReady *q){while(atomic_flag_test_and_set_explicit(&q->gate,memory_order_acquire)){} }
static void leave(QRXSmpReady *q){atomic_flag_clear_explicit(&q->gate,memory_order_release);}
void qrx_smp_ready_init(QRXSmpReady *q){
    if(!q)return;
    memset(q,0,sizeof(*q));
    atomic_flag_clear(&q->gate);
    q->online_mask=QRX_SMP_CPU0;
}
void qrx_smp_ready_set_dual(QRXSmpReady *q,bool enabled){
    if(!q)return;
    enter(q);q->online_mask=enabled?(QRX_SMP_CPU0|QRX_SMP_CPU1):QRX_SMP_CPU0;leave(q);
}
bool qrx_smp_ready_submit(QRXSmpReady *q,QRXSmpWork w){
    if(!q||!w.id||!(w.affinity&3u)||(w.affinity&~3u))return false;
    enter(q);
    for(unsigned i=0;i<QRX_SMP_READY_MAX;i++){
        if(q->used[i])continue;
        q->slots[i]=w;q->used[i]=1;q->submitted++;leave(q);return true;
    }
    leave(q);return false;
}
bool qrx_smp_ready_take(QRXSmpReady *q,unsigned core,QRXSmpWork *out){
    if(!q||!out||core>1)return false;
    enter(q);
    unsigned mask=1u<<core;
    if(!(q->online_mask&mask)){leave(q);return false;}
    int best=-1;
    for(unsigned i=0;i<QRX_SMP_READY_MAX;i++){
        if(q->used[i] && (q->slots[i].affinity&mask) &&
           (best<0||q->slots[i].priority>q->slots[best].priority))best=(int)i;
    }
    if(best<0){leave(q);return false;}
    *out=q->slots[best];q->used[best]=0;q->completed++;leave(q);return true;
}
unsigned qrx_smp_ready_depth(QRXSmpReady *q){
    if(!q)return 0;enter(q);unsigned n=0;
    for(unsigned i=0;i<QRX_SMP_READY_MAX;i++)n+=q->used[i];
    leave(q);return n;
}
