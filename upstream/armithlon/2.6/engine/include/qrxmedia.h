#ifndef QRXMEDIA_H
#define QRXMEDIA_H
#include "qrxvirt.h"
#include "qrx_accel.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* QRXVIRT MEDIA ABI 0.4. This is NOT a SAGA, Voodoo or AHI implementation. */
#define QRX_MEDIA_GFX_BASE        (QRX_IO_BASE + 0x100u)
#define QRX_MEDIA_AUDIO_BASE      (QRX_IO_BASE + 0x200u)
#define QRX_MEDIA_INPUT_BASE      (QRX_IO_BASE + 0x300u)
/* QRXGPU v0.5 is a safe, bounded 2D command mailbox, not OpenGL/Direct3D yet.
 * All registers are big-endian 32-bit guest longwords. */
#define QRX_GPU_BASE              (QRX_IO_BASE + 0x400u)
#define QRX_GPU_MAGIC             UINT32_C(0x51524735) /* QRG5 */
#define QRX_GPU_ABI_VERSION       UINT32_C(0x00050000)
#define QRX_GPU_CAP_FILL          UINT32_C(1)
#define QRX_GPU_CAP_BLIT          UINT32_C(2)
#define QRX_GPU_CAP_PRESENT       UINT32_C(4)
#define QRX_GPU_CMD_CLEAR         1u
#define QRX_GPU_CMD_FILL          2u
#define QRX_GPU_CMD_BLIT          3u
#define QRX_GPU_CMD_PRESENT       4u
#define QRX_GPU_ERR_NONE          0u
#define QRX_GPU_ERR_BAD_COMMAND   1u
#define QRX_GPU_ERR_BAD_ARGUMENT  2u
#define QRX_GPU_ERR_BOUNDS        3u
#define QRX_GPU_ERR_NO_FB         4u
/* QRX3D versioned 68k-safe HLE protocol (not a Glide DLL or Voodoo card). */
#define QRX_3D_BASE              (QRX_IO_BASE + 0x500u)
#define QRX_3D_MAGIC             UINT32_C(0x51523344) /* QR3D */
#define QRX_3D_ABI_VERSION       UINT32_C(0x00060000)
#define QRX_3D_CAPS              UINT32_C(0x0000001F) /* clear,tri,swap,texture,depth */
#define QRX_3D_CMD_CLEAR         1u
#define QRX_3D_CMD_TRIANGLE      2u
#define QRX_3D_CMD_SWAP          3u
#define QRX_3D_CMD_TEXTURE       4u
#define QRX_3D_DEPTH_TEST        (1u<<0)
#define QRX_3D_DEPTH_WRITE       (1u<<1)
#define QRX_3D_ALPHA_BLEND       (1u<<2)
#define QRX_3D_TEXTURE           (1u<<3)
#define QRX_3D_CULL_BACK         (1u<<4)
#define QRX_3D_SUPPORTED_FLAGS   UINT32_C(0x1f)
#define QRX_3D_OK                0u
#define QRX_3D_ERR_COMMAND       1u
#define QRX_3D_ERR_ARGUMENT      2u
#define QRX_3D_ERR_BOUNDS        3u
#define QRX_3D_ERR_MEMORY        4u
#define QRX_3D_ERR_STATE         5u
#define QRX_MEDIA_FEATURE_3D     (1u<<7)
#define QRX_MEDIA_FEATURE_GPU2D  (1u << 6)
#define QRX_GFX_WIDTH            800u
#define QRX_GFX_HEIGHT           450u
#define QRX_GFX_PITCH            (QRX_GFX_WIDTH * 4u)
#define QRX_GFX_FRAMEBUFFER      UINT32_C(0x00200000)
#define QRX_GFX_FORMAT_XRGB8888  1u /* raw guest bytes: 00 R G B (BE 0x00RRGGBB) */
#define QRX_AUDIO_RATE           48000u
#define QRX_AUDIO_CHANNELS       2u
#define QRX_INPUT_KEY_DOWN       1u
#define QRX_INPUT_KEY_UP         2u
#define QRX_INPUT_MOUSE_MOVE     3u
#define QRX_INPUT_MOUSE_BUTTON   4u
#define QRX_INPUT_SCROLL         5u
#define QRX_INPUT_QUEUE_SIZE     256u
#define QRX_AUDIO_RING_SIZE      65536u
#define QRX_MEDIA_FEATURE_GFX    (1u << 3)
#define QRX_MEDIA_FEATURE_AUDIO  (1u << 4)
#define QRX_MEDIA_FEATURE_INPUT  (1u << 5)

/* SPSC: one GUI producer, one 68k consumer for input, one 68k producer,
 * one audio callback consumer for sound. Never allocate in audio callback. */
typedef struct QRXMediaDevice {
    void (*video_present)(void *ctx, const uint8_t *framebuffer,
                          uint32_t width, uint32_t height, uint32_t pitch);
    void *host_ctx;
    QRXAccelStream *accel;  /* optional native GPU command stream; null = exact software fallback */
    atomic_uint video_frames;
    uint32_t glide_args[8];
    uint32_t glide_error, glide_last_cmd, glide_success_count;
    uint32_t glide_tex_addr, glide_tex_stride, glide_tex_width, glide_tex_height;
    uint16_t *glide_depth; /* lazily allocated, host owned; no guest pointer */
    uint32_t gpu_args[6]; /* single-CPU 68k bus producer; no concurrent MMIO writers */
    uint32_t gpu_error;
    uint32_t gpu_last_cmd;
    uint32_t gpu_success_count;
    uint32_t audio_dma_address;
    uint32_t audio_dma_frames;
    uint32_t audio_dma_error;
    atomic_uint input_read;
    atomic_uint input_write;
    atomic_uint input_dropped;
    uint32_t input_events[QRX_INPUT_QUEUE_SIZE];
    atomic_uint mouse_x;
    atomic_uint mouse_y;
    atomic_uint mouse_buttons;
    atomic_uint audio_read;
    atomic_uint audio_write;
    atomic_uint audio_dropped;
    uint32_t audio_frames[QRX_AUDIO_RING_SIZE];
} QRXMediaDevice;

void qrx_media_init(QRXMediaDevice *media,
    void (*present)(void *, const uint8_t *, uint32_t, uint32_t, uint32_t),
    void *ctx);
void qrx_vm_bind_media(QRXVM *vm, QRXMediaDevice *media);
bool qrx_media_read(QRXVM *vm, uint32_t address, unsigned width, uint32_t *value);
bool qrx_media_write(QRXVM *vm, uint32_t address, unsigned width, uint32_t value);
bool qrx_glide_read(QRXVM *vm, uint32_t address, unsigned width, uint32_t *value);
bool qrx_glide_write(QRXVM *vm, uint32_t address, unsigned width, uint32_t value);
void qrx_glide_shutdown(QRXMediaDevice *media); /* call before disposing media */
bool qrx_gpu_read(QRXVM *vm, uint32_t address, unsigned width, uint32_t *value);
bool qrx_gpu_write(QRXVM *vm, uint32_t address, unsigned width, uint32_t value);
bool qrx_media_push_input(QRXMediaDevice *media, uint32_t packed_event);
void qrx_media_set_mouse(QRXMediaDevice *media, uint32_t x, uint32_t y, uint32_t buttons);
/* Output is interleaved signed 16 bit stereo in HOST endian, silence on underrun. */
/* Queues frames ('left<<16|right') for playback; returns how many fit. */
size_t qrx_media_push_audio(QRXMediaDevice *m,const uint32_t *frames,size_t n);
unsigned qrx_media_audio_queued(QRXMediaDevice *m);
size_t qrx_media_pull_audio(QRXMediaDevice *media, int16_t *interleaved, size_t frames);
#endif
