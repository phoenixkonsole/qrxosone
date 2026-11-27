#ifndef QRX_EC64_BOARD_H
#define QRX_EC64_BOARD_H
/* The devices of the ARMithlon 68100EC64 board, in the core's device window (QEC64_MMIO_BASE and up,
 * long mode only). Registers are big endian like everything else; an access of the wrong size or to
 * an unknown register is a bus fault. See docs/armithlon/68100ec64.md, "ARMithlon board". */
#include "qrx68100ec64.h"
#include <stddef.h>
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
#define QEC64_HOST_VIEW         0x0130u  /* .l read: the host window's inside, width << 16 | height, in points; 0 headless */
#define QEC64_HOST_SCREEN       0x0134u  /* .l read: the host's screen, width << 16 | height, in points */
#define QEC64_FB_FRAMES         0x011Cu  /* .l, write: a frame is done (counted); read: the count */
#define QEC64_FB_FMT_RGB565     2u       /* big endian 16 bit */
#define QEC64_FB_FMT_XRGB32     5u       /* big endian 32 bit, the top byte unused */

/* Keyboard and mouse for drivers: one event per read, 0 when there is none.
 * Bits 31-24 kind (1 key down, 2 key up, 3 mouse move, 4 button), for keys 23-16 the macOS virtual
 * key code (the guest maps it to Amiga raw keys), for moves 23-12 x and 11-0 y in framebuffer pixels,
 * for buttons 23-16 the button (0 left, 1 right, 2 middle) and bit 0 set while it is pressed, for the
 * wheel 15-0 the steps, signed (positive: towards the user, the page moves up). */
#define QEC64_INPUT_EVENT       0x0200u  /* .l read */
#define QEC64_INPUT_KEY_DOWN    1u
#define QEC64_INPUT_KEY_UP      2u
#define QEC64_INPUT_MOVE        3u
#define QEC64_INPUT_BUTTON      4u
#define QEC64_INPUT_WHEEL       5u

/* Host clocks */
#define QEC64_CLOCK_US          0x0300u  /* .q read: a monotonic clock in microseconds */
#define QEC64_CLOCK_UNIX        0x0308u  /* .q read: seconds since 1970 (UTC) */
#define QEC64_CPU_INSTRUCTIONS  0x0310u  /* .q read: instructions the reading core has run so far */
#define QEC64_IDLE              0x0318u  /* .l write: the idle task, in a loop; counted as idle time */
#define QEC64_CORES             0x031Cu  /* .l read: the cores of the machine */
#define QEC64_IDLE_US           0x0320u  /* .q read at +8*n: microseconds core n slept in IDLE, n < 8 */
#define QEC64_RAM_SIZE          0x0360u  /* .q read: guest RAM in bytes */
#define QEC64_UPTIME_US         0x0368u  /* .q read: microseconds since the machine started */
#define QEC64_SMP_SPREAD        0x03C0u  /* .l read: 1 lets new tasks run on every core (QRX_EC64_SMP_SPREAD=1) */
#define QEC64_HOST_CPU          0x0380u  /* 64 bytes read as .q: the host CPU's name, ASCII, 0 padded */

/* Host services: a write of a request's RAM address runs it before the write returns. The request is
 * 32-bit big endian words, laid out like the 32-bit door's (qrx86_door.c): [0] the service, [3] the
 * operation, [4..16] arguments and results. Service 4 is the network (qrx_hostnet.h); buffers it names
 * must lie in RAM below 4 GB, else the call fails with the Winsock error 10014 (bad address). */
#define QEC64_HOST_CALL         0x0400u  /* .q write */
#define QEC64_HOST_NET          4u
/* Service 8 is OpenGL 2.1 (qrx_gl68k.c, guest library tools/qrxgl68k): [3] 1, [4..5] the address of
 * the command stream (high, low word), [6] its size in bytes; the last call's result in [8..9]. */
#define QEC64_HOST_GL           8u
#define QEC64_HOST_FEATURES     0x0408u  /* .l read: the services the host has */
#define QEC64_FEATURE_NET       1u
#define QEC64_FEATURE_GL        2u
#define QEC64_FEATURE_HOSTFS    4u   /* service 12, qrx_ec64_hostfs.h: a host directory is shared */

/* Sound: 48 kHz stereo, 16 bit. A frame is one big endian 32-bit word, left << 16 | right. The guest
 * writes the address and count of a run of frames, then GO queues them on the host's ring; frames that
 * do not fit are dropped, so a driver reads QUEUED first and keeps it near its target. */
#define QEC64_AUDIO_ADDR        0x0500u  /* .q */
#define QEC64_AUDIO_FRAMES      0x0508u  /* .l, at most QEC64_AUDIO_RING */
#define QEC64_AUDIO_GO          0x050Cu  /* .l write: queue; read: frames taken by the last GO */
#define QEC64_AUDIO_QUEUED      0x0510u  /* .l read: frames waiting to be played */
#define QEC64_AUDIO_RATE        0x0514u  /* .l read: 48000 */
#define QEC64_AUDIO_RING        65536u

typedef struct qec64_board {
    uint8_t *ram;
    uint64_t ram_size;
    /* the framebuffer registers, read by the display */
    volatile uint64_t fb_addr;
    volatile uint32_t fb_width, fb_height, fb_stride, fb_format, fb_enable, fb_frames;
    /* the host window, for the guest's display to follow (written by the window) */
    volatile uint32_t host_view, host_screen;
    /* input events from the window to the guest */
    uint32_t events[256];
    volatile unsigned ev_head, ev_tail;
    /* the cores' sleep in IDLE, and what QRXInfo shows of the host */
    unsigned cores;
    uint64_t idle_us[8], idle_last[8], start_us;
    char host_cpu[64];
    /* sound from the guest to the host's audio output */
    uint64_t audio_addr;
    uint32_t audio_frames, audio_taken;
    unsigned audio_read, audio_write;
    uint32_t audio_ring[QEC64_AUDIO_RING];
} qec64_board;

void qec64_board_init(qec64_board *b, uint8_t *ram, uint64_t ram_size);
/* For qec64_machine_set_mmio(), with the board as the user pointer */
bool qec64_board_mmio(qec64_cpu *c, uint64_t addr, unsigned size, bool write, uint64_t *value);
/* From the window's thread */
void qec64_board_push_event(qec64_board *b, uint32_t event);
/* From the audio output's thread: up to n frames as host endian 16-bit stereo, returns how many */
size_t qec64_board_pull_audio(qec64_board *b, int16_t *out, size_t n);
/* The framebuffer as the display should show it now; 0 while the console is shown */
int qec64_board_framebuffer(const qec64_board *b, const uint8_t **pixels, uint32_t *w, uint32_t *h,
                            uint32_t *stride, uint32_t *format);
#endif
