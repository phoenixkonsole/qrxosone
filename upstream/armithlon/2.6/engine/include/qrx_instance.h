#ifndef QRX_INSTANCE_H
#define QRX_INSTANCE_H
/* One QRX OS One instance inside an ARMithlon machine. The 64-bit system (AROS on the 68100EC64)
 * and, later, "Classic" (the 32-bit QRX OS One on the 68100 with the M1 JIT and its own libraries)
 * each run as an instance. Instances share nothing but what this interface names: a display,
 * input, the host file system (HostFS) and sound. Classic is not built against this yet; the
 * interface is here so that the 64-bit machine does not grow assumptions that would shut it out.
 * See docs/armithlon/qrx64-umstieg.md for the decision ("Classic now, native later"). */
#include <stddef.h>
#include <stdint.h>

typedef enum { QRX_INSTANCE_EC64 = 1, QRX_INSTANCE_CLASSIC = 2 } qrx_instance_kind;

/* What an instance shows: a framebuffer in its own guest RAM, or console text */
typedef struct {
    const uint8_t *pixels;          /* NULL: the instance shows its console */
    uint32_t width, height, stride;
    uint32_t format;                /* QEC64_FB_FMT_* values; Classic maps its RTG formats onto them */
    uint32_t frame;                 /* changes with every finished frame */
} qrx_instance_display;

typedef struct qrx_instance qrx_instance;
struct qrx_instance {
    qrx_instance_kind kind;
    const char *name;
    /* Start and stop. start returns 0 on success; stop ends the guest at once. */
    int (*start)(qrx_instance *self);
    void (*stop)(qrx_instance *self);
    int (*running)(const qrx_instance *self);
    /* The host asks for the picture at display rate, from its own thread */
    void (*display)(const qrx_instance *self, qrx_instance_display *out);
    /* Input from the host: typed text for a console, raw keys and the mouse for drivers
     * (the QEC64_INPUT_* event layout) */
    void (*typed)(qrx_instance *self, const char *text, size_t len);
    void (*event)(qrx_instance *self, uint32_t event);
    /* The host directory the instance sees as its shared drive, NULL for none */
    const char *hostfs;
    void *impl;
};
#endif
