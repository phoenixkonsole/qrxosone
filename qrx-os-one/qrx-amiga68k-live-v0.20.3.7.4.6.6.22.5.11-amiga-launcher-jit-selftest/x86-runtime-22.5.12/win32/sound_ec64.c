/* DirectSound and waveOut on the 68100EC64 board (QRX OS One 64). The 32-bit system mixes on
 * the Mac behind the ARMithlon door (service 1, qrx86_door.c); AROS 64 has no door, so the same
 * voices are mixed here and handed to the board's sound registers (qrx_ec64_board.h, 0x500).
 * The EC64 has no FPU: gains are Q15, positions 32.32 fixed point. */
#include "qrxwin.h"
#include <string.h>

#if defined(__m68k64__)
#define BOARD(off) (0xFFFFFFFF00000000ull + (off))
#define BOARD_ID_MAGIC 0x5152584543363442ull
#define R64(off) (*(volatile uint64_t *)BOARD(off))
#define R32(off) (*(volatile uint32_t *)BOARD(off))
#define AUDIO_ADDR 0x500u
#define AUDIO_FRAMES 0x508u
#define AUDIO_GO 0x50Cu
#define AUDIO_QUEUED 0x510u
#define MIX_RATE 48000u
#define MIX_TARGET 4096u          /* about 85 ms queued on the host */
#define MIX_VOICES 128u          /* as the door: waveOut uses 124 and 125 */

typedef struct {
    int used, playing, looping;
    uint32_t mem, size, rate, chans, bits;
    int32_t gain_l, gain_r;      /* Q15 */
    uint64_t pos;                /* frames, 32.32 */
} voice;
static voice g_v[MIX_VOICES];
static int g_live, g_board = -1;
static uint32_t g_out[MIX_TARGET];

static int board_ok(void)
{
    if (g_board < 0) g_board = R64(0) == BOARD_ID_MAGIC;
    return g_board;
}
/* 10^(cb/2000) in Q15 for cb <= 0 (hundredths of a decibel): halve per 602 cB, then a table step */
static int32_t gain_q15(int32_t cb)
{
    static const uint16_t frac[16] = { 32768, 31379, 30049, 28775, 27555, 26387, 25268, 24197,
                                       23171, 22189, 21248, 20348, 19485, 18659, 17868, 17110 };
    if (cb >= 0) return 32768;
    if (cb <= -10000) return 0;
    int32_t d = -cb, halves = d / 602, rest = d % 602;
    int32_t g = frac[rest * 16 / 602];
    return halves >= 15 ? 0 : g >> halves;
}
static void voice_set(voice *v, const uint32_t *q)
{
    v->mem = q[5]; v->size = q[6]; v->rate = q[7] ? q[7] : 22050; v->chans = q[8]; v->bits = q[9];
    if (v->bits != 8 && v->bits != 16) v->bits = 16;
    if (v->chans < 1 || v->chans > 2) v->chans = 2;
    int32_t vol = (int32_t)q[10], pan = (int32_t)q[11];
    int32_t g = gain_q15(vol);
    v->gain_l = (int32_t)((int64_t)g * gain_q15(pan > 0 ? -pan : 0) >> 15);
    v->gain_r = (int32_t)((int64_t)g * gain_q15(pan < 0 ? pan : 0) >> 15);
    v->looping = q[12] != 0;
}
static inline int32_t sample(qwin *w, const voice *v, uint32_t frame, unsigned ch)
{
    uint32_t bytes = v->bits / 8u, a = v->mem + (frame * v->chans + ch) * bytes;
    uint8_t *p = x86_host_ptr(&w->cpu, a);
    if (!p) return 0;
    if (bytes == 1) return ((int32_t)p[0] - 128) << 8;
    /* the second byte may sit on the next page */
    uint8_t hi = (a & X86_PAGE_MASK) == X86_PAGE_MASK ? x86_rd8(&w->cpu, a + 1) : p[1];
    return (int16_t)(p[0] | hi << 8);
}
void qw_ec64_mix(qwin *w)
{
    if (!g_live || !board_ok()) return;
    uint32_t queued = R32(AUDIO_QUEUED);
    if (queued >= MIX_TARGET) return;
    uint32_t n = MIX_TARGET - queued, i, vi;
    static int32_t L[MIX_TARGET], R[MIX_TARGET];
    memset(L, 0, n * sizeof L[0]); memset(R, 0, n * sizeof R[0]);
    for (vi = 0; vi < MIX_VOICES; vi++) {
        voice *v = &g_v[vi];
        if (!v->used || !v->playing || !v->size) continue;
        uint32_t frames = v->size / (v->chans * (v->bits / 8u));
        if (!frames) continue;
        uint64_t step = ((uint64_t)v->rate << 32) / MIX_RATE, end = (uint64_t)frames << 32;
        for (i = 0; i < n; i++) {
            uint32_t f = (uint32_t)(v->pos >> 32);
            int32_t l = sample(w, v, f, 0), r = v->chans > 1 ? sample(w, v, f, 1) : l;
            L[i] += l * v->gain_l >> 15; R[i] += r * v->gain_r >> 15;
            v->pos += step;
            if (v->pos >= end) {
                if (v->looping) v->pos -= end;
                else { v->playing = 0; v->pos = 0; g_live--; break; }
            }
        }
    }
    for (i = 0; i < n; i++) {
        int32_t l = L[i] > 32767 ? 32767 : L[i] < -32768 ? -32768 : L[i];
        int32_t r = R[i] > 32767 ? 32767 : R[i] < -32768 ? -32768 : R[i];
        g_out[i] = (uint32_t)(uint16_t)l << 16 | (uint16_t)r;
    }
    R64(AUDIO_ADDR) = (uint64_t)(uintptr_t)g_out;
    R32(AUDIO_FRAMES) = n;
    R32(AUDIO_GO) = 1;
}
/* The door's sound service, block layout as in dx.c: [3] op, [4] voice, [5..13] arguments,
 * [14] position and [15] playing for op 4. */
int qw_ec64_sound(qwin *w, uint32_t *q)
{
    if (!board_ok() || q[0] != 1u || q[4] >= MIX_VOICES) return 0;
    voice *v = &g_v[q[4]];
    switch (q[3]) {
    case 1: {                                   /* start */
        int was = v->used && v->playing;
        voice_set(v, q);
        v->used = v->playing = 1;
        if (!was) g_live++;
        qw_ec64_mix(w);
        break;
    }
    case 2: if (v->used && v->playing) { v->playing = 0; g_live--; } break;
    case 3:                                     /* volume, pan, frequency, position + 1 */
        voice_set(v, q);
        v->used = 1;
        if (q[13]) v->pos = (uint64_t)((q[13] - 1u) / (v->chans * (v->bits / 8u))) << 32;
        break;
    case 4:
        q[14] = v->used ? (uint32_t)(v->pos >> 32) * v->chans * (v->bits / 8u) : 0;
        q[15] = (uint32_t)(v->used && v->playing);
        break;
    case 5: if (v->used && v->playing) g_live--; memset(v, 0, sizeof *v); break;
    default: break;
    }
    return 1;
}
int qw_ec64_sound_ok(void) { return board_ok(); }
int qw_ec64_playing(void) { return g_live > 0; }
#else
int qw_ec64_playing(void) { return 0; }
void qw_ec64_mix(qwin *w) { (void)w; }
int qw_ec64_sound(qwin *w, uint32_t *q) { (void)w; (void)q; return 0; }
int qw_ec64_sound_ok(void) { return 0; }
#endif
