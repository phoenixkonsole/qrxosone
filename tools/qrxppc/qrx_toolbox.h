/*
 * QRXPPC - classic Mac Toolbox HLE layer (P2.2).
 *
 * This header covers the first slice: the Memory Manager.  It manages a heap
 * zone inside the guest address space, the same way the real Memory Manager
 * does: every block carries its logical size in the word before the visible
 * data, and a handle is a pointer cell (*handle is the data address).  The
 * callbacks are reachable from PPC code through a single registered import
 * (qrx_mm_dispatch), which is the trap seam an application uses.
 *
 * Scope of these slices:
 *   - nonrelocatable blocks (NewPtr/DisposePtr/SetPtrSize/GetPtrSize),
 *   - relocatable blocks (NewHandle/DisposeHandle/SetHandleSize/GetHandleSize),
 *   - FreeMem/MaxMem queries,
 *   - Resource Manager (AddResource/GetResource/LoadRelease/Detach/Rmve,
 *     SizeRsrc, GetResAttrs/SetResAttrs, Count/GetInd, ResError),
 *   - File Manager (Create/Open/Close/Read/Write/Delete/GetEOF/SetEOF,
 *     GetFileInfo/SetFileInfo) on one virtual volume,
 *   - QuickDraw (InitGraf/SetPort/pen/MoveTo/LineTo and the five rectangle
 *     operations Frame/Paint/Erase/Invert/Fill plus Rect helpers) on
 *     1-bit bitmaps in guest memory,
 *   - Window Manager (InitWindows/NewWindow/ShowWindow/HideWindow/CloseWindow/
 *     SelectWindow/MoveWindow/SizeWindow/FrontWindow/titles/refCon) with
 *     an offscreen content bitmap per window, composited on the screen
 *     bitmap,
 *   - Dialog Manager (Alert/NoteAlert/CautionAlert/StopAlert/ModalDialog
 *     dismiss immediately with item 1 - there is no event loop yet),
 *   - Menu Manager (NewMenu/AppendMenu/CountMItems/Get+SetMenuItemText/
 *     EnableItem/DisableItem/CheckItem/menubar list).
 * Not included: zones, purging, locking, fragmentation moves, resource files
 * on disk (resources are seeded with AddResource), aliases, volumes beyond
 * the one virtual volume, colour QuickDraw, regions/polygons/pictures, window
 * scrolling/tearing, real font resources (text uses one synthetic 8x8 system
 * font), sleeping in WaitNextEvent, and modal event loops; each reports an
 * explicit error, never a hang.
 */
#ifndef QRX_TOOLBOX_H
#define QRX_TOOLBOX_H

#include "qrx_mix.h"

/* A-line trap numbers (Inside Macintosh, linked from the trap dispatch
 * tables; see cxmon mon_atraps.h). */
#define QRX_TB_FREE_MEM 0xA01Cu
#define QRX_TB_DISPOSE_PTR 0xA01Fu
#define QRX_TB_SET_PTR_SIZE 0xA020u
#define QRX_TB_GET_PTR_SIZE 0xA021u
#define QRX_TB_DISPOSE_HANDLE 0xA023u
#define QRX_TB_SET_HANDLE_SIZE 0xA024u
#define QRX_TB_GET_HANDLE_SIZE 0xA025u
#define QRX_TB_MAX_MEM 0xA11Du
#define QRX_TB_NEW_PTR 0xA11Eu
#define QRX_TB_NEW_HANDLE 0xA122u

/* File Manager traps (mon_atraps.h; procedure names are the MFS-era ones). */
#define QRX_TB_FM_OPEN 0xA000u
#define QRX_TB_FM_CLOSE 0xA001u
#define QRX_TB_FM_READ 0xA002u
#define QRX_TB_FM_WRITE 0xA003u
#define QRX_TB_FM_GET_VOL_INFO 0xA007u
#define QRX_TB_FM_CREATE 0xA008u
#define QRX_TB_FM_DELETE 0xA009u
#define QRX_TB_FM_GET_FILE_INFO 0xA00Cu
#define QRX_TB_FM_SET_FILE_INFO 0xA00Du
#define QRX_TB_FM_GET_EOF 0xA011u
#define QRX_TB_FM_SET_EOF 0xA012u

/* Resource Manager traps (mon_atraps.h).  The ResourceDispatch selector for
 * SetResourceSize is $7003. */
#define QRX_TB_RM_DETACH 0xA992u
#define QRX_TB_RM_COUNT 0xA99Cu
#define QRX_TB_RM_GET_IND 0xA99Du
#define QRX_TB_RM_COUNT_TYPES 0xA99Eu
#define QRX_TB_RM_GET_IND_TYPE 0xA99Fu
#define QRX_TB_RM_GET 0xA9A0u
#define QRX_TB_RM_GET_NAMED 0xA9A1u
#define QRX_TB_RM_LOAD 0xA9A2u
#define QRX_TB_RM_RELEASE 0xA9A3u
#define QRX_TB_RM_SIZE 0xA9A5u
#define QRX_TB_RM_GET_ATTRS 0xA9A6u
#define QRX_TB_RM_SET_ATTRS 0xA9A7u
#define QRX_TB_RM_ADD 0xA9ABu
#define QRX_TB_RM_REMOVE 0xA9ADu
#define QRX_TB_RM_ERROR 0xA9AFu
#define QRX_TB_RM_DISPATCH 0xA822u
#define QRX_TB_RM_SET_SIZE_SELEN 0x7003u

/* QuickDraw traps (mon_atraps.h).  Ports are addressed by their BitMap:
 * basePtr @0 (uint32), rowBytes @4 (int16), bounds @6 (top,left,bottom,
 * right as int16); pixels are 1-bit, MSB first, rows counted from the top
 * of bounds.  Rects are four int16 (top,left,bottom,right) and Points two
 * int16 (h,v).  Patterns are 8 bytes (8x8, MSB first). */
#define QRX_TB_QD_INIT_PORT 0xA86Du
#define QRX_TB_QD_INIT_GRAF 0xA86Eu
#define QRX_TB_QD_OPEN_PORT 0xA86Fu
#define QRX_TB_QD_SET_PORT 0xA873u
#define QRX_TB_QD_GET_PORT 0xA874u
#define QRX_TB_QD_PORT_SIZE 0xA876u
#define QRX_TB_QD_MOVE_PORT_TO 0xA877u
#define QRX_TB_QD_SET_CLIP 0xA879u
#define QRX_TB_QD_BACK_PAT 0xA87Cu
#define QRX_TB_QD_CLOSE_PORT 0xA87Du
#define QRX_TB_QD_SET_PT 0xA880u
#define QRX_TB_QD_MOVE_TO 0xA893u
#define QRX_TB_QD_LINE_TO 0xA891u
#define QRX_TB_QD_LINE 0xA892u
#define QRX_TB_QD_HIDE_PEN 0xA896u
#define QRX_TB_QD_SHOW_PEN 0xA897u
#define QRX_TB_QD_GET_PEN 0xA89Au
#define QRX_TB_QD_PEN_SIZE 0xA89Bu
#define QRX_TB_QD_PEN_MODE 0xA89Cu
#define QRX_TB_QD_PEN_PAT 0xA89Du
#define QRX_TB_QD_PEN_NORMAL 0xA89Eu
#define QRX_TB_QD_FRAME_RECT 0xA8A1u
#define QRX_TB_QD_PAINT_RECT 0xA8A2u
#define QRX_TB_QD_ERASE_RECT 0xA8A3u
#define QRX_TB_QD_INVERT_RECT 0xA8A4u
#define QRX_TB_QD_FILL_RECT 0xA8A5u
#define QRX_TB_QD_EQUAL_RECT 0xA8A6u
#define QRX_TB_QD_SET_RECT 0xA8A7u
#define QRX_TB_QD_OFFSET_RECT 0xA8A8u
#define QRX_TB_QD_INSET_RECT 0xA8A9u
#define QRX_TB_QD_PT_IN_RECT 0xA8ADu

/* Window Manager traps (mon_atraps.h).  A WindowPtr is the guest address
 * of a window record (232 bytes, Inside Macintosh WindowRecord layout):
 * windowKind @0, visible @2, refCon @4, frame (Rect) @24, nextWindow @56,
 * titleLength @64, port (GrafPort, starting with its BitMap) @82.  The
 * title text itself is kept host-side (titleLength is mirrored into the
 * record).  Window contents live in an offscreen 1-bit bitmap allocated
 * from the Memory Manager zone; ShowWindow composites it onto the screen
 * bitmap at the frame origin. */
#define QRX_TB_WM_GET_WMGR_PORT 0xA910u
#define QRX_TB_WM_INIT_WINDOWS 0xA912u
#define QRX_TB_WM_NEW_WINDOW 0xA913u
#define QRX_TB_WM_DISPOSE_WINDOW 0xA914u
#define QRX_TB_WM_SHOW_WINDOW 0xA915u
#define QRX_TB_WM_HIDE_WINDOW 0xA916u
#define QRX_TB_WM_GET_REFCON 0xA917u
#define QRX_TB_WM_SET_REFCON 0xA918u
#define QRX_TB_WM_GET_TITLE 0xA919u
#define QRX_TB_WM_SET_TITLE 0xA91Au
#define QRX_TB_WM_MOVE_WINDOW 0xA91Bu
#define QRX_TB_WM_HILITE_WINDOW 0xA91Cu
#define QRX_TB_WM_SIZE_WINDOW 0xA91Du
#define QRX_TB_WM_SELECT_WINDOW 0xA91Fu
#define QRX_TB_WM_BRING_TO_FRONT 0xA920u
#define QRX_TB_WM_SEND_BEHIND 0xA921u
#define QRX_TB_WM_BEGIN_UPDATE 0xA922u
#define QRX_TB_WM_END_UPDATE 0xA923u
#define QRX_TB_WM_FRONT_WINDOW 0xA924u
#define QRX_TB_WM_INVAL_RECT 0xA928u
#define QRX_TB_WM_FIND_WINDOW 0xA92Cu
#define QRX_TB_WM_CLOSE_WINDOW 0xA92Du

/* Menu Manager traps (mon_atraps.h).  A MenuHandle is a Memory Manager
 * handle; the MenuInfo record is menuID @0 (int16), menuWidth @2,
 * menuHeight @4, menuProc @6, enableFlags @10 (bit item-1 = enabled),
 * menuData @14 (handle to the concatenated Pascal items).  Item text is
 * kept host-side per menu. */
#define QRX_TB_MENU_INIT 0xA930u
#define QRX_TB_MENU_NEW 0xA931u
#define QRX_TB_MENU_DISPOSE 0xA932u
#define QRX_TB_MENU_APPEND 0xA933u
#define QRX_TB_MENU_CLEAR_BAR 0xA934u
#define QRX_TB_MENU_INSERT 0xA935u
#define QRX_TB_MENU_DELETE 0xA936u
#define QRX_TB_MENU_DRAW_BAR 0xA937u
#define QRX_TB_MENU_ENABLE_ITEM 0xA939u
#define QRX_TB_MENU_DISABLE_ITEM 0xA93Au
#define QRX_TB_MENU_GET_BAR 0xA93Bu
#define QRX_TB_MENU_SET_BAR 0xA93Cu
#define QRX_TB_MENU_CHECK_ITEM 0xA945u
#define QRX_TB_MENU_GET_ITEM_TEXT 0xA946u
#define QRX_TB_MENU_SET_ITEM_TEXT 0xA947u
#define QRX_TB_MENU_COUNT_ITEMS 0xA950u
#define QRX_TB_MENU_DELETE_ITEM 0xA952u

/* Dialog Manager traps (mon_atraps.h).  With no event loop, Alert and
 * friends and ModalDialog dismiss at once with item 1. */
#define QRX_TB_DM_INIT 0xA97Bu
#define QRX_TB_DM_ALERT 0xA985u
#define QRX_TB_DM_STOP_ALERT 0xA986u
#define QRX_TB_DM_NOTE_ALERT 0xA987u
#define QRX_TB_DM_CAUTION_ALERT 0xA988u
#define QRX_TB_DM_MODAL_DIALOG 0xA991u

/* Classic OSErr codes used by the slices (Inside Macintosh: Files /
 * More Macintosh Toolbox). */
#define QRX_TB_NO_ERR 0
#define QRX_TB_VOL_OFF_LIN_ERR (-32)
#define QRX_TB_DSK_FUL_ERR (-34)
#define QRX_TB_FNF_ERR (-43)
#define QRX_TB_DUP_FN (-48)
#define QRX_TB_PARAM_ERR (-50)
#define QRX_TB_FN_OPN_ERR (-38)
#define QRX_TB_RF_NUM_ERR (-51)
#define QRX_TB_RES_NOT_FOUND (-192)
#define QRX_TB_W_WRONG_KIND (-101)  /* bad window pointer or closed window */
#define QRX_TB_W_NO_CONTENT (-102)  /* window without a content bitmap */

#define QRX_MM_MAX_BLOCKS 64u     /* tracked allocations per zone */
#define QRX_MM_ALIGN 8u           /* block granularity */
#define QRX_MM_UNKNOWN 0xFFFFFFFFu /* marker for size queries on bad blocks */

typedef enum {
    QRX_MM_OK = 0,      /* noErr */
    QRX_MM_ERR_ARG,     /* NULL arguments */
    QRX_MM_ERR_FULL,    /* no room in the zone (memFullErr) */
    QRX_MM_ERR_INVALID, /* pointer or handle does not name a block */
    QRX_MM_ERR_SIZE     /* the requested resize cannot be served (see below) */
} qrx_mm_status_t;

/* One tracked block.  The header word (logical size) sits at base, so the
 * visible data address is base + 4 and the size lives at data - 4, exactly as
 * the real Memory Manager lays a block out. */
typedef struct {
    uint32_t base;      /* header address in guest memory */
    uint32_t total;     /* physical size including the header word */
    uint32_t size;      /* logical size in bytes */
    int kind;           /* 0 free, 1 pointer, 2 handle data, 3 handle cell */
    uint32_t cell;      /* handle data: guest address of the master pointer */
} qrx_mm_block_t;

typedef struct {
    qrx_ppc_t *ppc;
    uint32_t base;      /* heap zone start in guest memory */
    uint32_t size;      /* heap zone length */
    uint32_t bump;      /* end of the never-allocated tail */
    uint32_t last_err;  /* sticky memory error of the last call */
    qrx_mm_block_t blocks[QRX_MM_MAX_BLOCKS];
    unsigned nblocks;
} qrx_mm_t;

/* Initialise a zone.  base must be 8-byte aligned and bytes may be zero (the
 * zone is then empty). */
qrx_mm_status_t qrx_mm_init(qrx_mm_t *mm, qrx_ppc_t *ppc, uint32_t base,
                            uint32_t size);

/* NewPtr: visible pointer on success, 0 when nothing fits.  The data is not
 * zeroed unless clear set.  *out receives the guest address. */
qrx_mm_status_t qrx_mm_new_ptr(qrx_mm_t *mm, uint32_t bytes, int clear,
                               uint32_t *out);

/* DisposePtr: releases a pointer block; a handle's data pointer, an already
 * disposed block or a foreign address is QRX_MM_ERR_INVALID. */
qrx_mm_status_t qrx_mm_dispose_ptr(qrx_mm_t *mm, uint32_t ptr);

/* NewHandle: *out receives the handle (address of the master pointer); the
 * block itself holds the size word at data - 4, as NewPtr blocks do. */
qrx_mm_status_t qrx_mm_new_handle(qrx_mm_t *mm, uint32_t bytes, int clear,
                                  uint32_t *out);

/* DisposeHandle: releases data block and master pointer. */
qrx_mm_status_t qrx_mm_dispose_handle(qrx_mm_t *mm, uint32_t handle);

/* SetHandleSize: shrinking edits the size word in place; growing moves the
 * block, copies the common part, updates the master pointer and frees the old
 * block.  When no room exists the old block is left untouched and the call
 * fails with QRX_MM_ERR_FULL. */
qrx_mm_status_t qrx_mm_set_handle_size(qrx_mm_t *mm, uint32_t handle,
                                       uint32_t bytes);

/* SetPtrSize: pointer blocks are nonrelocatable, so they only shrink in
 * place; a growth request is QRX_MM_ERR_SIZE.  Use a handle to grow. */
qrx_mm_status_t qrx_mm_set_ptr_size(qrx_mm_t *mm, uint32_t ptr,
                                    uint32_t bytes);

qrx_mm_status_t qrx_mm_get_ptr_size(qrx_mm_t *mm, uint32_t ptr,
                                    uint32_t *size);
qrx_mm_status_t qrx_mm_get_handle_size(qrx_mm_t *mm, uint32_t handle,
                                       uint32_t *size);

/* FreeMem/MaxMem queries: free bytes, and largest single allocatable block. */
uint32_t qrx_mm_free_bytes(qrx_mm_t *mm);
uint32_t qrx_mm_max_block(qrx_mm_t *mm);

/* Import adapter (qrx_mix_import_fn): r3 is the trap number, r4/r5 its
 * arguments, the return value lands in r3.  NewPtr/NewHandle return the
 * guest address (0 when nothing fits), Get*Size the size (QRX_MM_UNKNOWN on a
 * bad block), Dispose and Set return the memory error (0 on success) and an unknown
 * trap number returns QRX_MM_UNKNOWN. */
uint32_t qrx_mm_dispatch(void *ctx, const qrx_mix_args_t *args,
                         qrx_mix_result_t *extra);

/* ---- Resource Manager ------------------------------------------------
 * A flat table of registered resources.  Entries reference Memory Manager
 * handles; the data the original Resource Manager would read from a
 * resource file is seeded by the application with AddResource (as it would
 * be after loading a MacBinary resource fork), so there is no disk.  A
 * resource is always resident: LoadResource/ReleaseResource toggle the
 * loaded flag without touching the handle, and GetResource returns the
 * handle in either state - the disk re-loading path is the documented
 * simplification.  ResError() reports the last outcome (-192 resNotFound
 * for an unknown handle or type/id pair). */
#define QRX_RM_MAX_ENTRIES 32u

typedef struct {
    uint32_t type;      /* OSType */
    int16_t id;
    uint8_t attrs;      /* resPurgeable (0x80) etc., as set by SetResAttrs */
    uint8_t used;
    uint8_t loaded;
    uint32_t handle;    /* Memory Manager handle */
    char name[32];      /* C string, empty means unnamed */
} qrx_rm_entry_t;

typedef struct {
    qrx_mm_t *mm;
    qrx_rm_entry_t entries[QRX_RM_MAX_ENTRIES];
    int32_t last_err;   /* ResError value */
} qrx_rm_t;

void qrx_rm_init(qrx_rm_t *rm, qrx_mm_t *mm);

/* AddResource / GetResource.  AddResource takes an existing MM handle;
 * GetResource returns the handle or 0 (ResError resNotFound). */
int32_t qrx_rm_add_resource(qrx_rm_t *rm, uint32_t type, int16_t id,
                            const char *name, uint32_t handle);
uint32_t qrx_rm_get_resource(qrx_rm_t *rm, uint32_t type, int16_t id);
uint32_t qrx_rm_get_named_resource(qrx_rm_t *rm, uint32_t type,
                                   const char *name);

/* LoadResource / ReleaseResource / DetachResource / RmveResource /
 * SizeRsrc / GetResAttrs / SetResAttrs / ResError. */
int32_t qrx_rm_load_resource(qrx_rm_t *rm, uint32_t handle);
int32_t qrx_rm_release_resource(qrx_rm_t *rm, uint32_t handle);
int32_t qrx_rm_detach_resource(qrx_rm_t *rm, uint32_t handle);
int32_t qrx_rm_remove_resource(qrx_rm_t *rm, uint32_t handle);
int32_t qrx_rm_size_resource(qrx_rm_t *rm, uint32_t handle);
int32_t qrx_rm_get_attrs(qrx_rm_t *rm, uint32_t handle);
int32_t qrx_rm_set_attrs(qrx_rm_t *rm, uint32_t handle, int32_t attrs);
int32_t qrx_rm_res_error(const qrx_rm_t *rm);

/* CountResources(type) / GetIndResource(type, index) / CountTypes /
 * GetIndType(index). */
uint32_t qrx_rm_count_resources(const qrx_rm_t *rm, uint32_t type);
uint32_t qrx_rm_get_ind_resource(const qrx_rm_t *rm, uint32_t type,
                                 uint32_t index);
uint32_t qrx_rm_count_types(const qrx_rm_t *rm);
uint32_t qrx_rm_get_ind_type(const qrx_rm_t *rm, uint32_t index);

/* SetResourceSize: grows/shrinks the resource's handle via the MM layer. */
int32_t qrx_rm_set_resource_size(qrx_rm_t *rm, uint32_t handle,
                                 uint32_t size);

/* ---- File Manager ------------------------------------------------------
 * One virtual volume ("QRXPPC", vrefnum 1) with up to 32 files; file
 * contents live in host buffers and are copied through the caller's guest
 * buffer on Read/Write - only the param block results are guest-visible.
 * A file has a 16-byte Finder info, an open count with a per-access-path
 * file mark, and a single data fork.  Names are Pascal strings (1..31
 * bytes) and compare case-insensitively, like HFS.
 *
 * Two param block layouts are honoured, exactly as Inside Macintosh:
 * Files lays them out:
 *   FileParams (Create/Delete/GetFileInfo/SetFileInfo):
 *     ioResult @0 (int16), ioNamePtr @2 (uint32), ioVRefNum @6 (int16),
 *     ioFndrInfo @8 (16 bytes).
 *   IOParam (Open/Close/Read/Write/GetEOF/SetEOF):
 *     ioCmd @0 (int16, ignored - the trap selects the operation),
 *     ioResult @2 (int16), ioNamePtr @4 (uint32), ioVRefNum @8 (int16),
 *     ioRefNum @10 (int16), ioBuffer @12 (uint32), ioReqCount @16 (int32),
 *     ioActCount @20 (int32; SetEOF input+output), ioPosMode @24 (int16),
 *     ioPosOffset @26 (int32).
 * Every call writes the result code into ioResult (and returns it), like
 * the real traps do. */
#define QRX_FM_MAX_FILES 32u
#define QRX_FM_MAX_OPENS 16u
#define QRX_FM_MAX_NAME 31u
#define QRX_FM_MAX_FILE 32768u

typedef struct {
    uint8_t used;
    char name[QRX_FM_MAX_NAME + 1u];  /* C string */
    uint8_t fndr[16];
    uint8_t *data;                    /* host buffer, NULL when empty */
    uint32_t size;
    uint32_t cap;
} qrx_fm_file_t;

typedef struct {
    uint8_t used;
    uint8_t file;         /* index into files[] */
    uint32_t mark;        /* file mark */
} qrx_fm_open_t;

typedef struct {
    qrx_ppc_t *ppc;
    qrx_fm_file_t files[QRX_FM_MAX_FILES];
    qrx_fm_open_t opens[QRX_FM_MAX_OPENS];
} qrx_fm_t;

void qrx_fm_init(qrx_fm_t *fm, qrx_ppc_t *ppc);

/* The OSErr of the operation; helpers for the dispatch below. */
int32_t qrx_fm_create_at(qrx_fm_t *fm, const char *name,
                         const uint8_t *fndr);
int32_t qrx_fm_open(qrx_fm_t *fm, const char *name, int16_t *refnum);
int32_t qrx_fm_close(qrx_fm_t *fm, int16_t refnum);
int32_t qrx_fm_delete(qrx_fm_t *fm, const char *name);
int32_t qrx_fm_get_file_info(const qrx_fm_t *fm, const char *name,
                             uint8_t *fndr);
int32_t qrx_fm_set_file_info(qrx_fm_t *fm, const char *name,
                             const uint8_t *fndr);

/* ---- QuickDraw ---------------------------------------------------------
 * 1-bit bitmaps in guest memory (BitMap layout above).  The current port
 * is addressed by its BitMap; a window's port is record + 82.  All drawing
 * clips to the port bounds and to the caller's memory; empty or bad
 * rectangles/ports report paramErr instead of faulting.  Transfer modes
 * honoured: srcCopy (0) and patCopy (1); anything else is paramErr.
 * Patterns are 8x8; the drawing operations are:
 *   PaintRect - fill with the pen pattern,
 *   EraseRect - fill with the background pattern (white by default),
 *   FillRect  - fill with the pattern at r5,
 *   InvertRect - xor the pixels,
 *   FrameRect - outline with the pen pattern, thickness from PenSize.
 * MoveTo/LineTo/Move/Line keep the pen location in port coordinates. */
#define QRX_QD_MAX_PAT 8u

typedef struct {
    qrx_ppc_t *ppc;
    uint32_t port;          /* current port (BitMap address), 0 = none */
    int16_t pen_h;          /* pen location, port coordinates */
    int16_t pen_v;
    int16_t pen_w;          /* pen size (width, height) */
    int16_t pen_hgt;
    int16_t pen_mode;       /* 0 = srcCopy, 1 = patCopy */
    uint8_t pen_pat[QRX_QD_MAX_PAT];
    uint8_t back_pat[QRX_QD_MAX_PAT];
    int pen_vis;            /* 0 hidden, 1 shown */
} qrx_qd_t;

void qrx_qd_init(qrx_qd_t *qd, qrx_ppc_t *ppc);
void qrx_qd_set_screen(qrx_qd_t *qd, uint32_t bitmap_addr);
uint32_t qrx_qd_get_port(const qrx_qd_t *qd);
/* Direct-draw helpers (also used by the dispatch): return 0 on success. */
int32_t qrx_qd_move_to(qrx_qd_t *qd, int16_t h, int16_t v);
int32_t qrx_qd_line_to(qrx_qd_t *qd, int16_t h, int16_t v);
int32_t qrx_qd_fill_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                         int16_t bottom, int16_t right,
                         const uint8_t *pat);
int32_t qrx_qd_frame_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                          int16_t bottom, int16_t right);
int32_t qrx_qd_invert_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                           int16_t bottom, int16_t right);
int32_t qrx_qd_erase_rect(qrx_qd_t *qd, int16_t top, int16_t left,
                          int16_t bottom, int16_t right);

/* ---- Window Manager -----------------------------------------------------
 * Up to 8 windows.  Each window owns an offscreen 1-bit content bitmap
 * (Memory Manager block) sized to its frame; drawing into the window's
 * port (record + 82) clips to that bitmap.  ShowWindow composites the
 * content onto the screen bitmap; HideWindow redraws the desktop (screen
 * cleared, then visible windows front-to-back).  The window list order
 * is front-to-back; FrontWindow returns the first visible window. */
#define QRX_WM_MAX_WINDOWS 8u
#define QRX_WM_RECORD_SIZE 232u
#define QRX_WM_KIND 4         /* regular window */

typedef struct {
    uint8_t used;
    uint8_t visible;
    uint8_t own_record;       /* record allocated from the MM zone */
    int16_t top, left, bottom, right;   /* frame (content rect) */
    uint32_t record;          /* WindowPtr (guest address) */
    uint32_t bitmap;          /* content bitmap data (guest) */
    uint32_t bitmap_bytes;
    uint32_t refcon;
    char title[64];           /* C string, mirrored length at record + 64 */
    uint16_t proc;            /* window definition id, kept for parity */
    int next;                 /* next slot in the front-to-back list */
} qrx_wm_window_t;

typedef struct {
    qrx_ppc_t *ppc;
    qrx_mm_t *mm;
    uint32_t screen;          /* screen BitMap address, 0 = none */
    qrx_wm_window_t wins[QRX_WM_MAX_WINDOWS];
    int front;                /* slot of the front window, -1 = none */
} qrx_wm_t;

void qrx_wm_init(qrx_wm_t *wm, qrx_ppc_t *ppc, qrx_mm_t *mm,
                 uint32_t screen_bitmap);
uint32_t qrx_wm_front_window(const qrx_wm_t *wm);
int32_t qrx_wm_close_window(qrx_wm_t *wm, uint32_t win);
int32_t qrx_wm_show_window(qrx_wm_t *wm, uint32_t win);
int32_t qrx_wm_hide_window(qrx_wm_t *wm, uint32_t win);

/* ---- Menu Manager -------------------------------------------------------
 * Up to 8 menus, each a Memory Manager handle.  Items are Pascal strings
 * kept host-side; GetMenuItemText copies one out, SetMenuItemText replaces
 * it.  The menubar is an ordered list of MenuHandles. */
#define QRX_MENU_MAX_MENUS 8u
#define QRX_MENU_MAX_ITEMS 16u
#define QRX_MENU_MAX_TEXT 32u

typedef struct {
    uint8_t used;
    uint8_t enabled[QRX_MENU_MAX_ITEMS];
    uint8_t checked[QRX_MENU_MAX_ITEMS];
    char text[QRX_MENU_MAX_ITEMS][QRX_MENU_MAX_TEXT + 1u]; /* C strings */
    uint32_t handle;          /* MenuHandle (MM handle, guest) */
    int16_t id;
    uint16_t nitems;
} qrx_menu_t;

typedef struct {
    qrx_ppc_t *ppc;
    qrx_mm_t *mm;
    qrx_menu_t menus[QRX_MENU_MAX_MENUS];
    uint32_t bar[QRX_MENU_MAX_MENUS];   /* MenuHandles, front to back */
    uint16_t nbar;
} qrx_menu_mgr_t;

void qrx_menu_init(qrx_menu_mgr_t *mmgr, qrx_ppc_t *ppc, qrx_mm_t *mm);
uint32_t qrx_menu_new(qrx_menu_mgr_t *mmgr, int16_t id, const char *title);
int32_t qrx_menu_append(qrx_menu_mgr_t *mmgr, uint32_t menu, const char *text);
uint32_t qrx_menu_count_items(const qrx_menu_mgr_t *mmgr, uint32_t menu);

/* ---- Dialog Manager -----------------------------------------------------
 * No event loop: Alert/Note/Caution/Stop and ModalDialog dismiss at once
 * and return item 1.  Kept as a struct so the call sites have a home and
 * the dispatch has a layer to hang off. */
typedef struct {
    qrx_ppc_t *ppc;
    int16_t last_item;        /* always 1 until an event loop exists */
    uint16_t last_alert_id;   /* alert resource id of the last alert */
} qrx_dm_t;

void qrx_dm_init(qrx_dm_t *dm, qrx_ppc_t *ppc);

/* ---- Text/Font Manager ------------------------------------------------
 * One synthetic system font, family 0 ("Chicago"), rendered from the
 * public-domain 8x8 glyphs in qrx_font8x8.h.  The glyph cell is 8x8 pixels
 * and TextSize selects an integer scale of 1..4 (12 -> 1, 18 -> 2, 27 -> 3,
 * 36 -> 4), so metrics and pixels always agree:
 *   advance = 8*scale, ascent = 7*scale, descent = 1*scale, widMax = 8*scale,
 *   leading = 0.  SpaceExtra is a Fixed (16.16) added to every advance.
 * TextFace is stored but not rendered.  Text modes: 0 (srcCopy, the complete
 * glyph cell is written), 1 (srcOr, only set bits), 2 (srcXor), 3 (srcBic);
 * anything else is paramErr.  Drawing goes through the QuickDraw layer, so
 * the current port, its clipping and the guest-memory checks apply. */
#define QRX_TB_TEXT_DRAW_CHAR 0xA883u
#define QRX_TB_TEXT_DRAW_STRING 0xA884u
#define QRX_TB_TEXT_DRAW_TEXT 0xA885u
#define QRX_TB_TEXT_WIDTH 0xA886u
#define QRX_TB_TEXT_FONT 0xA887u
#define QRX_TB_TEXT_FACE 0xA888u
#define QRX_TB_TEXT_MODE 0xA889u
#define QRX_TB_TEXT_SIZE 0xA88Au
#define QRX_TB_TEXT_FONT_INFO 0xA88Bu
#define QRX_TB_TEXT_STRING_WIDTH 0xA88Cu
#define QRX_TB_TEXT_CHAR_WIDTH 0xA88Du
#define QRX_TB_TEXT_SPACE_EXTRA 0xA88Eu
#define QRX_TB_TEXT_INIT_FONTS 0xA8FEu
#define QRX_TB_TEXT_GET_FONT_NAME 0xA8FFu
#define QRX_TB_TEXT_GET_FNUM 0xA900u

#define QRX_TEXT_SYSTEM_FONT 0
#define QRX_TEXT_SYSTEM_SIZE 12
#define QRX_TEXT_MAX_SIZE 96

typedef struct {
    qrx_ppc_t *ppc;
    qrx_qd_t *qd;
    int16_t font;        /* family id; only QRX_TEXT_SYSTEM_FONT exists */
    int16_t size;        /* point size, 1..QRX_TEXT_MAX_SIZE */
    int16_t face;        /* bold/italic/... stored, not rendered */
    int16_t mode;        /* txMode: 0 srcCopy, 1 srcOr, 2 srcXor, 3 srcBic */
    int32_t space_extra; /* Fixed 16.16 per character */
} qrx_font_t;

void qrx_font_init(qrx_font_t *ft, qrx_ppc_t *ppc, qrx_qd_t *qd);
int16_t qrx_text_advance(const qrx_font_t *ft);
int16_t qrx_text_ascent(const qrx_font_t *ft);
int16_t qrx_text_descent(const qrx_font_t *ft);
/* Advance including SpaceExtra, in pixels. */
int16_t qrx_text_char_advance(const qrx_font_t *ft);
/* Glyph rows for a character (8 bytes, bit 0 = leftmost), or NULL. */
const uint8_t *qrx_text_glyph(uint32_t ch);
void qrx_text_draw(qrx_font_t *ft, const uint8_t *bytes, uint32_t count);
uint32_t qrx_text_dispatch(qrx_font_t *ft, const qrx_mix_args_t *args);

/* ---- Event Manager -----------------------------------------------------
 * A synthetic event source: the app posts events, the harness sets mouse,
 * keyboard and modifier state, and TickCount reads a host-supplied tick
 * source (or a value the harness advances).  GetNextEvent/EventAvail honour
 * the event mask; a mask of 0 is treated as everyEventMask.  When no event
 * matches, a nullEvent record is returned (boolean 0) without touching the
 * queue.  WaitNextEvent never sleeps: it behaves like GetNextEvent and
 * ignores the sleep and mouseRgn arguments.  The queue holds
 * QRX_EV_QUEUE records; PostEvent is dropped when it is full. */
#define QRX_TB_EV_WAIT_NEXT 0xA860u
#define QRX_TB_EV_GET_NEXT 0xA970u
#define QRX_TB_EV_AVAIL 0xA971u
#define QRX_TB_EV_GET_MOUSE 0xA972u
#define QRX_TB_EV_STILL_DOWN 0xA973u
#define QRX_TB_EV_BUTTON 0xA974u
/* OS traps (Inside Macintosh: Operating System Utilities). */
#define QRX_TB_GET_ZONE 0xA11Au
#define QRX_TB_SET_ZONE 0xA11Bu
#define QRX_TB_GESTALT 0xA1ADu

#define QRX_TB_EV_TICK_COUNT 0xA975u
#define QRX_TB_EV_GET_KEYS 0xA976u
#define QRX_TB_EV_WAIT_MOUSE_UP 0xA977u
#define QRX_TB_EV_POST 0xA02Fu
#define QRX_TB_EV_FLUSH 0xA032u
#define QRX_TB_EV_SYSTEM_TASK 0xA9B4u

/* Event codes and masks (Inside Macintosh: Toolbox Essentials). */
#define QRX_EV_NULL 0
#define QRX_EV_MOUSE_DOWN 1
#define QRX_EV_MOUSE_UP 2
#define QRX_EV_KEY_DOWN 3
#define QRX_EV_KEY_UP 4
#define QRX_EV_AUTO_KEY 5
#define QRX_EV_UPDATE 6
#define QRX_EV_DISK 7
#define QRX_EV_ACTIVATE 8
#define QRX_EV_MASK_MOUSE_DOWN 0x0002
#define QRX_EV_MASK_MOUSE_UP 0x0004
#define QRX_EV_MASK_KEY_DOWN 0x0008
#define QRX_EV_MASK_KEY_UP 0x0010
#define QRX_EV_MASK_AUTO_KEY 0x0020
#define QRX_EV_MASK_UPDATE 0x0040
#define QRX_EV_MASK_DISK 0x0080
#define QRX_EV_MASK_ACTIVATE 0x0100
#define QRX_EV_MASK_EVERY 0xFFFF

/* Modifier bits of the EventRecord. */
#define QRX_EV_CMD_KEY 0x0100
#define QRX_EV_SHIFT_KEY 0x0200
#define QRX_EV_ALPHA_LOCK 0x0400
#define QRX_EV_OPTION_KEY 0x0800
#define QRX_EV_CONTROL_KEY 0x1000

#define QRX_EV_QUEUE 8u
#define QRX_EV_RECORD_SIZE 16u

typedef struct {
    int16_t what;
    uint16_t modifiers;
    int32_t message;
    int32_t when;
    int16_t where_v;
    int16_t where_h;
} qrx_event_t;

typedef struct {
    qrx_ppc_t *ppc;
    qrx_event_t queue[QRX_EV_QUEUE];
    unsigned head;       /* index of the oldest queued event */
    unsigned count;
    int16_t mouse_v;
    int16_t mouse_h;
    int mouse_down;
    uint8_t keymap[16];  /* one bit per virtual key code */
    uint16_t modifiers;
    uint32_t ticks;
    uint32_t (*tick_fn)(void *);
    void *tick_ctx;
} qrx_ev_t;

void qrx_ev_init(qrx_ev_t *ev, qrx_ppc_t *ppc);
/* TickCount source: when fn is NULL the internal ticks value is used. */
void qrx_ev_set_tick_source(qrx_ev_t *ev, uint32_t (*fn)(void *), void *ctx);
void qrx_ev_set_ticks(qrx_ev_t *ev, uint32_t ticks);
void qrx_ev_advance_ticks(qrx_ev_t *ev, uint32_t delta);
void qrx_ev_set_mouse(qrx_ev_t *ev, int16_t v, int16_t h, int down);
void qrx_ev_set_key(qrx_ev_t *ev, unsigned code, int down);
void qrx_ev_set_modifiers(qrx_ev_t *ev, uint16_t modifiers);
/* Queues one record at the current tick and mouse position; 0 when full. */
int qrx_ev_post(qrx_ev_t *ev, int16_t what, int32_t message);
unsigned qrx_ev_count(const qrx_ev_t *ev);
uint32_t qrx_ev_dispatch(qrx_ev_t *ev, const qrx_mix_args_t *args);

/* ---- unified toolbox dispatch -----------------------------------------
 * One import that routes by trap number: Memory Manager traps go to the
 * MM layer, File Manager traps to the FM layer (param block in r4),
 * Resource Manager traps to the RM layer, QuickDraw/Window/Menu/Dialog
 * traps to their layers.  r4/r5/r6/r7 are the routine's arguments, the
 * return value lands in r3. */
typedef struct {
    qrx_mm_t mm;
    qrx_rm_t rm;
    qrx_fm_t fm;
    qrx_qd_t qd;
    qrx_wm_t wm;
    qrx_menu_mgr_t menu;
    qrx_dm_t dm;
    qrx_font_t font;
    qrx_ev_t ev;
} qrx_tb_t;

void qrx_tb_set_screen(qrx_tb_t *tb, uint32_t bitmap_addr);

void qrx_tb_init(qrx_tb_t *tb, qrx_ppc_t *ppc, uint32_t zone_base,
                 uint32_t zone_size);

uint32_t qrx_tb_dispatch(void *ctx, const qrx_mix_args_t *args,
                         qrx_mix_result_t *extra);
uint32_t qrx_rm_dispatch(qrx_rm_t *rm, const qrx_mix_args_t *args);
uint32_t qrx_fm_dispatch(qrx_fm_t *fm, const qrx_mix_args_t *args);
uint32_t qrx_qd_dispatch(qrx_qd_t *qd, const qrx_mix_args_t *args);
uint32_t qrx_wm_dispatch(qrx_wm_t *wm, const qrx_mix_args_t *args);
uint32_t qrx_menu_dispatch(qrx_menu_mgr_t *mmgr, const qrx_mix_args_t *args);
uint32_t qrx_dm_dispatch(qrx_dm_t *dm, const qrx_mix_args_t *args);

#endif /* QRX_TOOLBOX_H */
