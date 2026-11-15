/*
 * QRXPPC - 68k classic MacOS runtime (P2.4).
 *
 * A classic 68k MacOS application keeps its code in 'CODE' resources: CODE 0
 * is the jump table (the A5 world), CODE 1..n are the segments.  This module
 * maps a flat 16 MiB Macintosh address space, loads the segments, copies the
 * jump table to A5 and executes the application on the Musashi 68020 core that
 * the ARMithlon engine already carries.  Toolbox calls become A-line traps and
 * land in the HLE below; the first trap the HLE does not implement ends the run
 * with QRX_M68K_UNIMPLEMENTED, exactly like QRXAppRun does for PPC.
 *
 * The runtime is deliberately small: no ROM, no MacOS, no QuickDraw.  Memory
 * Manager, Resource Manager, File Manager and the segment loader are provided
 * only as far as the HLE table below lists them.  Installers whose 'CODE'
 * resources are packed by their builder (VISE, Aladdin ADC) are rejected at
 * load time as QRX_M68K_ERR_PACKED, with the format in the detail text.
 */
#ifndef QRX_M68K_H
#define QRX_M68K_H

#include <stdint.h>

#define QRX_M68K_MEM_SIZE (16u * 1024u * 1024u) /* default; mem-kib may ask more */
#define QRX_M68K_TRACE_MAX 32u
#define QRX_M68K_SEG_MAX 64u
#define QRX_M68K_DETAIL 160u
#define QRX_M68K_A5_BASE 0x00002000u   /* jump table and application globals */
#define QRX_M68K_A5_MAX 0x00010000u    /* space reserved for the A5 world */
#define QRX_M68K_EXIT_STUB 0x00001000u /* TRAP #0: the application returned */
#define QRX_M68K_STACK_TOP 0x00080000u /* stack grows down from here */
#define QRX_M68K_CODE_BASE 0x00100000u /* first segment */
#define QRX_M68K_HEAP_FRACTION 2u      /* heap starts at mem_size / 2 */
#define QRX_M68K_HEAP_RESERVE 0x00100000u /* 1 MiB left above the heap */
#define QRX_M68K_HEAP_FREE_MAX 32u     /* returned blocks kept for reuse */

/* Trap numbers the HLE answers, with the A-line nibble stripped: the trap
 * word is 0xA000 | number. */
#define QRX_M68K_TRAP_LOAD_SEG 0x9F0u      /* A9F0 */
#define QRX_M68K_TRAP_EXIT_TO_SHELL 0x9F4u /* A9F4 */
#define QRX_M68K_TRAP_GESTALT 0x1ADu       /* A1AD */
#define QRX_M68K_TRAP_GET_ZONE 0x11Au      /* A11A */
#define QRX_M68K_TRAP_SET_ZONE 0x11Bu      /* A11B */
#define QRX_M68K_TRAP_NEW_PTR 0x11Eu       /* A11E */
#define QRX_M68K_TRAP_DISPOSE_PTR 0x11Fu   /* A11F */
#define QRX_M68K_TRAP_SYS_ENVIRONS 0x90u   /* A090 */
#define QRX_M68K_TRAP_NEW_HANDLE 0x122u         /* A122 */
#define QRX_M68K_TRAP_DISPOSE_HANDLE 0x023u     /* A023 */
#define QRX_M68K_TRAP_SET_HANDLE_SIZE 0x024u    /* A024 */
#define QRX_M68K_TRAP_GET_HANDLE_SIZE 0x025u    /* A025 */
#define QRX_M68K_TRAP_HLOCK 0x029u              /* A029 */
#define QRX_M68K_TRAP_HUNLOCK 0x02Au            /* A02A */
#define QRX_M68K_TRAP_OPEN_RES_FILE 0x997u      /* A997 */
#define QRX_M68K_TRAP_CUR_RES_FILE 0x994u       /* A994 */
#define QRX_M68K_TRAP_USE_RES_FILE 0x998u       /* A998 */
#define QRX_M68K_TRAP_GET_RESOURCE 0x9A0u       /* A9A0 */
#define QRX_M68K_TRAP_OPEN 0x000u                /* A000 */
#define QRX_M68K_TRAP_CLOSE 0x001u               /* A001 */
#define QRX_M68K_TRAP_READ 0x002u                /* A002 */
#define QRX_M68K_TRAP_WRITE 0x003u               /* A003 */
#define QRX_M68K_TRAP_CREATE 0x008u              /* A008 */
#define QRX_M68K_TRAP_GET_FPOS 0x010u            /* A010 */
#define QRX_M68K_TRAP_SET_FPOS 0x011u            /* A011 */
#define QRX_M68K_TRAP_GET_EOF 0x016u             /* A016 */
#define QRX_M68K_TRAP_RELEASE_RESOURCE 0x9A3u   /* A9A3 */
/* QuickDraw, cursor, Dialog Manager and Event Manager traps.  The numbers are
 * the ones the PPC toolbox slice already uses (cxmon mon_atraps) and the Dialog
 * Manager entries next to them. */
#define QRX_M68K_TRAP_INIT_CURSOR 0x850u    /* A850 */
#define QRX_M68K_TRAP_HIDE_CURSOR 0x852u    /* A852 */
#define QRX_M68K_TRAP_SHOW_CURSOR 0x853u    /* A853 */
#define QRX_M68K_TRAP_WAIT_NEXT_EVENT 0x860u /* A860 */
#define QRX_M68K_TRAP_INIT_PORT 0x86Du      /* A86D */
#define QRX_M68K_TRAP_INIT_GRAF 0x86Eu      /* A86E */
#define QRX_M68K_TRAP_OPEN_PORT 0x86Fu      /* A86F */
#define QRX_M68K_TRAP_SET_PORT 0x873u       /* A873 */
#define QRX_M68K_TRAP_GET_PORT 0x874u       /* A874 */
#define QRX_M68K_TRAP_PORT_SIZE 0x876u      /* A876 */
#define QRX_M68K_TRAP_MOVE_PORT_TO 0x877u   /* A877 */
#define QRX_M68K_TRAP_SET_CLIP 0x879u       /* A879 */
#define QRX_M68K_TRAP_BACK_PAT 0x87Cu       /* A87C */
#define QRX_M68K_TRAP_CLOSE_PORT 0x87Du     /* A87D */
#define QRX_M68K_TRAP_SET_PT 0x880u         /* A880 */
#define QRX_M68K_TRAP_DRAW_CHAR 0x883u      /* A883 */
#define QRX_M68K_TRAP_DRAW_STRING 0x884u    /* A884 */
#define QRX_M68K_TRAP_DRAW_TEXT 0x885u      /* A885 */
#define QRX_M68K_TRAP_TEXT_FONT 0x887u      /* A887 */
#define QRX_M68K_TRAP_TEXT_FACE 0x888u      /* A888 */
#define QRX_M68K_TRAP_TEXT_MODE 0x889u      /* A889 */
#define QRX_M68K_TRAP_TEXT_SIZE 0x88Au      /* A88A */
#define QRX_M68K_TRAP_LINE_TO 0x891u        /* A891 */
#define QRX_M68K_TRAP_LINE 0x892u           /* A892 */
#define QRX_M68K_TRAP_MOVE_TO 0x893u        /* A893 */
#define QRX_M68K_TRAP_HIDE_PEN 0x896u       /* A896 */
#define QRX_M68K_TRAP_SHOW_PEN 0x897u       /* A897 */
#define QRX_M68K_TRAP_GET_PEN 0x89Au        /* A89A */
#define QRX_M68K_TRAP_PEN_SIZE 0x89Bu       /* A89B */
#define QRX_M68K_TRAP_PEN_MODE 0x89Cu       /* A89C */
#define QRX_M68K_TRAP_PEN_PAT 0x89Du        /* A89D */
#define QRX_M68K_TRAP_PEN_NORMAL 0x89Eu     /* A89E */
#define QRX_M68K_TRAP_FRAME_RECT 0x8A1u     /* A8A1 */
#define QRX_M68K_TRAP_PAINT_RECT 0x8A2u     /* A8A2 */
#define QRX_M68K_TRAP_ERASE_RECT 0x8A3u     /* A8A3 */
#define QRX_M68K_TRAP_INVERT_RECT 0x8A4u    /* A8A4 */
#define QRX_M68K_TRAP_FILL_RECT 0x8A5u      /* A8A5 */
#define QRX_M68K_TRAP_EQUAL_RECT 0x8A6u     /* A8A6 */
#define QRX_M68K_TRAP_OFFSET_RECT 0x8A8u    /* A8A8 */
#define QRX_M68K_TRAP_INSET_RECT 0x8A9u     /* A8A9 */
#define QRX_M68K_TRAP_PT_IN_RECT 0x8ADu     /* A8AD */
#define QRX_M68K_TRAP_GET_NEXT_EVENT 0x970u /* A970 */
#define QRX_M68K_TRAP_EVENT_AVAIL 0x971u    /* A971 */
#define QRX_M68K_TRAP_GET_MOUSE 0x972u      /* A972 */
#define QRX_M68K_TRAP_STILL_DOWN 0x973u     /* A973 */
#define QRX_M68K_TRAP_BUTTON 0x974u         /* A974 */
#define QRX_M68K_TRAP_TICK_COUNT 0x975u     /* A975 */
#define QRX_M68K_TRAP_GET_KEYS 0x976u       /* A976 */
#define QRX_M68K_TRAP_WAIT_MOUSE_UP 0x977u  /* A977 */
#define QRX_M68K_TRAP_POST_EVENT 0x02Fu     /* A02F */
#define QRX_M68K_TRAP_FLUSH_EVENTS 0x032u   /* A032 */
#define QRX_M68K_TRAP_SYSTEM_TASK 0x9B4u    /* A9B4 */
#define QRX_M68K_TRAP_INIT_DIALOGS 0x97Bu   /* A97B */
#define QRX_M68K_TRAP_GET_NEW_DIALOG 0x97Cu /* A97C */
#define QRX_M68K_TRAP_IS_DIALOG_EVENT 0x97Fu /* A97F */
#define QRX_M68K_TRAP_INFO_SCRAP 0x9F9u     /* A9F9 */
#define QRX_M68K_TRAP_UNLOAD_SCRAP 0x9FAu   /* A9FA */
#define QRX_M68K_TRAP_LOAD_SCRAP 0x9FBu     /* A9FB */
#define QRX_M68K_TRAP_ZERO_SCRAP 0x9FCu     /* A9FC */
#define QRX_M68K_TRAP_GET_SCRAP 0x9FDu      /* A9FD */
#define QRX_M68K_TRAP_PUT_SCRAP 0x9FEu      /* A9FE */
#define QRX_M68K_TRAP_DIALOG_SELECT 0x980u  /* A980 */
#define QRX_M68K_TRAP_DRAW_DIALOG 0x981u    /* A981 */
#define QRX_M68K_TRAP_CLOSE_DIALOG 0x982u   /* A982 */
#define QRX_M68K_TRAP_DISPOSE_DIALOG 0x983u /* A983 */
#define QRX_M68K_TRAP_MODAL_DIALOG 0x991u   /* A991 */
#define QRX_M68K_TRAP_ALERT 0x985u          /* A985 */
#define QRX_M68K_TRAP_STOP_ALERT 0x986u     /* A986 */
#define QRX_M68K_TRAP_NOTE_ALERT 0x987u     /* A987 */
#define QRX_M68K_TRAP_CAUTION_ALERT 0x988u  /* A988 */
#define QRX_M68K_TRAP_PARAM_TEXT 0x98Bu     /* A98B */
#define QRX_M68K_TRAP_SYS_BEEP 0x9C8u       /* A9C8 */
/* Window Manager and Menu Manager traps (same trap table). */
#define QRX_M68K_TRAP_GET_WMGR_PORT 0x910u   /* A910 */
#define QRX_M68K_TRAP_INIT_WINDOWS 0x912u    /* A912 */
#define QRX_M68K_TRAP_NEW_WINDOW 0x913u      /* A913 */
#define QRX_M68K_TRAP_DISPOSE_WINDOW 0x914u  /* A914 */
#define QRX_M68K_TRAP_SHOW_WINDOW 0x915u     /* A915 */
#define QRX_M68K_TRAP_HIDE_WINDOW 0x916u     /* A916 */
#define QRX_M68K_TRAP_GET_WREF_CON 0x917u    /* A917 */
#define QRX_M68K_TRAP_SET_WREF_CON 0x918u    /* A918 */
#define QRX_M68K_TRAP_GET_WTITLE 0x919u      /* A919 */
#define QRX_M68K_TRAP_SET_WTITLE 0x91Au      /* A91A */
#define QRX_M68K_TRAP_MOVE_WINDOW 0x91Bu     /* A91B */
#define QRX_M68K_TRAP_HILITE_WINDOW 0x91Cu   /* A91C */
#define QRX_M68K_TRAP_SIZE_WINDOW 0x91Du     /* A91D */
#define QRX_M68K_TRAP_SELECT_WINDOW 0x91Fu   /* A91F */
#define QRX_M68K_TRAP_BRING_TO_FRONT 0x920u  /* A920 */
#define QRX_M68K_TRAP_SEND_BEHIND 0x921u     /* A921 */
#define QRX_M68K_TRAP_BEGIN_UPDATE 0x922u    /* A922 */
#define QRX_M68K_TRAP_END_UPDATE 0x923u      /* A923 */
#define QRX_M68K_TRAP_FRONT_WINDOW 0x924u    /* A924 */
#define QRX_M68K_TRAP_INVAL_RECT 0x928u      /* A928 */
#define QRX_M68K_TRAP_FIND_WINDOW 0x92Cu     /* A92C */
#define QRX_M68K_TRAP_CLOSE_WINDOW 0x92Du    /* A92D */
#define QRX_M68K_TRAP_INIT_MENUS 0x930u      /* A930 */
#define QRX_M68K_TRAP_NEW_MENU 0x931u        /* A931 */
#define QRX_M68K_TRAP_DISPOSE_MENU 0x932u    /* A932 */
#define QRX_M68K_TRAP_APPEND_MENU 0x933u     /* A933 */
#define QRX_M68K_TRAP_CLEAR_MENU_BAR 0x934u  /* A934 */
#define QRX_M68K_TRAP_INSERT_MENU 0x935u     /* A935 */
#define QRX_M68K_TRAP_DELETE_MENU 0x936u     /* A936 */
#define QRX_M68K_TRAP_DRAW_MENU_BAR 0x937u   /* A937 */
#define QRX_M68K_TRAP_ENABLE_ITEM 0x939u     /* A939 */
#define QRX_M68K_TRAP_DISABLE_ITEM 0x93Au    /* A93A */
#define QRX_M68K_TRAP_CHECK_ITEM 0x945u      /* A945 */
#define QRX_M68K_TRAP_GET_MENU_ITEM_TEXT 0x946u /* A946 */
#define QRX_M68K_TRAP_SET_MENU_ITEM_TEXT 0x947u /* A947 */
#define QRX_M68K_TRAP_COUNT_MITEMS 0x950u    /* A950 */
#define QRX_M68K_TRAP_DELETE_MENU_ITEM 0x952u /* A952 */
#define QRX_M68K_TRAP_GET_ITEM_CMD 0x84Eu   /* A84E: menu A0, item D0, byte* A1 */
#define QRX_M68K_TRAP_SET_ITEM_CMD 0x84Fu   /* A84F: menu A0, item D0, char D1 */
#define QRX_M68K_TRAP_SET_ITM_ICON 0x940u   /* A940: menu A0, item D0, icon D1 */
#define QRX_M68K_TRAP_SET_ITM_STYLE 0x942u  /* A942: menu A0, item D0, style D1 */
#define QRX_M68K_TRAP_SET_ITEM_MARK 0x944u  /* A944: menu A0, item D0, mark D1 */
/* Control Manager.  A control is a record this runtime keeps; the handle it
 * hands out is opaque to the guest, which asks for values through the traps. */
#define QRX_M68K_TRAP_NEW_CONTROL 0x954u
#define QRX_M68K_TRAP_DISPOSE_CONTROL 0x955u
#define QRX_M68K_TRAP_KILL_CONTROLS 0x956u
#define QRX_M68K_TRAP_SHOW_CONTROL 0x957u
#define QRX_M68K_TRAP_HIDE_CONTROL 0x958u
#define QRX_M68K_TRAP_MOVE_CONTROL 0x959u
#define QRX_M68K_TRAP_SIZE_CONTROL 0x95Cu
#define QRX_M68K_TRAP_HILITE_CONTROL 0x95Du
#define QRX_M68K_TRAP_GET_CONTROL_TITLE 0x95Eu
#define QRX_M68K_TRAP_SET_CONTROL_TITLE 0x95Fu
#define QRX_M68K_TRAP_GET_CONTROL_VALUE 0x960u
#define QRX_M68K_TRAP_GET_CONTROL_MIN 0x961u
#define QRX_M68K_TRAP_GET_CONTROL_MAX 0x962u
#define QRX_M68K_TRAP_SET_CONTROL_VALUE 0x963u
#define QRX_M68K_TRAP_DRAW_CONTROLS 0x969u
#define QRX_M68K_TRAP_FIND_CONTROL 0x96Cu
#define QRX_M68K_TRAP_DRAW_1_CONTROL 0x96Du
#define QRX_M68K_TRAP_TRACK_CONTROL 0x968u
#define QRX_M68K_TRAP_HILITE_MENU 0x938u    /* A938: menu ID in D0 */
#define QRX_M68K_TRAP_MENU_SELECT 0x93Du    /* A93D: Point on the stack */
#define QRX_M68K_TRAP_MENU_KEY 0x93Eu       /* A93E: character in D0 */

/* Window operations the window hook receives. */
#define QRX_M68K_WIN_CREATE 0
#define QRX_M68K_WIN_SHOW 1
#define QRX_M68K_WIN_HIDE 2
#define QRX_M68K_WIN_SELECT 3
#define QRX_M68K_WIN_MOVE 4
#define QRX_M68K_WIN_DISPOSE 5
#define QRX_M68K_WIN_UPDATE_BEGIN 6
#define QRX_M68K_WIN_UPDATE_END 7
#define QRX_M68K_WIN_INVAL 8
#define QRX_M68K_WIN_TITLE_CHANGED 9
#define QRX_M68K_WIN_HILITE 10

/* Menu operations the menu hook receives. */
#define QRX_M68K_MENU_NEW 0
#define QRX_M68K_MENU_ITEM 1
#define QRX_M68K_MENU_DELETE_ITEM 2
#define QRX_M68K_MENU_BAR 3
#define QRX_M68K_MENU_DISPOSE 4
#define QRX_M68K_MENU_ITEM_TEXT 5
/* Enable/Disable/CheckItem only change an item that already exists: value
 * carries bit 0 = enabled and bit 1 = checked, so the guest can patch the live
 * menu instead of rebuilding it. */
#define QRX_M68K_MENU_ITEM_STATE 6
/* HiliteMenu: value carries the menu ID whose title is highlighted, 0 takes
 * the highlight away again (the usual MenuSelect/HiliteMenu(0) pair). */
#define QRX_M68K_MENU_HILITE 7
/* SetItemCmd: value carries the Command-key character (already uppercased) for
 * an existing item, so the guest can show it in the strip. */
#define QRX_M68K_MENU_ITEM_CMD 8
/* SetItmStyle/SetItmIcon/SetItemMark carry the raw MacOS value of the item. */
#define QRX_M68K_MENU_ITEM_STYLE 9
#define QRX_M68K_MENU_ITEM_ICON 10
#define QRX_M68K_MENU_ITEM_MARK 11

/* Window records this runtime hands out.  The fields are the ones the HLE and
 * the window hook use; this is not a byte copy of the ROM WindowRecord, so an
 * application must reach its data through the Toolbox, not through GrafPort
 * internals. */
#define QRX_M68K_WINDOW_MAX 8u
#define QRX_M68K_CONTROL_MAX 16u
#define QRX_M68K_WINDOW_RECORD 64u
#define QRX_M68K_WIN_KIND 0u      /* int16, 0 = document window */
#define QRX_M68K_WIN_VISIBLE 2u   /* uint8 */
#define QRX_M68K_WIN_HILITED 3u   /* uint8 */
#define QRX_M68K_WIN_REFCON 4u    /* int32 */
#define QRX_M68K_WIN_FRAME 8u     /* Rect */
#define QRX_M68K_WIN_NEXT 16u     /* uint32, next window in the list */
#define QRX_M68K_WIN_TITLE 20u    /* Pascal string, at most 31 bytes */

#define QRX_M68K_MENU_MAX 8u
#define QRX_M68K_MENU_ITEM_MAX 24u
#define QRX_M68K_MENU_TEXT 32u
#define QRX_M68K_MENU_BAR_MAX 8u

/* Operations the rectangle hook receives. */
#define QRX_M68K_RECT_FILL 0
#define QRX_M68K_RECT_FRAME 1
#define QRX_M68K_RECT_PAINT 2
#define QRX_M68K_RECT_ERASE 3
#define QRX_M68K_RECT_INVERT 4

/* Sizes of the event record and of the stub dialog record the Dialog Manager
 * traps hand out (see QRX_M68K_DLG_* below). */
#define QRX_M68K_EVENT_RECORD 16u
#define QRX_M68K_PARAM_TEXT 4u
#define QRX_M68K_TEXT_MAX 64u
#define QRX_M68K_DIALOG_MAX 4u
#define QRX_M68K_DIALOG_ITEM_MAX 24u
#define QRX_M68K_DLG_RECORD 64u
#define QRX_M68K_DLG_ENTRY 12u
/* Stub DialogRecord: windowKind, visible, item count, last item hit, the
 * bounds the item rectangles were folded into, and the parsed item table. */
#define QRX_M68K_DLG_KIND 0u      /* int16, 2 = dialogKind */
#define QRX_M68K_DLG_VISIBLE 2u   /* uint8 */
#define QRX_M68K_DLG_ITEMS 4u     /* int16, number of DITL items */
#define QRX_M68K_DLG_HIT 6u       /* int16, item ModalDialog reports */
#define QRX_M68K_DLG_BOUNDS 8u    /* Rect */
#define QRX_M68K_DLG_TABLE 16u    /* uint32, guest address of the item table */

/* The runtime has no screen and no event source: drawing leaves through these
 * hooks (the AROS guest prints them, the host test records them), and the
 * Event Manager answers from a one-event queue the application fills itself
 * with PostEvent.  Patterns are the 8 bytes of a QuickDraw Pattern. */

/* The menu pick the guest reports as menuEvt (what = 15): the MacOS menu ID in
 * the high half of the message, the item number in the low half. */
#define QRX_M68K_EVENT_MENU 15u

/* Events the Event Manager holds before the oldest is dropped. */
#define QRX_M68K_EVENT_QUEUE 8u

/* Open files the File Manager traps can hold at once, and the longest guest
 * path that is converted for the host. */
#define QRX_M68K_FILE_MAX 8u
#define QRX_M68K_PATH_MAX 256u

/* The runtime has no file system of its own.  The caller installs these five
 * hooks after loading and the File Manager traps translate MacOS calls into
 * them; the AROS guest backs them with dos.library, the host test with stdio.
 * Paths arrive as plain relative names with '/' separators, never with "..". */
typedef struct {
    void *ctx;
    long (*open)(void *ctx, const char *path, int create);   /* fd >= 0, <0 error */
    long (*read)(void *ctx, long fd, void *buf, uint32_t len);
    long (*write)(void *ctx, long fd, const void *buf, uint32_t len);
    long (*seek)(void *ctx, long fd, int32_t offset, int whence); /* 0 set, 1 current, 2 end */
    void (*close)(void *ctx, long fd);
} qrx_m68k_fs_t;

/* Shared clipboard.  The Scrap Manager traps translate the application's
 * 'TEXT' scrap into these hooks; the AROS guest backs them with
 * clipboard.device, so a cut in a Mac application reaches AROS applications
 * and a copy there reaches the Mac application.  put replaces the whole clip,
 * get copies at most len bytes and returns how many, size returns the length
 * of the clip. */
typedef struct {
    void *ctx;
    void (*put)(void *ctx, const uint8_t *data, uint32_t len);
    int32_t (*get)(void *ctx, uint8_t *buf, uint32_t len);
    int32_t (*size)(void *ctx);
} qrx_m68k_clip_t;

/* QuickDraw output.  The runtime has no screen of its own: the guest hands
 * rect, line and text drawing to these hooks and puts the pixels into the
 * AROS window the pen belongs to.  Without them the calls still answer but
 * draw nothing. */
typedef struct {
    void *ctx;
    void (*rect)(void *ctx, int32_t top, int32_t left, int32_t bottom,
                 int32_t right, int op, const uint8_t *pattern);
    void (*line)(void *ctx, int32_t from_y, int32_t from_x, int32_t to_y,
                 int32_t to_x, uint16_t mode, uint16_t width, uint16_t height);
    void (*text)(void *ctx, int32_t y, int32_t x, const char *text,
                 uint32_t len, uint16_t font, uint16_t size, uint16_t face);
    void (*window)(void *ctx, int op, int32_t top, int32_t left, int32_t bottom,
                   int32_t right, const char *title, uint32_t len, uint32_t id);
    void (*menu)(void *ctx, int op, const char *title, uint32_t title_len,
                 int32_t item, const char *text, uint32_t text_len,
                 int32_t value, uint32_t id);
    /* The guest pumps its own event source here (an AROS window's IDCMP port)
     * before an event trap answers, and names the window a drawing call goes
     * into.  Without them events never arrive and drawing stays log-only. */
    void (*poll)(void *ctx);
    void (*target)(void *ctx, uint32_t id);
} qrx_m68k_gfx_t;

/* Scrap Manager results, as the Toolbox reports them. */
#define QRX_M68K_SCRAP_NO_SCRAP (-100)  /* noScrapErr */
#define QRX_M68K_SCRAP_NO_TYPE (-102)   /* noTypeErr */
#define QRX_M68K_SCRAP_NO_MEM (-108)    /* memFullErr */

/* Master pointers and resource handles the HLE keeps bookkeeping for. */
#define QRX_M68K_HANDLE_MAX 32u
#define QRX_M68K_RES_CACHE 32u

/* SysEnvirons reports no specific Macintosh model: the runtime is a 68020 with
 * System 8.1 values, not a cloned board. */
#define QRX_M68K_MACHINE_TYPE 0x0000u

typedef enum {
    QRX_M68K_OK = 0,
    QRX_M68K_ERR_ARG,        /* NULL pointer or impossible layout */
    QRX_M68K_ERR_IMAGE,      /* neither MacBinary nor a resource fork */
    QRX_M68K_ERR_NO_CODE,    /* no 'CODE' resources: not a 68k application */
    QRX_M68K_ERR_MEM,        /* segments do not fit the 16 MiB space */
    QRX_M68K_ERR_PACKED,     /* 'CODE' packed by the installer builder */
    QRX_M68K_UNIMPLEMENTED,  /* first trap the HLE does not implement */
    QRX_M68K_EXITED,         /* the application returned to the exit stub */
    QRX_M68K_LIMIT,          /* instruction budget exhausted */
    QRX_M68K_FAULT,          /* address error, illegal non-trap instruction */
    QRX_M68K_ERR_RUN         /* other CPU or dispatch failure */
} qrx_m68k_status_t;

typedef struct {
    uint32_t trap;        /* A-line trap number */
    uint32_t pc;          /* address of the trap instruction */
    uint32_t d0;          /* D0 at the call */
    uint32_t a0;          /* A0 at the call */
    uint32_t result;      /* value left in D0 */
    uint8_t implemented;  /* 0 when the HLE stopped at this trap */
} qrx_m68k_trap_t;

typedef struct {
    uint8_t *mem;
    uint32_t mem_size;
    uint32_t a5;
    uint32_t entry;
    uint32_t sp;
    uint32_t heap;           /* next NewPtr address */
    uint32_t heap_end;       /* first address NewPtr may not hand out */
    /* Blocks handed back by DisposeControl/KillControls; NewPtr reuses them
     * first-fit, and a block at the top of the heap lowers the bump pointer. */
    unsigned nheap_free;
    struct {
        uint32_t addr;
        uint32_t size;       /* rounded up to four bytes */
    } heap_free[QRX_M68K_HEAP_FREE_MAX];
    unsigned code_count;
    struct {
        uint32_t id;
        uint32_t base;
        uint32_t size;
    } seg[QRX_M68K_SEG_MAX];
    uint8_t *rsrc;           /* copy of the application's resource fork */
    uint32_t rsrc_len;
    char app_name[64];       /* name OpenResFile compares against */
    uint32_t resfile;        /* current resource file, 0xFFFFFFFF = -1 */
    unsigned nhandle;
    struct {
        uint32_t mp;         /* master pointer, 0 when the slot is free */
        uint32_t size;
    } handle[QRX_M68K_HANDLE_MAX];
    unsigned nres;
    struct {
        uint32_t type;
        int32_t id;
        uint32_t h;
    } rescache[QRX_M68K_RES_CACHE];
    qrx_m68k_fs_t fs;        /* file hooks, installed after loading */
    int has_fs;
    struct {
        long fd;
        int used;
    } file[QRX_M68K_FILE_MAX];
    qrx_m68k_gfx_t gfx;      /* QuickDraw output, installed by the caller */
    int has_gfx;
    uint32_t port;           /* current grafPort, 0 when none is open */
    int32_t pen_y, pen_x;    /* pen position */
    uint16_t pen_mode;
    uint16_t pen_size_w, pen_size_h;
    uint8_t pen_pat[8];
    int pen_vis;
    uint16_t text_font, text_size, text_face, text_mode;
    uint32_t ticks;          /* TickCount source, one tick per call */
    int mouse_down;          /* Button/StillDown state */
    int32_t mouse_y, mouse_x;
    /* Events arrive from two sides: the application's PostEvent and the
     * guest's input pump.  The queue keeps the oldest eight. */
    unsigned nevent;
    struct {
        uint16_t what;
        uint32_t message;
        int32_t vy, vx;
        uint16_t mods;
    } evq[QRX_M68K_EVENT_QUEUE];
    qrx_m68k_clip_t clip;    /* shared clipboard, installed by the caller */
    int has_clip;
    unsigned nwindow;
    struct {
        uint32_t rec;
        int used;
        int visible;
        int hilited;
        uint32_t wref;
        int32_t top, left, bottom, right;
        char title[QRX_M68K_MENU_TEXT];
    } win[QRX_M68K_WINDOW_MAX];
    uint32_t front;          /* front window, 0 when there is none */
    uint32_t wmgr_port;      /* the port GetWMgrPort hands out */
    unsigned nmenu;
    struct {
        uint32_t handle;
        int used;
        int16_t id;
        unsigned count;
        char title[QRX_M68K_MENU_TEXT];
        char item[QRX_M68K_MENU_ITEM_MAX][QRX_M68K_MENU_TEXT];
        int enabled[QRX_M68K_MENU_ITEM_MAX];
        int checked[QRX_M68K_MENU_ITEM_MAX];
        char cmd[QRX_M68K_MENU_ITEM_MAX];   /* Command-key equivalent */
        int16_t style[QRX_M68K_MENU_ITEM_MAX];  /* MacOS style bits */
        int16_t icon[QRX_M68K_MENU_ITEM_MAX];   /* MacOS icon index */
        char mark[QRX_M68K_MENU_ITEM_MAX];      /* mark character, 0 = none */
    } menu[QRX_M68K_MENU_MAX];
    uint32_t mbar[QRX_M68K_MENU_BAR_MAX];   /* menu handles in bar order */
    unsigned nmbar;
    /* The last menu the guest reported as picked, menuID << 16 | item, the way
     * MenuSelect hands it back. */
    uint32_t menu_pick;
    int menu_pick_valid;
    int32_t menu_point_y, menu_point_x;     /* the startPt MenuSelect got */
    int16_t menu_hilite;                    /* title HiliteMenu keeps lit */
    unsigned ncontrol;
    struct {
        uint32_t handle;
        int used;
        uint32_t owner;          /* the window or dialog record it belongs to */
        int visible;
        int16_t value, min, max;
        int16_t hilite;
        int16_t item;            /* DITL item number, 0 for a program control */
        int32_t top, left, bottom, right;
        char title[QRX_M68K_MENU_TEXT];
    } ctrl[QRX_M68K_CONTROL_MAX];
    unsigned ndialog;
    struct {
        uint32_t rec;
        uint16_t count;
    } dialog_rec[QRX_M68K_DIALOG_MAX];
    char param[QRX_M68K_PARAM_TEXT][QRX_M68K_TEXT_MAX];
    qrx_m68k_trap_t trace[QRX_M68K_TRACE_MAX];
    unsigned ntrace;
    unsigned nimplemented;
    uint32_t stop_trap;
    char detail[QRX_M68K_DETAIL];
} qrx_m68k_t;

/* Parse a MacBinary image (or a bare 'CODE' resource fork) and lay out the
 * A5 world.  The image only has to live during this call: segments are copied
 * into the machine's memory. */
qrx_m68k_status_t qrx_m68k_load(qrx_m68k_t *m, const uint8_t *image,
                                uint32_t size);

/* Same, but never allocates more than max_mem bytes (0 = default sizing).
 * MemTop, the heap base and the NewPtr limit follow the size that was actually
 * available, so a smaller machine never reports addresses outside its block. */
qrx_m68k_status_t qrx_m68k_load_limited(qrx_m68k_t *m, const uint8_t *image,
                                        uint32_t size, uint32_t max_mem);

/* Execute the application.  budget is a Musashi cycle budget. */
qrx_m68k_status_t qrx_m68k_start(qrx_m68k_t *m, uint32_t budget);


/* Install the file hooks the File Manager traps call.  Call it after a load;
 * the runtime keeps a copy of the struct, ctx stays the caller's. */
void qrx_m68k_set_fs(qrx_m68k_t *m, const qrx_m68k_fs_t *fs);

/* Install the QuickDraw hooks.  Like qrx_m68k_set_fs, call it after a load;
 * the runtime keeps a copy of the struct, ctx stays the caller's. */
void qrx_m68k_set_gfx(qrx_m68k_t *m, const qrx_m68k_gfx_t *gfx);

/* Install the shared clipboard hooks the Scrap Manager traps call. */
void qrx_m68k_set_clip(qrx_m68k_t *m, const qrx_m68k_clip_t *clip);

/* Queue an event the way the guest's input pump does: the Event Manager hands
 * it out before it answers with nullEvent.  Returns 0 when the queue is full. */
int qrx_m68k_post_event(qrx_m68k_t *m, uint16_t what, uint32_t message,
                        int32_t vy, int32_t vx, uint16_t mods);

/* Free the machine block and the resource copy a load allocated. */
void qrx_m68k_unload(qrx_m68k_t *m);

const char *qrx_m68k_status_name(qrx_m68k_status_t st);

#endif /* QRX_M68K_H */
