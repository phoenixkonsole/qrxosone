#ifndef QRX_SMP_READY_H
#define QRX_SMP_READY_H
/* Shared ready queue / CPU-affinity proof for a future AROS m68k SMP Exec
 * scheduler. Does NOT schedule Exec tasks or run Musashi on two host threads.
 * 68040 dualcore hardware must be explicitly enabled by QRX-aware kernel. */
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#define QRX_SMP_READY_MAX 64u
#define QRX_SMP_CPU0 1u
#define QRX_SMP_CPU1 2u
typedef struct {
    uint32_t id, payload;
    uint8_t affinity; /* 1=BSP, 2=AP, 3=either */
    uint8_t priority; /* higher first */
} QRXSmpWork;
typedef struct {
    atomic_flag gate;
    uint8_t online_mask, used[QRX_SMP_READY_MAX];
    uint32_t submitted, completed;
    QRXSmpWork slots[QRX_SMP_READY_MAX];
} QRXSmpReady;
void qrx_smp_ready_init(QRXSmpReady *q);
void qrx_smp_ready_set_dual(QRXSmpReady *q,bool enabled);
bool qrx_smp_ready_submit(QRXSmpReady *q,QRXSmpWork job);
bool qrx_smp_ready_take(QRXSmpReady *q,unsigned core,QRXSmpWork *out);
unsigned qrx_smp_ready_depth(QRXSmpReady *q);
#endif
