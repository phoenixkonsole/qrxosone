#ifndef QRX68100_CORE1_H
#define QRX68100_CORE1_H
/* Second core of the 68100: a host thread that runs guest code in shared RAM in parallel
 * to core 0, with its own registers and JIT cache. See guest/qrx68100_smp.h for how a
 * guest starts it. It has no chipset access and takes no interrupts, and AROS does not
 * schedule tasks onto it: it runs the jobs a program hands it. */
#include "qrx68100_full.h"
#include <stdbool.h>
/* Needs the JIT on core 0, otherwise false and the registers stay absent. */
bool qrx_core1_attach(QRXFullCPU *c0);
void qrx_core1_detach(QRXFullCPU *c0);
#endif
