#ifndef QRX86_DOOR_H
#define QRX86_DOOR_H
/* Host door for QRX86: lets qrx-win32 and qrx-i386 in the guest run x86 code on
 * the Mac's CPU instead of through the 68k interpreter. Both sides share one
 * interpreter source; x86 pages already live in guest RAM. Registers are at
 * FF000880: +0 magic 'Q86D', +4 write the guest address of a request block. */
#include "qrx68100_full.h"
#define QRX86_DOOR_BASE UINT32_C(0xFF000880)
#define QRX86_DOOR_SIZE UINT32_C(0x80)
#define QRX86_DOOR_MAGIC UINT32_C(0x51383644)
void qrx86_door_attach(QRXFullCPU *c);
void qrx86_door_report(void);
#endif
