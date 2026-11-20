#ifndef QRX_HOSTGPU_H
#define QRX_HOSTGPU_H
/* Batched renderer on the Mac's GPU for the host-side graphics APIs. Called from the
 * guest CPU thread. Draws go to an offscreen target, which a present scales to the
 * window; it also works with no window at all, for tests. */
#include <stdbool.h>
#include <stdint.h>
typedef struct { float x,y,z,w, u,v, r,g,b,a; } QRXHGVertex;
enum { HG_BLEND_ZERO,HG_BLEND_ONE,HG_BLEND_SRC_COLOR,HG_BLEND_INV_SRC_COLOR,HG_BLEND_SRC_ALPHA,
       HG_BLEND_INV_SRC_ALPHA,HG_BLEND_DST_ALPHA,HG_BLEND_INV_DST_ALPHA,HG_BLEND_DST_COLOR,HG_BLEND_INV_DST_COLOR };
enum { HG_CMP_NEVER,HG_CMP_LESS,HG_CMP_EQUAL,HG_CMP_LEQUAL,HG_CMP_GREATER,HG_CMP_NOTEQUAL,HG_CMP_GEQUAL,HG_CMP_ALWAYS };
enum { HG_CULL_NONE,HG_CULL_FRONT,HG_CULL_BACK };
enum { HG_ENV_MODULATE,HG_ENV_REPLACE,HG_ENV_DECAL };
typedef struct {
    bool blend; uint8_t src,dst;
    bool depth_test,depth_write; uint8_t depth_func;
    uint8_t cull; bool front_ccw;
    bool alpha_test; uint8_t alpha_func; float alpha_ref;
    uint32_t texture;         /* 0: untextured */
    uint8_t env;
    bool color_write;
    int vx,vy,vw,vh;          /* viewport in target pixels, y from the top; w=0: whole */
    bool fog; float fog_rgb[3]; float fog_start,fog_end;
} QRXHGState;
void qrx_hg_state_default(QRXHGState *s);
bool qrx_hg_open(unsigned width, unsigned height);
void qrx_hg_clear(bool color, bool depth, float r, float g, float b, float a, float z);
void qrx_hg_triangles(const QRXHGState *s, const QRXHGVertex *v, unsigned count);
/* RGBA8 tightly packed. Replaces the whole texture; id 0 allocates a new one. */
uint32_t qrx_hg_texture(uint32_t id, unsigned w, unsigned h, const void *rgba, bool linear, bool wrap_s, bool wrap_t);
void qrx_hg_texture_sub(uint32_t id, unsigned x, unsigned y, unsigned w, unsigned h, const void *rgba);
void qrx_hg_texture_sampler(uint32_t id, bool linear, bool wrap_s, bool wrap_t);
void qrx_hg_texture_free(uint32_t id);
void qrx_hg_present(void);
uint64_t qrx_hg_frames(void);
/* Window side: the layer to show frames on, and whether frames are arriving. */
void qrx_hg_set_layer(void *cametal_layer);
bool qrx_hg_active(void);
#ifndef __APPLE__
/* Linux: the finished frame as tightly packed RGBA8, row 0 on top; rgba NULL asks for the size. */
bool qrx_hg_read_frame(void *rgba, unsigned *w, unsigned *h);
#endif
#endif
