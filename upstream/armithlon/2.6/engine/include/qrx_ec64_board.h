#ifndef QRX_EC64_BOARD_H
#define QRX_EC64_BOARD_H
/* The devices of the ARMithlon 68100EC64 board, in the core's device window (QEC64_MMIO_BASE and up,
 * long mode only). Registers are big endian like everything else; an access of the wrong size or to
 * an unknown register is a bus fault. See docs/armithlon/68100ec64.md, "ARMithlon board". */
#include "qrx68100ec64.h"
#include <stdint.h>

#define QEC64_BOARD_ID          0x0000u  /* .q read: "QRXEC64B" */
#define QEC64_BOARD_VERSION     0x0008u  /* .l read: 1 */
#define QEC64_BOARD_MAGIC       UINT64_C(0x5152584543363442)

/* Framebuffer in guest RAM. While FB_ENABLE is 0 the window shows the console. */
#define QEC64_FB_ADDR           0x0100u  /* .q */
#define QEC64_FB_WIDTH          0x0108u  /* .l, 1..4096 */
#define QEC64_FB_HEIGHT         0x010Cu  /* .l, 1..4096 */
#define QEC64_FB_STRIDE         0x0110u  /* .l, bytes per line */
#define QEC64_FB_FORMAT         0x0114u  /* .l, QEC64_FB_FMT_* */
#define QEC64_FB_ENABLE         0x0118u  /* .l, 1 shows the framebuffer */
#define QEC64_FB_FRAMES         0x011Cu  /* .l, write: a frame is done (counted); read: the count */
#define QEC64_FB_FMT_RGB565     2u       /* big endian 16 bit */
#define QEC64_FB_FMT_XRGB32     5u       /* big endian 32 bit, the top byte unused */

/* Keyboard and mouse for drivers: one event per read, 0 when there is none.
 * Bits 31-24 kind (1 key down, 2 key up, 3 mouse move, 4 button), for keys 23-16 the macOS virtual
 * key code (no Amiga raw key mapping yet), for moves 23-12 x and 11-0 y, for buttons 23-16 the
 * button and 0 down. */
#define QEC64_INPUT_EVENT       0x0200u  /* .l read */
#define QEC64_INPUT_KEY_DOWN    1u
#define QEC64_INPUT_KEY_UP      2u
#define QEC64_INPUT_MOVE        3u
#define QEC64_INPUT_BUTTON      4u

/* Host clocks */
#define QEC64_CLOCK_US          0x0300u  /* .q read: a monotonic clock in microseconds */
#define QEC64_CLOCK_UNIX        0x0308u  /* .q read: seconds since 1970 (UTC) */
#define QEC64_CPU_INSTRUCTIONS  0x0310u  /* .q read: instructions the reading core has run so far */

/* Host services: a write of a request's RAM address runs it before the write returns. The request is
 * 32-bit big endian words, laid out like the 32-bit door's (qrx86_door.c): [0] the service, [3] the
 * operation, [4..16] arguments and results. Service 4 is the network (qrx_hostnet.h); buffers it names
 * must lie in RAM below 4 GB, else the call fails with the Winsock error 10014 (bad address). */
#define QEC64_HOST_CALL         0x0400u  /* .q write */
#define QEC64_HOST_NET          4u

typedef struct qec64_board {
    uint8_t *ram;
    uint64_t ram_size;
    /* the framebuffer registers, read by the display */
    volatile uint64_t fb_addr;
    volatile uint32_t fb_width, fb_height, fb_stride, fb_format, fb_enable, fb_frames;
    /* input events from the window to the guest */
    uint32_t events[256];
    volatile unsigned ev_head, ev_tail;
} qec64_board;

void qec64_board_init(qec64_board *b, uint8_t *ram, uint64_t ram_size);
/* For qec64_machine_set_mmio(), with the board as the user pointer */
bool qec64_board_mmio(qec64_cpu *c, uint64_t addr, unsigned size, bool write, uint64_t *value);
/* From the window's thread */
void qec64_board_push_event(qec64_board *b, uint32_t event);
/* The framebuffer as the display should show it now; 0 while the console is shown */
int qec64_board_framebuffer(const qec64_board *b, const uint8_t **pixels, uint32_t *w, uint32_t *h,
                            uint32_t *stride, uint32_t *format);
#endif
