#ifndef QRX68_HLE_H
#define QRX68_HLE_H
/* QRX68 v0.1: isolated AmigaOS-3.x 68k ABI/HUNK compatibility runtime.
 * NOT a replacement for AROS exec.library and NOT universal AmigaOS support.
 * An unknown LVO terminates the guest, never pretends success. */
#include "qrxvirt.h"
#include "qrx_memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define QRX68_EXEC_BASE UINT32_C(0x00004000)
#define QRX68_DOS_BASE UINT32_C(0x00006000)
#define QRX68_GFX_BASE UINT32_C(0x00008000)
#define QRX68_INTUITION_BASE UINT32_C(0x0000A000)
#define QRX68_FB_WIDTH 640u
#define QRX68_FB_HEIGHT 256u
#define QRX68_MAX_WINDOWS 4u
#define QRX68_MAX_EVENTS 128u
#define QRX68_MAX_PORTS 24u
#define QRX68_MAX_GADGETS 32u
#define QRX68_MAX_MENUITEMS 64u
#define QRX68_IDCMP_GADGETDOWN (1u<<5)
#define QRX68_IDCMP_GADGETUP (1u<<6)
#define QRX68_IDCMP_MENUPICK (1u<<8)
#define QRX68_IDCMP_REFRESHWINDOW (1u<<2)
#define QRX68_IDCMP_CLOSEWINDOW (1u<<9)
#define QRX68_IDCMP_MOUSEBUTTONS (1u<<3)
#define QRX68_IDCMP_MOUSEMOVE (1u<<4)
#define QRX68_IDCMP_RAWKEY (1u<<10)
#define QRX68_IDCMP_VANILLAKEY (1u<<21)
#define QRX68_TRAP_BASE UINT32_C(0xE1000000)
#define QRX68_RETURN_PC UINT32_C(0xE1FFFFFC)
#define QRX68_MAX_HANDLES 16
#define QRX68_MAX_ALLOCS 128
#define QRX68_DEFAULT_RAM (2u*1024u*1024u)
typedef struct {
    int fd;
    unsigned used;
} QRX68File;
typedef struct {
    uint32_t addr, bytes, rounded;
    unsigned in_use, vec;
} QRX68Allocation;
typedef struct {
    uint32_t window, rastport;
    int x, y, width, height;
    int cursor_x, cursor_y;
    uint32_t text_font; /* guest TextFont, classic 52-byte ABI */
    uint8_t pen;
    bool open;
    uint32_t userport, idcmp_flags, first_gadget, menu_strip;
    uint32_t active_gadget;
    uint64_t z_order;
    int active_menu;
    uint8_t *surface;
} QRX68Window;
typedef struct {
    uint32_t ptr;
    uint8_t sigbit, active, window_owned;
} QRX68Port;
typedef struct {
    uint32_t ptr, window, type, port;
    uint64_t serial;
    bool pending, owned, external;
} QRX68Message;
typedef struct {
    QRXVM vm;
    uint8_t *memory;
    QRXHostMemory memory_allocation;
    size_t memory_size;
    uint32_t heap_cursor;
    uint32_t heap_limit;
    uint32_t ioerr;
    uint64_t steps, hle_calls;
    unsigned unsupported, complete, failed;
    uint32_t failed_lib, failed_lvo;
    unsigned dos_open_count, gfx_open_count, intuition_open_count;
    QRX68Window windows[QRX68_MAX_WINDOWS];
    uint8_t *framebuffer;
    bool experimental_gui;
    unsigned gfx_calls;
    uint64_t frame_generation, event_serial;
    bool yielded;
    QRX68Message messages[QRX68_MAX_EVENTS];
    QRX68Port ports[QRX68_MAX_PORTS];
    uint32_t task_ptr, signal_pending, signal_alloc, signal_wait;
    uint32_t cli_arg_ptr;
    char program_name[128], current_directory[128];
    unsigned defer_count;
    unsigned menu_picks, gadget_picks;
    QRX68File files[QRX68_MAX_HANDLES];
    QRX68Allocation allocations[QRX68_MAX_ALLOCS];
    int shared_dirfd;
    bool shared_write;
    QRXConsoleFn output;
    void *output_ctx;
} QRX68;
/* Configure a deliberately isolated guest address space, no ROMs required. */
bool qrx68_init(QRX68 *r, size_t ram_bytes, QRXConsoleFn output, void *ctx);
void qrx68_destroy(QRX68 *r);
/* Optional direct host folder, single-level files only; read-only by default. */
bool qrx68_share(QRX68 *r, const char *directory, bool allow_write);
/* HUNK_HEADER CODE/DATA/BSS/RELOC32; bounded by existing strict loader. */
bool qrx68_load_hunk(QRX68 *r, const uint8_t *data, size_t n);
/* Return 0 on successful program exit, 1 for budget exhaustion, -1 on fault. */
int qrx68_run(QRX68 *r, uint64_t budget);
/* One 68k instruction OR one real library-vector trap. */
bool qrx68_step(QRX68 *r);
/* CPU-neutral HLE boundary: 0=execute 68k, 1=serviced LVO, -1=finished/fault.
 * The genuine Musashi interpreter reads and updates r->vm.cpu registers at
 * this boundary. Unmapped API vectors always fail closed. */
int qrx68_boundary(QRX68 *r);
/* Experimental single-screen pixel output (not a full Intuition ABI). */
bool qrx68_save_ppm(const QRX68 *r, const char *path);
/* Existing Q86F frame protocol used by the native Metal QRX desktop preview. */
bool qrx68_send_gpu_frame(const QRX68 *r, const char *socket_path);
/* GUI extensions are strictly opt-in and provide a tiny offscreen canvas. */
bool qrx68_enable_gui(QRX68 *r);
/* Enqueue a real 68k IntuiMessage for GetMsg/ReplyMsg. Returns false if
 * unsupported mask/window or backpressure; no fake delivered events. */
bool qrx68_post_event(QRX68 *r, unsigned window_index, uint32_t type,
                      uint16_t code, uint16_t qualifier, int16_t x, int16_t y);
unsigned qrx68_open_window_count(const QRX68 *r);
/* Only available in the linked full-Musashi build, not the portable subset. */
#ifdef QRX68_MUSASHI_ENABLED
int qrx68_run_musashi(QRX68 *r, uint64_t instruction_budget);
bool qrx68_musashi_begin(QRX68 *r);
int qrx68_musashi_slice(QRX68 *r, uint64_t instruction_budget);
void qrx68_musashi_end(QRX68 *r);
#endif
#endif
