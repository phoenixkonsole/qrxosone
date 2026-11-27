/* QRX Linux I/O: display, input, console and sound straight on the Linux kernel, see qrx_lio.h. */
#define _GNU_SOURCE
#include "qrx_lio.h"
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>

static int testing;                 /* QRX_LIO_TEST: no hardware */
static const char *input_dir = "/dev/input";

/* ------------------------------------------------------------------ display */
typedef struct { uint32_t handle, fb, pitch; uint64_t size; uint8_t *map; } Buf;
static int drm = -1;
static uint32_t crtc_id, conn_id, plane_id;
static unsigned crtc_index;
static struct drm_mode_modeinfo mode;
static unsigned screen_w = 1280, screen_h = 720;
static Buf primary[2], frame_buf[2], cursor_buf;
static int primary_front, frame_front, use_plane, have_cursor;
static uint32_t *frame_mem;                       /* the frame as the caller writes it */
static unsigned frame_w, frame_h, prev_y0, prev_y1;
static unsigned dst_x, dst_y, dst_w, dst_h;       /* where the frame lands on the screen */
static int released;                              /* our terminal is not shown: hands off */
static int pointer_shape = -2, ptr_x, ptr_y, hot_x, hot_y;  /* the pointer: shape, frame pixels */

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static int buf_create(Buf *b, unsigned w, unsigned h)
{
    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (xioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return 0;
    b->handle = cd.handle; b->pitch = cd.pitch; b->size = cd.size;
    struct drm_mode_fb_cmd2 f = { .width = w, .height = h, .pixel_format = DRM_FORMAT_XRGB8888 };
    f.handles[0] = cd.handle; f.pitches[0] = cd.pitch;
    if (xioctl(drm, DRM_IOCTL_MODE_ADDFB2, &f)) return 0;
    b->fb = f.fb_id;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (xioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md)) return 0;
    b->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, (off_t)md.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; return 0; }
    memset(b->map, 0, cd.size);
    return 1;
}
static void buf_free(Buf *b)
{
    if (b->map) munmap(b->map, b->size);
    if (b->fb) xioctl(drm, DRM_IOCTL_MODE_RMFB, &b->fb);
    if (b->handle) { struct drm_mode_destroy_dumb d = { .handle = b->handle }; xioctl(drm, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
    memset(b, 0, sizeof *b);
}

/* the first card with a connected screen: vc4 on the Pi (v3d, the 3D unit, has no outputs) */
static int find_output(void)
{
    char path[32];
    for (int n = 0; n < 8; n++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", n);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_mode_card_res res = {0};
        if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) { close(fd); continue; }
        uint32_t conns[res.count_connectors], crtcs[res.count_crtcs], encs[res.count_encoders ? res.count_encoders : 1];
        res.count_fbs = 0;
        res.connector_id_ptr = (uintptr_t)conns; res.crtc_id_ptr = (uintptr_t)crtcs; res.encoder_id_ptr = (uintptr_t)encs;
        if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { close(fd); continue; }
        for (unsigned i = 0; i < res.count_connectors; i++) {
            struct drm_mode_get_connector c = { .connector_id = conns[i] };
            if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != 1 || !c.count_modes) continue;
            struct drm_mode_modeinfo modes[c.count_modes];
            uint32_t cencs[c.count_encoders ? c.count_encoders : 1];
            c.modes_ptr = (uintptr_t)modes; c.encoders_ptr = (uintptr_t)cencs;
            c.count_props = 0;
            if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || !c.count_modes) continue;
            /* QRX_MODE=WxH, else the screen's preferred mode, else its first */
            int pick = 0;
            unsigned want_w = 0, want_h = 0;
            const char *m = getenv("QRX_MODE");
            if (m) sscanf(m, "%ux%u", &want_w, &want_h);
            for (unsigned k = 0; k < c.count_modes; k++) if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { pick = (int)k; break; }
            for (unsigned k = 0; want_w && k < c.count_modes; k++)
                if (modes[k].hdisplay == want_w && modes[k].vdisplay == want_h) { pick = (int)k; break; }
            uint32_t enc_id = c.encoder_id ? c.encoder_id : c.count_encoders ? cencs[0] : 0;
            struct drm_mode_get_encoder e = { .encoder_id = enc_id };
            if (!enc_id || xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e)) continue;
            int ci = -1;
            for (unsigned k = 0; k < res.count_crtcs; k++) if (crtcs[k] == e.crtc_id) ci = (int)k;
            for (unsigned k = 0; ci < 0 && k < res.count_crtcs; k++) if (e.possible_crtcs & (1u << k)) ci = (int)k;
            if (ci < 0) continue;
            drm = fd; conn_id = conns[i]; crtc_id = crtcs[ci]; crtc_index = (unsigned)ci; mode = modes[pick];
            screen_w = mode.hdisplay; screen_h = mode.vdisplay;
            fprintf(stderr, "[LIO] %s: %ux%u@%u on connector %u\n", path, screen_w, screen_h, mode.vrefresh, conn_id);
            return 1;
        }
        close(fd);
    }
    return 0;
}

/* an overlay plane on our CRTC that takes XRGB8888: the display hardware scales it */
static uint32_t find_plane(void)
{
    struct drm_mode_get_plane_res pr = {0};
    if (xioctl(drm, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || !pr.count_planes) return 0;
    uint32_t ids[pr.count_planes];
    pr.plane_id_ptr = (uintptr_t)ids;
    if (xioctl(drm, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return 0;
    for (unsigned i = 0; i < pr.count_planes; i++) {
        struct drm_mode_get_plane p = { .plane_id = ids[i] };
        if (xioctl(drm, DRM_IOCTL_MODE_GETPLANE, &p) || !(p.possible_crtcs & (1u << crtc_index)) || !p.count_format_types) continue;
        uint32_t fmts[p.count_format_types];
        p.format_type_ptr = (uintptr_t)fmts;
        if (xioctl(drm, DRM_IOCTL_MODE_GETPLANE, &p)) continue;
        for (unsigned k = 0; k < p.count_format_types; k++) if (fmts[k] == DRM_FORMAT_XRGB8888) return ids[i];
    }
    return 0;
}

static int set_crtc(void)
{
    struct drm_mode_crtc cr = { .crtc_id = crtc_id, .fb_id = primary[primary_front].fb, .count_connectors = 1,
                                .set_connectors_ptr = (uintptr_t)&conn_id, .mode = mode, .mode_valid = 1 };
    return xioctl(drm, DRM_IOCTL_MODE_SETCRTC, &cr) == 0;
}

static void place_frame(void)
{
    double sx = (double)screen_w / frame_w, sy = (double)screen_h / frame_h, s = sx < sy ? sx : sy;
    if (getenv("QRX_SCALE_INTEGER") && s >= 1.0) s = (double)(unsigned)s;
    dst_w = (unsigned)(frame_w * s + 0.5); dst_h = (unsigned)(frame_h * s + 0.5);
    if (dst_w > screen_w) dst_w = screen_w;
    if (dst_h > screen_h) dst_h = screen_h;
    dst_x = (screen_w - dst_w) / 2; dst_y = (screen_h - dst_h) / 2;
}

static int set_plane(const Buf *b)
{
    struct drm_mode_set_plane sp = { .plane_id = plane_id, .crtc_id = crtc_id, .fb_id = b->fb,
        .crtc_x = (int32_t)dst_x, .crtc_y = (int32_t)dst_y, .crtc_w = dst_w, .crtc_h = dst_h,
        .src_w = frame_w << 16, .src_h = frame_h << 16 };
    return xioctl(drm, DRM_IOCTL_MODE_SETPLANE, &sp) == 0;
}

/* nearest-neighbour scaling of frame rows y0..y1-1 into a screen buffer (no scaling plane) */
static void scale_rows(Buf *b, unsigned y0, unsigned y1)
{
    if (!dst_w || !dst_h) return;
    unsigned dy0 = (unsigned)((uint64_t)y0 * dst_h / frame_h), dy1 = (unsigned)(((uint64_t)y1 * dst_h + frame_h - 1) / frame_h);
    if (dy1 > dst_h) dy1 = dst_h;
    static unsigned *xmap, xmap_w, xmap_src;
    if (xmap_w != dst_w || xmap_src != frame_w) {
        free(xmap);
        xmap = malloc(dst_w * sizeof *xmap);
        if (!xmap) { xmap_w = 0; return; }
        for (unsigned x = 0; x < dst_w; x++) xmap[x] = (unsigned)((uint64_t)x * frame_w / dst_w);
        xmap_w = dst_w; xmap_src = frame_w;
    }
    for (unsigned dy = dy0; dy < dy1; dy++) {
        const uint32_t *src = frame_mem + (size_t)((uint64_t)dy * frame_h / dst_h) * frame_w;
        uint32_t *o = (uint32_t *)(void *)(b->map + (size_t)(dst_y + dy) * b->pitch) + dst_x;
        for (unsigned x = 0; x < dst_w; x++) o[x] = src[xmap[x]];
    }
}

void qrx_lio_screen(unsigned *w, unsigned *h) { *w = screen_w; *h = screen_h; }

uint32_t *qrx_lio_frame_begin(unsigned w, unsigned h, unsigned *pitch)
{
    if (!w || !h || w > 8192 || h > 8192) return NULL;
    if (w != frame_w || h != frame_h || !frame_mem) {
        free(frame_mem);
        frame_mem = calloc((size_t)w * h, 4);
        if (!frame_mem) return NULL;
        frame_w = w; frame_h = h;
        place_frame();
        prev_y0 = 0; prev_y1 = h;
        if (drm >= 0) {
            buf_free(&frame_buf[0]); buf_free(&frame_buf[1]);
            use_plane = plane_id && buf_create(&frame_buf[0], w, h) && buf_create(&frame_buf[1], w, h) &&
                        !released && set_plane(&frame_buf[frame_front]);
            if (!use_plane) { buf_free(&frame_buf[0]); buf_free(&frame_buf[1]); }
            for (int i = 0; i < 2; i++) if (primary[i].map) memset(primary[i].map, 0, primary[i].size);
            fprintf(stderr, "[LIO] frame %ux%u -> %ux%u at %u,%u, %s\n", w, h, dst_w, dst_h, dst_x, dst_y,
                    use_plane ? "scaled by the display hardware" : "scaled by the CPU");
        }
    }
    *pitch = frame_w * 4;
    return frame_mem;
}

void qrx_lio_frame_end(unsigned y0, unsigned y1)
{
    if (!frame_mem || y0 >= y1 || y1 > frame_h) return;
    if (drm < 0 || released) {
        prev_y0 = 0; prev_y1 = frame_h;
        if (testing) usleep(16667);          /* no vertical blank to wait for: 60 frames a second */
        return;
    }
    /* the back buffer missed the rows of the last frame as well */
    unsigned a = y0 < prev_y0 ? y0 : prev_y0, b = y1 > prev_y1 ? y1 : prev_y1;
    prev_y0 = y0; prev_y1 = y1;
    if (use_plane) {
        Buf *back = &frame_buf[frame_front ^ 1];
        for (unsigned y = a; y < b; y++) memcpy(back->map + (size_t)y * back->pitch, frame_mem + (size_t)y * frame_w, frame_w * 4);
        if (set_plane(back)) frame_front ^= 1;           /* waits for the vertical blank */
    } else {
        Buf *back = &primary[primary_front ^ 1];
        scale_rows(back, a, b);
        struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc_id, .fb_id = back->fb, .flags = DRM_MODE_PAGE_FLIP_EVENT };
        if (xioctl(drm, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
            char ev[256];
            struct pollfd p = { drm, POLLIN, 0 };
            if (poll(&p, 1, 100) > 0) (void)!read(drm, ev, sizeof ev);    /* the flip happened */
            primary_front ^= 1;
        }
    }
}

int qrx_lio_capture(const char *path)
{
    if (!frame_mem) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%u %u\n255\n", screen_w, screen_h);
    uint8_t *row = malloc((size_t)screen_w * 3);
    for (unsigned y = 0; row && y < screen_h; y++) {
        for (unsigned x = 0; x < screen_w; x++) {
            uint32_t v = 0;
            if (x >= dst_x && x < dst_x + dst_w && y >= dst_y && y < dst_y + dst_h)
                v = frame_mem[(size_t)((uint64_t)(y - dst_y) * frame_h / dst_h) * frame_w + (uint64_t)(x - dst_x) * frame_w / dst_w];
            if (pointer_shape >= 0 && cursor_buf.map && frame_w) {      /* the pointer on top */
                int cx = (int)x - ((int)dst_x + (int)((int64_t)ptr_x * dst_w / frame_w) - hot_x);
                int cy = (int)y - ((int)dst_y + (int)((int64_t)ptr_y * dst_h / frame_h) - hot_y);
                if (cx >= 0 && cy >= 0 && cx < 64 && cy < 64) {
                    uint32_t c = ((uint32_t *)(void *)cursor_buf.map)[cy * 64 + cx], a = c >> 24;
                    uint32_t r = (c >> 16 & 255) + (v >> 16 & 255) * (255 - a) / 255;
                    uint32_t g = (c >> 8 & 255) + (v >> 8 & 255) * (255 - a) / 255;
                    uint32_t bl = (c & 255) + (v & 255) * (255 - a) / 255;
                    v = r << 16 | g << 8 | bl;
                }
            }
            row[x * 3] = (uint8_t)(v >> 16); row[x * 3 + 1] = (uint8_t)(v >> 8); row[x * 3 + 2] = (uint8_t)v;
        }
        fwrite(row, 1, (size_t)screen_w * 3, f);
    }
    free(row);
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ pointer (hardware cursor) */
/* The pointers of QRX OS One's desktop (QRXDesktopPrefs), as the Mac drew them
 * (macos/qrx_aros_window.m): 0 the classic arrow, 1 "QRX neon", 2 "Amiga style", 3 crosshair,
 * 4 "Large white". Drawn with 4x4 samples a pixel; premultiplied ARGB for the cursor plane. */
static int in_poly(const float *xy, int n, float x, float y)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = l > 0 ? ((px - ax) * dx + (py - ay) * dy) / l : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrtf(qx * qx + qy * qy);
}
static void cursor_image(int shape)
{
    uint32_t *px = (uint32_t *)(void *)cursor_buf.map;
    unsigned stride = cursor_buf.pitch / 4;
    memset(px, 0, cursor_buf.size);
    float k = shape == 4 ? 1.5f : 1.0f;            /* 36 instead of 24 points */
    static const float arrow[] = { 2,2, 2,19, 7,15, 11,22, 14,20.5f, 10.5f,14, 16,13.5f };
    float poly[14];
    for (int i = 0; i < 14; i++) poly[i] = arrow[i] * k;
    uint32_t fill, line;
    float lw = 1.4f * k;
    if (shape == 1) { fill = 0x0D1426; line = 0x38E6C7; }          /* QRX neon */
    else if (shape == 2) { fill = 0xF24026; line = 0x000000; }     /* Amiga style */
    else { fill = 0xFFFFFF; line = 0x000000; }                      /* classic, large white */
    hot_x = hot_y = shape == 3 ? 12 : (int)(2 * k);
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 40; x++) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                    uint32_t c;
                    int hit = 0;
                    if (shape == 3) {              /* crosshair: four arms and a ring, teal, 2 wide */
                        float d = fabsf(sqrtf((fx - 12) * (fx - 12) + (fy - 12) * (fy - 12)) - 4);
                        hit = d <= 1 || seg_dist(fx, fy, 12, 1, 12, 9) <= 1 || seg_dist(fx, fy, 12, 15, 12, 23) <= 1 ||
                              seg_dist(fx, fy, 1, 12, 9, 12) <= 1 || seg_dist(fx, fy, 15, 12, 23, 12) <= 1;
                        c = 0x38E6C7;
                    } else {
                        float d = 1e9f;
                        for (int i = 0, j = 6; i < 7; j = i++) {
                            float e = seg_dist(fx, fy, poly[2 * j], poly[2 * j + 1], poly[2 * i], poly[2 * i + 1]);
                            if (e < d) d = e;
                        }
                        if (d <= lw / 2) { hit = 1; c = line; }
                        else if (in_poly(poly, 7, fx, fy)) { hit = 1; c = fill; }
                    }
                    if (!hit) continue;
                    r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255; n++;
                }
            if (!n) continue;
            /* premultiplied: the sum over 16 samples is colour x coverage */
            px[y * stride + x] = (n * 255 / 16) << 24 | (r / 16) << 16 | (g / 16) << 8 | (b / 16);
        }
}
static void cursor_apply(int set_image)
{
    if (drm < 0 || !have_cursor || released || !frame_w) return;
    if (set_image && pointer_shape >= 0) cursor_image(pointer_shape);
    int sx = (int)dst_x + (int)((int64_t)ptr_x * dst_w / frame_w) - hot_x;
    int sy = (int)dst_y + (int)((int64_t)ptr_y * dst_h / frame_h) - hot_y;
    struct drm_mode_cursor2 c = { .crtc_id = crtc_id, .x = sx, .y = sy, .hot_x = hot_x, .hot_y = hot_y };
    if (set_image) {
        c.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE;
        if (pointer_shape >= 0) { c.handle = cursor_buf.handle; c.width = 64; c.height = 64; }
    } else c.flags = DRM_MODE_CURSOR_MOVE;
    xioctl(drm, DRM_IOCTL_MODE_CURSOR2, &c);
}
void qrx_lio_pointer(int shape, int x, int y)
{
    int moved = x != ptr_x || y != ptr_y;
    ptr_x = x; ptr_y = y;
    if (shape != pointer_shape) {
        pointer_shape = shape;
        if (testing && shape >= 0) cursor_image(shape);       /* for the screen capture */
        cursor_apply(1);
    }
    else if (moved && shape >= 0) cursor_apply(0);
}
void qrx_lio_pointer_pos(int *x, int *y) { *x = ptr_x; *y = ptr_y; }

/* ------------------------------------------------------------------ console terminal */
static int tty = -1, our_vt;
static void input_grab(int on);
static void release_screen(void)
{
    if (released) return;
    released = 1;
    input_grab(0);
    if (tty >= 0) { ioctl(tty, KDSETMODE, KD_TEXT); }
    if (drm >= 0) ioctl(drm, DRM_IOCTL_DROP_MASTER, 0);
}
static void take_screen(void)
{
    if (!released) return;
    if (drm >= 0 && ioctl(drm, DRM_IOCTL_SET_MASTER, 0)) return;   /* not ours yet */
    released = 0;
    if (tty >= 0) ioctl(tty, KDSETMODE, KD_GRAPHICS);
    input_grab(1);
    if (drm >= 0) {
        set_crtc();
        if (use_plane) set_plane(&frame_buf[frame_front]);
        prev_y0 = 0; prev_y1 = frame_h;
        cursor_apply(1);
    }
}
static void switch_vt(int n)
{
    if (tty < 0 || n == our_vt) return;
    release_screen();
    ioctl(tty, VT_ACTIVATE, n);
}
static void watch_vt(void)
{
    if (tty < 0) return;
    struct vt_stat st;
    if (ioctl(tty, VT_GETSTATE, &st)) return;
    if (st.v_active == our_vt) take_screen();
    else release_screen();
}

/* ------------------------------------------------------------------ input */
#define MAX_DEV 32
typedef struct { int fd, abs; struct input_absinfo ax, ay; char name[64]; } Dev;
static Dev devs[MAX_DEV];
static int ndev, inotify_fd = -1, grabbed;
static int rel_dx, rel_dy, abs_x = -1, abs_y = -1, moved;
static double mouse_speed = 1.0, rel_fx, rel_fy;
static unsigned mods;                              /* 1 shift, 2 ctrl, 4 alt */
#define QLEN 128
static qrx_lio_event queue[QLEN];
static unsigned qhead, qtail;
static void push(qrx_lio_event e) { unsigned n = (qtail + 1) % QLEN; if (n != qhead) { queue[qtail] = e; qtail = n; } }

static int test_bit(const unsigned long *bits, unsigned n) { return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }
static void add_device(const char *name)
{
    char path[300];
    if (strncmp(name, "event", 5) || ndev >= MAX_DEV) return;
    snprintf(path, sizeof path, "%s/%s", input_dir, name);
    for (int i = 0; i < ndev; i++) if (!strcmp(devs[i].name, name)) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    Dev d = { .fd = fd };
    snprintf(d.name, sizeof d.name, "%s", name);
    unsigned long ev[1] = {0}, abs[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) >= 0) {
        if (!test_bit(ev, EV_KEY) && !test_bit(ev, EV_REL) && !test_bit(ev, EV_ABS)) { close(fd); return; }
        if (test_bit(ev, EV_ABS) && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) >= 0 && test_bit(abs, ABS_X) && test_bit(abs, ABS_Y) &&
            !ioctl(fd, EVIOCGABS(ABS_X), &d.ax) && !ioctl(fd, EVIOCGABS(ABS_Y), &d.ay) &&
            d.ax.maximum > d.ax.minimum && d.ay.maximum > d.ay.minimum) d.abs = 1;
        if (grabbed) ioctl(fd, EVIOCGRAB, 1);
    }
    devs[ndev++] = d;
}
static void scan_devices(void)
{
    DIR *dir = opendir(input_dir);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) add_device(e->d_name);
    closedir(dir);
}
static void input_grab(int on)
{
    grabbed = on && !testing;
    for (int i = 0; i < ndev; i++) ioctl(devs[i].fd, EVIOCGRAB, grabbed ? 1 : 0);
}
static void drop_device(int i)
{
    close(devs[i].fd);
    devs[i] = devs[--ndev];
}
static void flush_motion(void)
{
    if (!moved || !frame_w) return;
    moved = 0;
    if (abs_x >= 0) { ptr_x = abs_x; ptr_y = abs_y; abs_x = abs_y = -1; }
    else {
        rel_fx += rel_dx * mouse_speed; rel_fy += rel_dy * mouse_speed;
        int ix = (int)rel_fx, iy = (int)rel_fy;
        rel_fx -= ix; rel_fy -= iy;
        ptr_x += ix; ptr_y += iy;
    }
    rel_dx = rel_dy = 0;
    if (ptr_x < 0) ptr_x = 0;
    if (ptr_y < 0) ptr_y = 0;
    if (ptr_x >= (int)frame_w) ptr_x = (int)frame_w - 1;
    if (ptr_y >= (int)frame_h) ptr_y = (int)frame_h - 1;
    push((qrx_lio_event){ .type = QRX_LIO_MOVE, .x = ptr_x, .y = ptr_y });
}
static void handle(const Dev *d, const struct input_event *ev)
{
    if (ev->type == EV_SYN && ev->code == SYN_REPORT) { flush_motion(); return; }
    if (ev->type == EV_REL) {
        if (ev->code == REL_X) { rel_dx += ev->value; moved = 1; }
        else if (ev->code == REL_Y) { rel_dy += ev->value; moved = 1; }
        else if (ev->code == REL_WHEEL && ev->value)   /* REL_WHEEL is positive away from the user */
            push((qrx_lio_event){ .type = QRX_LIO_WHEEL, .value = -ev->value });
        return;
    }
    if (ev->type == EV_ABS && d->abs && frame_w) {
        if (ev->code == ABS_X) { abs_x = (int)((int64_t)(ev->value - d->ax.minimum) * (frame_w - 1) / (d->ax.maximum - d->ax.minimum)); if (abs_y < 0) abs_y = ptr_y; moved = 1; }
        else if (ev->code == ABS_Y) { abs_y = (int)((int64_t)(ev->value - d->ay.minimum) * (frame_h - 1) / (d->ay.maximum - d->ay.minimum)); if (abs_x < 0) abs_x = ptr_x; moved = 1; }
        return;
    }
    if (ev->type != EV_KEY || ev->value == 2) return;          /* the guest repeats keys itself */
    unsigned c = ev->code;
    int down = ev->value != 0;
    if (c == BTN_LEFT || c == BTN_TOUCH || c == BTN_RIGHT || c == BTN_MIDDLE) {
        flush_motion();
        push((qrx_lio_event){ .type = QRX_LIO_BUTTON, .code = c == BTN_RIGHT ? 1u : c == BTN_MIDDLE ? 2u : 0u, .value = down });
        return;
    }
    if (c >= 256) return;
    unsigned bit = c == KEY_LEFTSHIFT || c == KEY_RIGHTSHIFT ? 1u : c == KEY_LEFTCTRL || c == KEY_RIGHTCTRL ? 2u :
                   c == KEY_LEFTALT || c == KEY_RIGHTALT ? 4u : 0u;
    if (bit) mods = down ? mods | bit : mods & ~bit;
    /* Ctrl+Alt+F1..F6: another terminal (the emergency shell on F2) */
    if (down && (mods & 6) == 6 && c >= KEY_F1 && c <= KEY_F6 && tty >= 0) { switch_vt((int)(c - KEY_F1) + 1); return; }
    push((qrx_lio_event){ .type = QRX_LIO_KEY, .code = c, .value = down, .shift = (mods & 1) != 0 });
}

int qrx_lio_poll(qrx_lio_event *e, int timeout_ms)
{
    if (qhead == qtail) {
        watch_vt();
        struct pollfd p[MAX_DEV + 1];
        int n = 0;
        for (int i = 0; i < ndev; i++) p[n++] = (struct pollfd){ devs[i].fd, POLLIN, 0 };
        int ino = -1;
        if (inotify_fd >= 0) { ino = n; p[n++] = (struct pollfd){ inotify_fd, POLLIN, 0 }; }
        if (released && timeout_ms > 100) timeout_ms = 100;          /* look for our terminal again */
        int nd = ndev;
        if (poll(p, (nfds_t)n, timeout_ms) > 0) {
            for (int i = nd - 1; i >= 0; i--) {
                if (!(p[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
                struct input_event ev[64];
                ssize_t r = read(devs[i].fd, ev, sizeof ev);
                if (r < 0 && errno == EAGAIN) continue;
                if (r <= 0) {
                    /* a FIFO in tests: its writer is gone, keep it open for the next one */
                    if (testing && r == 0) continue;
                    drop_device(i);
                    continue;
                }
                for (size_t k = 0; k < (size_t)r / sizeof ev[0]; k++) handle(&devs[i], &ev[k]);
                if (testing) flush_motion();                 /* test files may omit SYN_REPORT */
            }
            if (ino >= 0 && (p[ino].revents & POLLIN)) {     /* a device plugged in */
                char buf[4096] __attribute__((aligned(8)));
                ssize_t len = read(inotify_fd, buf, sizeof buf);
                for (char *q = buf; len > 0 && q < buf + len;) {
                    struct inotify_event *ie = (struct inotify_event *)(void *)q;
                    if (ie->len) add_device(ie->name);
                    q += sizeof *ie + ie->len;
                }
            }
        }
    }
    if (qhead == qtail) return 0;
    *e = queue[qhead];
    qhead = (qhead + 1) % QLEN;
    return 1;
}

int qrx_lio_key_char(unsigned code, int shift)
{
    static const char lo[] = "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char hi[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    if (code == KEY_KPENTER) return '\n';
    if (code >= sizeof lo - 1) return 0;
    return (unsigned char)(shift ? hi[code] : lo[code]);
}

/* ------------------------------------------------------------------ open / close */
int qrx_lio_open(void)
{
    const char *t = getenv("QRX_LIO_TEST");
    const char *ms = getenv("QRX_MOUSE_SPEED");
    if (ms && atof(ms) > 0) mouse_speed = atof(ms);
    if (t) {
        testing = 1;
        input_dir = t;
        const char *sz = getenv("QRX_LIO_TEST_SIZE");
        if (sz) sscanf(sz, "%ux%u", &screen_w, &screen_h);
        cursor_buf.pitch = 64 * 4; cursor_buf.size = 64 * 64 * 4;
        cursor_buf.map = calloc(1, cursor_buf.size);
    } else {
        if (!find_output()) { fprintf(stderr, "[LIO] no connected screen on /dev/dri/card*\n"); return 0; }
        if (!buf_create(&primary[0], screen_w, screen_h) || !buf_create(&primary[1], screen_w, screen_h) || !set_crtc()) {
            fprintf(stderr, "[LIO] cannot set the screen mode: %s\n", strerror(errno));
            return 0;
        }
        plane_id = getenv("QRX_NO_PLANE") ? 0 : find_plane();
        have_cursor = buf_create(&cursor_buf, 64, 64);
        /* our virtual terminal into graphics mode: the text console stays off the screen */
        struct vt_stat st;
        tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
        if (tty >= 0 && !ioctl(tty, VT_GETSTATE, &st)) {
            char path[32];
            our_vt = st.v_active;
            snprintf(path, sizeof path, "/dev/tty%d", our_vt);
            close(tty);
            tty = open(path, O_RDWR | O_CLOEXEC);
        } else if (tty >= 0) { close(tty); tty = -1; }
        if (tty >= 0) ioctl(tty, KDSETMODE, KD_GRAPHICS);
    }
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0 && inotify_add_watch(inotify_fd, input_dir, IN_CREATE | IN_ATTRIB | IN_MOVED_TO) < 0) { close(inotify_fd); inotify_fd = -1; }
    scan_devices();
    input_grab(1);
    fprintf(stderr, "[LIO] %d input device(s)%s\n", ndev, testing ? " (test)" : "");
    return 1;
}

void qrx_lio_close(void)
{
    qrx_lio_audio_close();
    input_grab(0);
    for (int i = 0; i < ndev; i++) close(devs[i].fd);
    ndev = 0;
    if (inotify_fd >= 0) close(inotify_fd);
    inotify_fd = -1;
    if (testing) { free(cursor_buf.map); memset(&cursor_buf, 0, sizeof cursor_buf); }
    if (drm >= 0) {
        buf_free(&frame_buf[0]); buf_free(&frame_buf[1]); buf_free(&cursor_buf);
        buf_free(&primary[0]); buf_free(&primary[1]);
        close(drm);
        drm = -1;
    }
    if (tty >= 0) { ioctl(tty, KDSETMODE, KD_TEXT); close(tty); tty = -1; }
    free(frame_mem);
    frame_mem = NULL; frame_w = frame_h = 0;
}

/* ------------------------------------------------------------------ sound */
static pthread_t audio_thread;
static atomic_int audio_run;
static snd_pcm_t *pcm;
static qrx_lio_pull audio_pull;
static void *audio_ctx;
static void *audio_main(void *arg)
{
    (void)arg;
    int16_t buf[480 * 2];                              /* 10 ms */
    while (atomic_load(&audio_run)) {
        size_t got = audio_pull(audio_ctx, buf, 480);
        if (got < 480) memset(buf + got * 2, 0, (480 - got) * 4);
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, 480);       /* blocks: paced by the card */
        if (w < 0) snd_pcm_recover(pcm, (int)w, 1);
    }
    return NULL;
}
int qrx_lio_audio_open(qrx_lio_pull pull, void *ctx)
{
    const char *dev = getenv("QRX_ALSA_DEVICE");
    if (snd_pcm_open(&pcm, dev ? dev : "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "[LIO] no sound device %s\n", dev ? dev : "default");
        pcm = NULL;
        return 0;
    }
    /* 40 ms of buffer: low latency, safe against the scheduler on a busy Pi */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 40000) < 0) {
        fprintf(stderr, "[LIO] sound device does not take 48 kHz stereo 16-bit\n");
        snd_pcm_close(pcm);
        pcm = NULL;
        return 0;
    }
    audio_pull = pull; audio_ctx = ctx;
    atomic_store(&audio_run, 1);
    if (pthread_create(&audio_thread, NULL, audio_main, NULL)) { atomic_store(&audio_run, 0); snd_pcm_close(pcm); pcm = NULL; return 0; }
    return 1;
}
void qrx_lio_audio_close(void)
{
    if (!pcm) return;
    atomic_store(&audio_run, 0);
    pthread_join(audio_thread, NULL);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
}
