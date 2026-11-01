#ifndef QRX_ACCEL_H
#define QRX_ACCEL_H
/* Host-owned validated GPU command stream. No native pointers in guest ABI.
 * Producer: guest CPU thread. Consumer: render thread. Pending frame is transferred
 * by ownership; release only after host GPU finishes its use of the resources. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#define QRX_ACCEL_MAX_COMMANDS 512u
#define QRX_ACCEL_MAX_PAYLOAD (16u*1024u*1024u)
#define QRX_AC_CLEAR 1u
#define QRX_AC_RECT 2u
#define QRX_AC_IMAGE 3u
#define QRX_AC_TRIANGLE 4u
typedef struct QRXAccelVertex { float x,y,z,u,v,r,g,b,a; } QRXAccelVertex;
typedef struct QRXAccelCmd {
    uint32_t kind,flags, color, depth;
    uint32_t x,y,w,h, pitch;
    QRXAccelVertex vert[3];
    uint8_t *image;  /* host RGBA8 snapshot; owned by frame */
} QRXAccelCmd;
typedef struct QRXAccelFrame {
    uint64_t sequence;
    uint32_t count;
    size_t payload_bytes;
    QRXAccelCmd commands[QRX_ACCEL_MAX_COMMANDS];
} QRXAccelFrame;
typedef struct QRXAccelStream {
    pthread_mutex_t lock;
    QRXAccelFrame *building; /* producer only */
    QRXAccelFrame *ready;    /* exchange under lock */
    uint64_t submitted;
    uint64_t dropped;
    bool initialized;
} QRXAccelStream;
bool qrx_accel_init(QRXAccelStream *s);
void qrx_accel_destroy(QRXAccelStream *s);
void qrx_accel_frame_release(QRXAccelFrame *f);
QRXAccelFrame *qrx_accel_take_latest(QRXAccelStream *s);
bool qrx_accel_commit(QRXAccelStream *s);
bool qrx_accel_clear(QRXAccelStream *s,uint32_t color,uint32_t depth);
bool qrx_accel_rect(QRXAccelStream *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t color);
bool qrx_accel_image(QRXAccelStream *s,uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint8_t *src,size_t rowbytes);
bool qrx_accel_triangle(QRXAccelStream *s,const QRXAccelVertex v[3],uint32_t flags,
                        const uint8_t *texture,uint32_t width,uint32_t height,uint32_t stride);
#endif
