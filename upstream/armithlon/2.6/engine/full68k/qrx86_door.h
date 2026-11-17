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
/* +16 reads feature bits. Service 4 with no page directory: sockets for native 68k programs. */
#define QRX86_DOOR_FEATURE_NATIVE_NET UINT32_C(1)
/* Service 8: OpenGL 2.1 for native 68k programs (qrx_gl68k.c). */
#define QRX86_DOOR_FEATURE_GL UINT32_C(2)
/* +20 reads the host window's inside, width << 16 | height in points, for the guest's display to
 * follow it (QRXDesktop's guest tools); 0 without a window. */
#define QRX86_DOOR_FEATURE_HOST_VIEW UINT32_C(4)
extern volatile uint32_t qrx_host_view_size;
void qrx86_door_attach(QRXFullCPU *c);
void qrx86_door_report(void);
#endif
