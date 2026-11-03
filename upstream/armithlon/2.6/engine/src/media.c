#include "qrxmedia.h"
#include <string.h>

void qrx_media_init(QRXMediaDevice *m,
    void (*present)(void *, const uint8_t *, uint32_t, uint32_t, uint32_t),
    void *ctx) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->video_present = present;
    m->host_ctx = ctx;
}
void qrx_vm_bind_media(QRXVM *vm, QRXMediaDevice *m) {if (vm) vm->media=m;}

bool qrx_media_push_input(QRXMediaDevice *m, uint32_t event) {
    if (!m) return false;
    unsigned w=atomic_load_explicit(&m->input_write,memory_order_relaxed);
    unsigned r=atomic_load_explicit(&m->input_read,memory_order_acquire);
    if (w-r >= QRX_INPUT_QUEUE_SIZE) {
        atomic_fetch_add_explicit(&m->input_dropped,1,memory_order_relaxed);
        return false;
    }
    m->input_events[w % QRX_INPUT_QUEUE_SIZE]=event;
    atomic_store_explicit(&m->input_write,w+1,memory_order_release);
    return true;
}
void qrx_media_set_mouse(QRXMediaDevice *m, uint32_t x,uint32_t y,uint32_t buttons) {
    if (!m) return;
    atomic_store_explicit(&m->mouse_x,x,memory_order_relaxed);
    atomic_store_explicit(&m->mouse_y,y,memory_order_relaxed);
    atomic_store_explicit(&m->mouse_buttons,buttons,memory_order_relaxed);
}
static bool pop_input(QRXMediaDevice *m,uint32_t *event) {
    unsigned r=atomic_load_explicit(&m->input_read,memory_order_relaxed);
    unsigned w=atomic_load_explicit(&m->input_write,memory_order_acquire);
    if (r==w) {*event=0; return true;}
    *event=m->input_events[r % QRX_INPUT_QUEUE_SIZE];
    atomic_store_explicit(&m->input_read,r+1,memory_order_release);
    return true;
}
static bool push_audio(QRXMediaDevice *m,uint32_t frame) {
    unsigned w=atomic_load_explicit(&m->audio_write,memory_order_relaxed);
    unsigned r=atomic_load_explicit(&m->audio_read,memory_order_acquire);
    if (w-r >= QRX_AUDIO_RING_SIZE) {
        atomic_fetch_add_explicit(&m->audio_dropped,1,memory_order_relaxed);
        return false;
    }
    m->audio_frames[w % QRX_AUDIO_RING_SIZE]=frame;
    atomic_store_explicit(&m->audio_write,w+1,memory_order_release);
    return true;
}
size_t qrx_media_pull_audio(QRXMediaDevice *m,int16_t *dst,size_t frames) {
    if (!m || !dst) return 0;
    unsigned r=atomic_load_explicit(&m->audio_read,memory_order_relaxed);
    unsigned w=atomic_load_explicit(&m->audio_write,memory_order_acquire);
    size_t have=(size_t)(w-r);
    if (have>frames) have=frames;
    for (size_t i=0;i<have;i++) {
        uint32_t f=m->audio_frames[(r+(unsigned)i)%QRX_AUDIO_RING_SIZE];
        dst[i*2]=(int16_t)(f>>16);
        dst[i*2+1]=(int16_t)f;
    }
    for (size_t i=have;i<frames;i++) dst[i*2]=dst[i*2+1]=0;
    atomic_store_explicit(&m->audio_read,r+(unsigned)have,memory_order_release);
    return have;
}

bool qrx_media_read(QRXVM *v,uint32_t addr,unsigned width,uint32_t *value) {
    if (!v || !v->media || !value || width!=4 || (addr&3u)) return false;
    QRXMediaDevice *m=v->media;
    switch (addr) {
        case QRX_MEDIA_GFX_BASE+0x00: *value=QRX_GFX_WIDTH; return true;
        case QRX_MEDIA_GFX_BASE+0x04: *value=QRX_GFX_HEIGHT; return true;
        case QRX_MEDIA_GFX_BASE+0x08: *value=QRX_GFX_PITCH; return true;
        case QRX_MEDIA_GFX_BASE+0x0c: *value=QRX_GFX_FRAMEBUFFER; return true;
        case QRX_MEDIA_GFX_BASE+0x10: *value=QRX_GFX_FORMAT_XRGB8888; return true;
        case QRX_MEDIA_GFX_BASE+0x18: *value=atomic_load(&m->video_frames); return true;
        case QRX_MEDIA_AUDIO_BASE+0x00: *value=QRX_AUDIO_RATE; return true;
        case QRX_MEDIA_AUDIO_BASE+0x04: *value=QRX_AUDIO_CHANNELS; return true;
        case QRX_MEDIA_AUDIO_BASE+0x0c:
            *value=atomic_load_explicit(&m->audio_write,memory_order_acquire)-
                   atomic_load_explicit(&m->audio_read,memory_order_acquire); return true;
        case QRX_MEDIA_AUDIO_BASE+0x10: *value=atomic_load(&m->audio_dropped); return true;
        case QRX_MEDIA_AUDIO_BASE+0x14: *value=m->audio_dma_address; return true;
        case QRX_MEDIA_AUDIO_BASE+0x18: *value=m->audio_dma_frames; return true;
        case QRX_MEDIA_AUDIO_BASE+0x20: *value=m->audio_dma_error; return true;
        case QRX_MEDIA_INPUT_BASE+0x00:
            *value=atomic_load_explicit(&m->input_write,memory_order_acquire)-
                   atomic_load_explicit(&m->input_read,memory_order_acquire); return true;
        case QRX_MEDIA_INPUT_BASE+0x04: return pop_input(m,value);
        case QRX_MEDIA_INPUT_BASE+0x08: *value=atomic_load(&m->mouse_x); return true;
        case QRX_MEDIA_INPUT_BASE+0x0c: *value=atomic_load(&m->mouse_y); return true;
        case QRX_MEDIA_INPUT_BASE+0x10: *value=atomic_load(&m->mouse_buttons); return true;
        case QRX_MEDIA_INPUT_BASE+0x14: *value=atomic_load(&m->input_dropped); return true;
        default: return false;
    }
}
bool qrx_media_write(QRXVM *v,uint32_t addr,unsigned width,uint32_t value) {
    if (!v || !v->media || width!=4 || (addr&3u)) return false;
    QRXMediaDevice *m=v->media;
    if (addr==QRX_MEDIA_GFX_BASE+0x14 && value==1) {
        if (v->ram.size < (uint64_t)QRX_GFX_FRAMEBUFFER+(uint64_t)QRX_GFX_PITCH*QRX_GFX_HEIGHT)
            return false;
        if (m->accel && !qrx_accel_commit(m->accel))return false;
        if (!m->accel && m->video_present)
            m->video_present(m->host_ctx,v->ram.data+QRX_GFX_FRAMEBUFFER,
                             QRX_GFX_WIDTH,QRX_GFX_HEIGHT,QRX_GFX_PITCH);
        atomic_fetch_add(&m->video_frames,1);
        return true;
    }
    if (addr==QRX_MEDIA_AUDIO_BASE+0x08) return push_audio(m,value);
    if (addr==QRX_MEDIA_AUDIO_BASE+0x14) {m->audio_dma_address=value; return true;}
    if (addr==QRX_MEDIA_AUDIO_BASE+0x18) {m->audio_dma_frames=value; return true;}
    if (addr==QRX_MEDIA_AUDIO_BASE+0x1c && value==1) {
        /* Bounded, BE signed-16 stereo DMA from guest RAM to the SPSC ring.
         * AHI driver can fill RAM once, then submit one mailbox command. */
        uint32_t src=m->audio_dma_address,n=m->audio_dma_frames;
        unsigned wr=atomic_load_explicit(&m->audio_write,memory_order_relaxed);
        unsigned rd=atomic_load_explicit(&m->audio_read,memory_order_acquire);
        m->audio_dma_error=1;
        if (!n || n>4096 || (src&3u) || !v->ram.data ||
            (uint64_t)src+(uint64_t)n*4u>v->ram.size ||
            /* RAM continues above the I/O window on boards with more than 16 MiB. */
            ((uint64_t)src<(uint64_t)QRX_IO_BASE+QRX_IO_LENGTH &&
             (uint64_t)src+(uint64_t)n*4u>QRX_IO_BASE) ||
            (unsigned)(wr-rd)>QRX_AUDIO_RING_SIZE || n>QRX_AUDIO_RING_SIZE-(wr-rd))
            return true;
        for(uint32_t i=0;i<n;i++){
            const uint8_t *p=v->ram.data+(size_t)src+(size_t)i*4u;
            uint32_t f=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
            m->audio_frames[(wr+i)%QRX_AUDIO_RING_SIZE]=f;
        }
        atomic_store_explicit(&m->audio_write,wr+n,memory_order_release);
        m->audio_dma_error=0;
        return true;
    }
    return false;
}
