/*
 * QRX68KRun - run a classic MacOS 68k application under the QRXPPC 68k HLE.
 *
 * Usage: QRX68KRun <application> [resource-fork] [mem-kib] [host-root]
 *
 * The first file is a MacBinary application or a bare resource fork; a separate
 * resource fork is wrapped into a MacBinary image in memory first, which is what
 * a file extracted from HFS or a .sit archive usually needs.  The application's
 * 'CODE' segments are loaded into a 16 MiB Macintosh address space and executed
 * on the Musashi 68020 core; every Toolbox trap is printed and the run ends at
 * the first trap the HLE does not implement.
 *
 * Exit codes: 0 application returned, 2 unimplemented trap, 3 no 'CODE'
 * resources, 4 packed 'CODE' resources, 1 any other stop.
 *
 * [host-root] is the directory the File Manager traps write into: MacOS paths
 * with ':' separators become relative paths below it ("TestDir:file" ->
 * <root>/TestDir/file).  Without it the paths are used as they are.
 */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/gfxbase.h>
#include <graphics/gfx.h>
#include <libraries/gadtools.h>
#include <devices/clipboard.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "qrx_m68k.h"
#include "qrx_macapp.h"

#define APP_CYCLES 4000000000u

/* The File Manager HLE needs a file system; in the guest that is dos.library,
 * rooted at the directory the caller names (a HostFS share, usually). */
struct dos_fs {
    char root[256];
};

static void dos_path(const struct dos_fs *f, const char *path, char *out,
                     uint32_t cap)
{
    if (f->root[0] != '\0')
        snprintf(out, cap, "%s/%s", f->root, path);
    else
        snprintf(out, cap, "%s", path);
}

static long dosfs_open(void *ctx, const char *path, int create)
{
    const struct dos_fs *f = (const struct dos_fs *)ctx;
    char full[512];
    BPTR fp;

    dos_path(f, path, full, (uint32_t)sizeof full);
    fp = Open((STRPTR)full, create ? MODE_NEWFILE : MODE_OLDFILE);
    return fp ? (long)fp : -1;
}

static long dosfs_read(void *ctx, long fd, void *buf, uint32_t len)
{
    (void)ctx;
    return (long)Read((BPTR)fd, buf, len);
}

static long dosfs_write(void *ctx, long fd, const void *buf, uint32_t len)
{
    (void)ctx;
    return (long)Write((BPTR)fd, (APTR)buf, len);
}

static long dosfs_seek(void *ctx, long fd, int32_t offset, int whence)
{
    int32_t mode = whence == 0 ? OFFSET_BEGINNING
                   : whence == 2 ? OFFSET_END
                                 : OFFSET_CURRENT;
    int32_t pos;

    (void)ctx;
    pos = Seek((BPTR)fd, offset, mode);
    return pos < 0 ? -1 : (long)pos;
}

static void dosfs_close(void *ctx, long fd)
{
    (void)ctx;
    Close((BPTR)fd);
}

/* ------------------------------------------------- windows, clipboard (AROS)
 *
 * A MacOS window becomes a real Intuition window on the public screen, so a
 * classic application sits on the AROS desktop the way a Windows program sits
 * on the X11 desktop under Wine.  The runtime hands every window operation to
 * the hook below with the window record as its id, QuickDraw drawing goes into
 * the RastPort of the window the application last selected as its port, and
 * the IDCMP port of the window is pumped into the runtime's event queue.  The
 * Scrap Manager is clipboard.device: the Mac scrap is the AROS clip, 'TEXT'
 * being the only flavor with a counterpart. */

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *GadToolsBase = NULL;

#define AROS_WINDOW_MAX 16u

struct aros_window {
    uint32_t id;             /* MacOS window record */
    struct Window *win;
    int used;
    int hidden;
};

static struct aros_window aros_win[AROS_WINDOW_MAX];
static struct Window *draw_win;          /* RastPort the pen draws into */
static qrx_m68k_t *guest_machine;        /* where the input pump posts */
static unsigned aros_windows, aros_created;

static struct aros_window *aros_find(uint32_t id)
{
    unsigned i;

    if (id == 0u)
        return NULL;
    for (i = 0; i < AROS_WINDOW_MAX; i++)
        if (aros_win[i].used && aros_win[i].id == id)
            return &aros_win[i];
    return NULL;
}

static struct aros_window *aros_free_slot(void)
{
    unsigned i;

    for (i = 0; i < AROS_WINDOW_MAX; i++)
        if (!aros_win[i].used)
            return &aros_win[i];
    return NULL;
}

static void aros_close(struct aros_window *w)
{
    if (w->win != NULL) {
        if (draw_win == w->win)
            draw_win = NULL;
        CloseWindow(w->win);
        aros_windows--;
    }
    memset(w, 0, sizeof *w);
}

/* ---------------------------------------------------------------- clipboard */

static struct MsgPort *clip_port;
static struct IOClipReq *clip_req;
static int clip_ready;
static uint8_t clip_buf[65536];

static void clip_open(void)
{
    clip_port = CreateMsgPort();
    if (clip_port == NULL)
        return;
    clip_req = (struct IOClipReq *)CreateIORequest(clip_port,
                                                  (ULONG)sizeof *clip_req);
    if (clip_req == NULL)
        return;
    if (OpenDevice((STRPTR)"clipboard.device", PRIMARY_CLIP,
                   (struct IORequest *)clip_req, 0) != 0) {
        Printf((STRPTR)"aros: no clipboard.device, scrap stays local\n");
        clip_req = NULL;
        return;
    }
    clip_ready = 1;
}

static void clip_close(void)
{
    if (clip_ready) {
        CloseDevice((struct IORequest *)clip_req);
        clip_ready = 0;
    }
    if (clip_req != NULL) {
        DeleteIORequest((struct IORequest *)clip_req);
        clip_req = NULL;
    }
    if (clip_port != NULL) {
        DeleteMsgPort(clip_port);
        clip_port = NULL;
    }
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* A clip is a FORM 'FTXT' holding a 'CHRS' chunk.  Anything else is not text
 * and the Scrap Manager reports no 'TEXT' flavor for it. */
static int32_t clip_text(uint8_t **text)
{
    uint32_t pos = 0u, size, id;

    while (pos + 8u <= sizeof clip_buf) {
        id = rd_be32(clip_buf + pos);
        size = rd_be32(clip_buf + pos + 4u);
        if (id == 0x464F524Du) {                    /* FORM */
            if (pos + 12u > sizeof clip_buf)
                return -1;
            if (rd_be32(clip_buf + pos + 8u) != 0x46545854u)  /* FTXT */
                return -1;
            pos += 12u;
            continue;
        }
        if (pos + 8u + size > sizeof clip_buf)
            return -1;
        if (id == 0x43485253u) {                    /* CHRS */
            *text = clip_buf + pos + 8u;
            return (int32_t)size;
        }
        pos += 8u + ((size + 1u) & ~1u);
    }
    return -1;
}

static void clip_put(void *ctx, const uint8_t *data, uint32_t len)
{
    uint32_t padded, total;

    (void)ctx;
    if (!clip_ready)
        return;
    if (len > sizeof clip_buf - 20u) {              /* the clip is not a file */
        len = (uint32_t)sizeof clip_buf - 20u;
        while (len > 0u && (data[len - 1u] & 0xC0u) == 0x80u)
            len--;                                  /* cut on a UTF-8 edge */
    }
    padded = (len + 1u) & ~1u;
    total = 20u + padded;
    memcpy(clip_buf, "FORM", 4u);
    be32(clip_buf + 4u, 4u + 8u + padded);
    memcpy(clip_buf + 8u, "FTXT", 4u);
    memcpy(clip_buf + 12u, "CHRS", 4u);
    be32(clip_buf + 16u, len);
    if (len > 0u)
        memcpy(clip_buf + 20u, data, len);
    clip_buf[20u + padded - 1u] = 0;
    clip_req->io_Command = CMD_WRITE;
    clip_req->io_Data = (STRPTR)clip_buf;
    clip_req->io_Length = total;
    clip_req->io_Offset = 0;
    clip_req->io_ClipID = 0;
    if (DoIO((struct IORequest *)clip_req) == 0) {
        clip_req->io_Command = CMD_UPDATE;
        DoIO((struct IORequest *)clip_req);
    }
}

static int32_t clip_length(void *ctx)
{
    uint8_t *text;
    int32_t size;

    (void)ctx;
    if (!clip_ready)
        return 0;
    clip_req->io_Command = CMD_READ;
    clip_req->io_Data = (STRPTR)clip_buf;
    clip_req->io_Length = (ULONG)sizeof clip_buf;
    clip_req->io_Offset = 0;
    clip_req->io_ClipID = 0;
    if (DoIO((struct IORequest *)clip_req) != 0)
        return 0;
    size = clip_text(&text);
    return size > 0 ? size : 0;
}

static int32_t clip_get(void *ctx, uint8_t *buf, uint32_t len)
{
    uint8_t *text;
    int32_t size;

    (void)ctx;
    if (!clip_ready)
        return -1;
    clip_req->io_Command = CMD_READ;
    clip_req->io_Data = (STRPTR)clip_buf;
    clip_req->io_Length = (ULONG)sizeof clip_buf;
    clip_req->io_Offset = 0;
    clip_req->io_ClipID = 0;
    if (DoIO((struct IORequest *)clip_req) != 0)
        return -1;
    size = clip_text(&text);
    if (size <= 0)
        return 0;
    if ((uint32_t)size > len)
        size = (int32_t)len;
    memcpy(buf, text, (size_t)size);
    return size;
}

static const qrx_m68k_clip_t clip_hooks = {
    NULL, clip_put, clip_get, clip_length
};

/* --------------------------------------------------------------- menu bar
 *
 * The MacOS menu bar is an Intuition menu strip on the screen, not a strip
 * drawn into a window: NewMenu/AppendMenu/InsertMenu/DeleteMenu keep a model of
 * the bar here, and every change rebuilds the strip with gadtools' CreateMenus
 * and attaches it with SetMenuStrip to each of our windows.  A pick from the
 * IDCMP port becomes menuEvt (menu ID and item in the message) followed by the
 * click that selected it, so a MacOS application sees the same three events an
 * Intuition application would produce. */

#define AROS_MENU_MAX 8u
#define AROS_MENU_ITEM_MAX 24u
#define AROS_MENU_TEXT 32u

struct aros_menu_item {
    char text[AROS_MENU_TEXT];
    int enabled;
    int checked;
    char cmd;                     /* MacOS Command-key equivalent, 0 = none */
    int16_t style;                /* MacOS style bits from SetItmStyle */
    int16_t icon;                 /* MacOS icon index from SetItmIcon */
    char mark;                    /* mark character from SetItemMark, 0 = none */
    struct TextAttr attr;         /* the styled font handed to IntuiText */
    struct TextAttr *attr_orig;   /* what the item used before we styled it */
    int attr_known;
    APTR fill_orig;               /* the IntuiText the item came with */
    UWORD flags_orig;
    int fill_known;
    APTR icon_raster;             /* bitmap with icon + text, ours to free */
    UWORD icon_width;
    struct BitMap icon_bm;
    struct Image icon_img;
};

struct aros_menu {
    uint32_t id;                  /* MacOS menu handle */
    int used;
    int16_t num;                  /* MacOS menu ID */
    int32_t x;                    /* where the BAR call put it */
    char title[AROS_MENU_TEXT];
    unsigned nitems;
    struct aros_menu_item item[AROS_MENU_ITEM_MAX];
};

static struct aros_menu aros_menus[AROS_MENU_MAX];
static struct aros_menu *aros_strip_model[AROS_MENU_MAX];  /* strip order */
static unsigned aros_strip_count;
static struct Menu *aros_strip;
static unsigned aros_menu_picks;
static int menu_test_done;
static int menu_test_reported;
static struct NewMenu aros_nm[AROS_MENU_MAX * (AROS_MENU_ITEM_MAX + 1u) + 1u];


static struct Screen *first_screen(void)
{
    unsigned i;

    for (i = 0u; i < AROS_WINDOW_MAX; i++)
        if (aros_win[i].used && aros_win[i].win != NULL)
            return aros_win[i].win->WScreen;
    return NULL;
}

static struct aros_menu *menu_by_handle(uint32_t id)
{
    unsigned i;

    if (id == 0u)
        return NULL;
    for (i = 0u; i < AROS_MENU_MAX; i++)
        if (aros_menus[i].used && aros_menus[i].id == id)
            return &aros_menus[i];
    return NULL;
}

static struct aros_menu *menu_free_slot(void)
{
    unsigned i;

    for (i = 0u; i < AROS_MENU_MAX; i++)
        if (!aros_menus[i].used)
            return &aros_menus[i];
    return NULL;
}

static void menu_copy(char *dst, const char *src, uint32_t len)
{
    if (len > AROS_MENU_TEXT - 1u)
        len = AROS_MENU_TEXT - 1u;
    if (src != NULL && len != 0u)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

/* A menu pick arrives as an Intuition menu number: five bits item, five bits
 * menu, both one based. */
static void menu_pick(UWORD number)
{
    struct aros_menu *mm;
    unsigned mi, it;

    if (guest_machine == NULL || number == MENUNULL || aros_strip_count == 0u)
        return;
    mi = (unsigned)((number >> 5) & 0x1Fu);
    it = (unsigned)(number & 0x1Fu);
    if (mi == 0u || mi > aros_strip_count || it == 0u)
        return;
    mm = aros_strip_model[mi - 1u];
    if (mm == NULL || it > mm->nitems)
        return;
    Printf((STRPTR)"aros: menu pick \"%s\" item %ld (menu %ld, event 15)\n",
           (LONG)mm->title, (LONG)it, (LONG)mm->num);
    (void)qrx_m68k_post_event(guest_machine, QRX_M68K_EVENT_MENU,
                              (((uint32_t)(uint16_t)mm->num) << 16) | it, 8,
                              mm->x + 4, 0u);
    (void)qrx_m68k_post_event(guest_machine, 1u, (uint32_t)(uint16_t)mm->num, 8,
                              mm->x + 4, 0u);
    (void)qrx_m68k_post_event(guest_machine, 2u, (uint32_t)(uint16_t)mm->num, 8,
                              mm->x + 4, 0u);
    aros_menu_picks++;
}

/* A headless run has nobody to click a menu.  When the marker file
 * C:QRX68K_MENU_TEST exists the guest picks the first item of the first menu
 * once, so the whole path from the strip through menu_pick into the event
 * queue is exercised.  (A variable would be nicer than a file, but GetVar
 * blocks in this guest when the variable is unset.) */
static int menu_test_requested(void)
{
    BPTR f = Open((STRPTR)"C:QRX68K_MENU_TEST", MODE_OLDFILE);
    int on = 0;

    if (f != 0) {
        Close(f);
        on = 1;
    }
    return on;
}

/* Pick the first item of the first menu once, through the same path IDCMP
 * uses. */
static void menu_self_test(void)
{
    struct Window *win = NULL;
    struct Menu *m;
    UWORD number = 0u;
    unsigned i, mi = 0u;

    if (menu_test_done || aros_strip == NULL || aros_strip_count == 0u)
        return;
    if (!menu_test_requested()) {
        if (!menu_test_reported) {
            menu_test_reported = 1;
            Printf((STRPTR)"aros: no menu test marker\n");
        }
        return;
    }
    if (!menu_test_reported) {
        menu_test_reported = 1;
        Printf((STRPTR)"aros: menu test marker seen\n");
    }
    for (i = 0u; i < AROS_WINDOW_MAX; i++)
        if (aros_win[i].used && aros_win[i].win != NULL) {
            win = aros_win[i].win;
            break;
        }
    if (win == NULL || win->MenuStrip != aros_strip) {
        return;
    }
    for (m = aros_strip; m != NULL && mi < aros_strip_count;
         m = m->NextMenu, mi++)
        if (m->FirstItem != NULL) {
            number = (UWORD)(((mi + 1u) << 5) | 1u);
            break;
        }
    if (number == 0u)
        return;
    menu_test_done = 1;
    menu_pick(number);
}

/* The live strip entry of a model, so a flag or a text can be changed in
 * place instead of building the whole menu again. */
static struct Menu *menu_strip_for(struct aros_menu *mm)
{
    struct Menu *m = aros_strip;
    unsigned i;

    for (i = 0u; m != NULL && i < aros_strip_count; i++, m = m->NextMenu)
        if (aros_strip_model[i] == mm)
            return m;
    return NULL;
}

static struct MenuItem *menu_live_item(struct Menu *m, unsigned item)
{
    struct MenuItem *mi = m != NULL ? m->FirstItem : NULL;
    unsigned j = 0u;

    while (mi != NULL && j < item) {
        mi = mi->NextItem;
        j++;
    }
    return j == item ? mi : NULL;
}

/* CheckItem, EnableItem and DisableItem are flags on the item that is already
 * there; nothing about the strip's shape changes. */
static struct MenuItem *menu_patch_state(struct aros_menu *mm, unsigned item)
{
    struct Menu *m = menu_strip_for(mm);
    struct MenuItem *mi;

    if (m == NULL || item >= mm->nitems)
        return NULL;
    mi = menu_live_item(m, item);
    if (mi == NULL)
        return NULL;
    if (mm->item[item].enabled)
        mi->Flags |= ITEMENABLED;
    else
        mi->Flags = (UWORD)(mi->Flags & ~ITEMENABLED);
    if (mm->item[item].checked || mm->item[item].mark != 0)
        mi->Flags |= CHECKED;
    else
        mi->Flags = (UWORD)(mi->Flags & ~CHECKED);
    return mi;
}

/* SetItemCmd writes the Command-key field of the live item; Intuition draws it
 * only while COMMSEQ is set. */
static struct MenuItem *menu_patch_cmd(struct aros_menu *mm, unsigned item)
{
    struct Menu *m = menu_strip_for(mm);
    struct MenuItem *mi;

    if (m == NULL || item >= mm->nitems)
        return NULL;
    mi = menu_live_item(m, item);
    if (mi == NULL)
        return NULL;
    if (mm->item[item].cmd != 0) {
        mi->Command = (BYTE)mm->item[item].cmd;
        mi->Flags |= COMMSEQ;
    } else {
        mi->Flags = (UWORD)(mi->Flags & ~COMMSEQ);
    }
    return mi;
}

/* MacOS style bits to the ones a TextAttr carries; outline, shadow, condense
 * and extend have no Amiga counterpart and are dropped. */
static UWORD mac_style_bits(int16_t style)
{
    UWORD bits = 0u;

    if ((style & 0x01) != 0)
        bits |= FSF_BOLD;
    if ((style & 0x02) != 0)
        bits |= FSF_ITALIC;
    if ((style & 0x04) != 0)
        bits |= FSF_UNDERLINED;
    return bits;
}

/* SetItmStyle points the item's IntuiText at a TextAttr with the wanted style,
 * so Intuition draws the item in that font; clearing it puts the old one back. */
static struct MenuItem *menu_patch_style(struct aros_menu *mm, unsigned item)
{
    struct Menu *m = menu_strip_for(mm);
    struct aros_menu_item *it_model = &mm->item[item];
    struct MenuItem *mi;
    struct IntuiText *it;

    if (m == NULL || item >= mm->nitems)
        return NULL;
    mi = menu_live_item(m, item);
    if (mi == NULL || (mi->Flags & ITEMTEXT) == 0)
        return NULL;
    it = (struct IntuiText *)mi->ItemFill;
    if (it == NULL)
        return NULL;
    if (!it_model->attr_known) {
        it_model->attr_known = 1;
        it_model->attr_orig = it->ITextFont;
    }
    if (it_model->style == 0) {
        it->ITextFont = it_model->attr_orig;
        return mi;
    }
    if (it_model->attr_orig != NULL && it_model->attr_orig->ta_Name != NULL) {
        it_model->attr.ta_Name = it_model->attr_orig->ta_Name;
        it_model->attr.ta_YSize = it_model->attr_orig->ta_YSize;
        it_model->attr.ta_Flags = it_model->attr_orig->ta_Flags;
    } else if (GfxBase != NULL && GfxBase->DefaultFont != NULL) {
        it_model->attr.ta_Name =
            (STRPTR)GfxBase->DefaultFont->tf_Message.mn_Node.ln_Name;
        it_model->attr.ta_YSize = GfxBase->DefaultFont->tf_YSize;
        it_model->attr.ta_Flags = 0;
    } else {
        return NULL;
    }
    it_model->attr.ta_Style = (UBYTE)mac_style_bits(it_model->style);
    it->ITextFont = &it_model->attr;
    return mi;
}

/* Intuition renders a menu item from exactly one fill: either an IntuiText or
 * an Image, and only objects of the same type may be chained. A mixed chain of
 * image and text is therefore not possible, and a second menu item would put
 * the icon on its own row. So the guest draws icon and text into one small
 * bitmap and hands that over as the item's image (ITEMTEXT cleared). The Mac
 * icon art ('ICON', 'SICN') does not exist here, so the index is drawn as a
 * digit. */
#define AROS_ICON_W 8
#define AROS_ICON_H 7
#define AROS_ICON_GAP 2
#define AROS_ICON_ROW 8

static struct Image aros_icon_img[10];
static UWORD aros_icon_data[10][AROS_ICON_H];
static int aros_icons_ready;

static const unsigned char aros_digit[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}
};

static void aros_icon_init(void)
{
    unsigned d, r, x;

    if (aros_icons_ready)
        return;
    for (d = 0u; d < 10u; d++) {
        for (r = 0u; r < AROS_ICON_H; r++)
            aros_icon_data[d][r] = 0u;
        for (r = 0u; r < 5u; r++) {
            UWORD bits = 0u;

            for (x = 0u; x < 3u; x++)
                if (((aros_digit[d][r] >> (2u - x)) & 1u) != 0u)
                    bits |= (UWORD)(0x8000u >> (x + 2u));
            aros_icon_data[d][r + 1u] = bits;
        }
        aros_icon_img[d].LeftEdge = 0;
        aros_icon_img[d].TopEdge = 0;
        aros_icon_img[d].Width = AROS_ICON_W;
        aros_icon_img[d].Height = AROS_ICON_H;
        aros_icon_img[d].Depth = 1;
        aros_icon_img[d].ImageData = aros_icon_data[d];
        aros_icon_img[d].PlanePick = 1;
        aros_icon_img[d].PlaneOnOff = 0;
        aros_icon_img[d].NextImage = NULL;
    }
    aros_icons_ready = 1;
}

static void menu_free_icon(struct aros_menu_item *im)
{
    if (im->icon_raster != NULL) {
        FreeRaster((PLANEPTR)im->icon_raster, im->icon_width, AROS_ICON_ROW);
        im->icon_raster = NULL;
    }
}

/* Draw the digit and the item text side by side into one bitmap. */
static int menu_render_icon(struct aros_menu_item *im)
{
    struct RastPort rp;
    WORD text_w, width;
    unsigned len = (unsigned)strlen(im->text);

    if (GfxBase == NULL || GfxBase->DefaultFont == NULL)
        return 0;
    InitRastPort(&rp);
    SetFont(&rp, GfxBase->DefaultFont);
    text_w = TextLength(&rp, (STRPTR)im->text, (long)len);
    width = (WORD)(AROS_ICON_W + AROS_ICON_GAP + text_w + 2);
    if (width < 12)
        width = 12;
    menu_free_icon(im);
    im->icon_raster = (APTR)AllocRaster((UWORD)width, AROS_ICON_ROW);
    if (im->icon_raster == NULL)
        return 0;
    im->icon_width = (UWORD)width;
    InitBitMap(&im->icon_bm, 1, width, AROS_ICON_ROW);
    im->icon_bm.Planes[0] = (PLANEPTR)im->icon_raster;
    InitRastPort(&rp);
    rp.BitMap = &im->icon_bm;
    SetAPen(&rp, 1);
    SetFont(&rp, GfxBase->DefaultFont);
    aros_icon_init();
    if (im->icon >= 1 && im->icon <= 9)
        DrawImage(&rp, &aros_icon_img[im->icon], 0, 0);
    Move(&rp, AROS_ICON_W + AROS_ICON_GAP, (WORD)rp.TxBaseline);
    Text(&rp, (STRPTR)im->text, (long)len);
    im->icon_img.LeftEdge = 0;
    im->icon_img.TopEdge = 0;
    im->icon_img.Width = width;
    im->icon_img.Height = AROS_ICON_ROW;
    im->icon_img.Depth = 1;
    im->icon_img.ImageData = (UWORD *)im->icon_bm.Planes[0];
    im->icon_img.PlanePick = 1;
    im->icon_img.PlaneOnOff = 0;
    im->icon_img.NextImage = NULL;
    return 1;
}

/* How many pixels the text part of the combined bitmap holds; the icon sits in
 * the first byte, so counting from byte 2 can only find text. */
static unsigned icon_text_pixels(struct aros_menu_item *im)
{
    PLANEPTR plane = (PLANEPTR)im->icon_bm.Planes[0];
    unsigned per_row = (unsigned)im->icon_bm.BytesPerRow;
    unsigned row, byte, bits = 0u;

    for (row = 0u; row < AROS_ICON_ROW; row++) {
        for (byte = 2u; byte < per_row; byte++) {
            unsigned char v = plane[row * per_row + byte];

            while (v != 0u) {
                bits += v & 1u;
                v = (unsigned char)(v >> 1);
            }
        }
    }
    return bits;
}

static struct MenuItem *menu_patch_icon(struct aros_menu *mm, unsigned item)
{
    struct Menu *m = menu_strip_for(mm);
    struct aros_menu_item *im = &mm->item[item];
    struct MenuItem *mi;

    if (m == NULL || item >= mm->nitems)
        return NULL;
    mi = menu_live_item(m, item);
    if (mi == NULL)
        return NULL;
    if (!im->fill_known) {
        im->fill_known = 1;
        im->fill_orig = mi->ItemFill;
        im->flags_orig = mi->Flags;
    }
    if (im->icon < 1 || im->icon > 9) {
        menu_free_icon(im);
        mi->ItemFill = im->fill_orig;
        if ((im->flags_orig & ITEMTEXT) != 0)
            mi->Flags |= ITEMTEXT;
        else
            mi->Flags = (UWORD)(mi->Flags & ~ITEMTEXT);
        return mi;
    }
    if (!menu_render_icon(im))
        return NULL;
    mi->Flags = (UWORD)(mi->Flags & ~ITEMTEXT);
    mi->ItemFill = (APTR)&im->icon_img;
    return mi;
}

/* SetItemMark writes the mark column, which Intuition only knows as the check
 * mark; anything but 0 shows up there as a check. */
static struct MenuItem *menu_patch_mark(struct aros_menu *mm, unsigned item)
{
    struct Menu *m = menu_strip_for(mm);
    struct MenuItem *mi;

    if (m == NULL || item >= mm->nitems)
        return NULL;
    mi = menu_live_item(m, item);
    if (mi == NULL)
        return NULL;
    if (mm->item[item].mark != 0 || mm->item[item].checked)
        mi->Flags |= CHECKED;
    else
        mi->Flags = (UWORD)(mi->Flags & ~CHECKED);
    return mi;
}

/* SetMenuItemText only swaps the IntuiText the item points at. */
static int menu_patch_text(struct aros_menu *mm, unsigned item)
{
    struct Menu *m = menu_strip_for(mm);
    struct MenuItem *mi;
    struct IntuiText *it;

    if (m == NULL || item >= mm->nitems)
        return 0;
    mi = menu_live_item(m, item);
    if (mi == NULL || (mi->Flags & ITEMTEXT) == 0)
        return 0;
    it = (struct IntuiText *)mi->ItemFill;
    if (it == NULL)
        return 0;
    it->IText = (STRPTR)mm->item[item].text;
    return 1;
}

/* After a patch Intuition has to draw the bar again; the menus themselves stay
 * where they are. */
static void menu_redraw(void)
{
    unsigned i;

    if (aros_strip == NULL)
        return;
    for (i = 0u; i < AROS_WINDOW_MAX; i++)
        if (aros_win[i].used && aros_win[i].win != NULL)
            (void)ResetMenuStrip(aros_win[i].win, aros_strip);
}

/* Rebuild the strip from the model and attach it to every Mac window. */
static void menu_rebuild(void)
{
    unsigned i, j, n = 0u;

    if (IntuitionBase == NULL || GadToolsBase == NULL)
        return;
    for (i = 0u; i < AROS_WINDOW_MAX; i++)
        if (aros_win[i].used && aros_win[i].win != NULL)
            ClearMenuStrip(aros_win[i].win);
    /* Icon bitmaps are ours, not gadtools'; free them before the items go. */
    for (i = 0u; i < AROS_MENU_MAX; i++)
        for (j = 0u; j < aros_menus[i].nitems; j++)
            menu_free_icon(&aros_menus[i].item[j]);
    if (aros_strip != NULL) {
        FreeMenus(aros_strip);
        aros_strip = NULL;
    }
    aros_strip_count = 0u;
    for (i = 0u; i < AROS_MENU_MAX; i++) {
        struct aros_menu *mm = &aros_menus[i];

        if (!mm->used)
            continue;
        aros_strip_model[aros_strip_count++] = mm;
        aros_nm[n].nm_Type = NM_TITLE;
        aros_nm[n].nm_Label = (CONST_STRPTR)mm->title;
        aros_nm[n].nm_CommKey = NULL;
        aros_nm[n].nm_Flags = 0u;
        aros_nm[n].nm_MutualExclude = 0;
        aros_nm[n].nm_UserData = (APTR)(IPTR)(((uint32_t)(uint16_t)mm->num) << 16);
        n++;
        for (j = 0u; j < mm->nitems; j++) {
            aros_nm[n].nm_Type = NM_ITEM;
            aros_nm[n].nm_Label = (CONST_STRPTR)mm->item[j].text;
            aros_nm[n].nm_CommKey = NULL;
            aros_nm[n].nm_Flags = CHECKIT |
                (mm->item[j].enabled ? 0u : NM_ITEMDISABLED) |
                (mm->item[j].checked ? CHECKED : 0u);
            aros_nm[n].nm_MutualExclude = 0;
            aros_nm[n].nm_UserData =
                (APTR)(IPTR)((((uint32_t)(uint16_t)mm->num) << 16) | (j + 1u));
            n++;
        }
    }
    if (n == 0u)
        return;
    aros_nm[n].nm_Type = NM_END;
    aros_nm[n].nm_Label = NULL;
    aros_nm[n].nm_CommKey = NULL;
    aros_nm[n].nm_Flags = 0u;
    aros_nm[n].nm_MutualExclude = 0;
    aros_nm[n].nm_UserData = NULL;
    aros_strip = CreateMenus(aros_nm, TAG_DONE);
    if (aros_strip == NULL) {
        Printf((STRPTR)"aros: CreateMenus refused the menu bar\n");
        aros_strip_count = 0u;
        return;
    }
    {
        struct Screen *scr = first_screen();
        struct VisualInfo *vi = scr != NULL
            ? (struct VisualInfo *)GetVisualInfoA((APTR)scr, NULL) : NULL;

        if (vi == NULL || !LayoutMenus(aros_strip, vi, TAG_DONE))
            Printf((STRPTR)"aros: LayoutMenus failed\n");
        if (vi != NULL)
            FreeVisualInfo(vi);
    }
    /* gadtools built the items without Command keys, so put the ones the model
     * already knows onto the fresh strip. */
    for (i = 0u; i < aros_strip_count; i++) {
        struct aros_menu *mm = aros_strip_model[i];
        unsigned j;

        for (j = 0u; mm != NULL && j < mm->nitems; j++) {
            /* The fresh strip has new IntuiTexts, so forget what the old ones
             * looked like before we touched them. */
            mm->item[j].attr_known = 0;
            mm->item[j].fill_known = 0;
            if (mm->item[j].cmd != 0)
                (void)menu_patch_cmd(mm, j);
            if (mm->item[j].style != 0)
                (void)menu_patch_style(mm, j);
            if (mm->item[j].mark != 0)
                (void)menu_patch_mark(mm, j);
            if (mm->item[j].icon != 0)
                (void)menu_patch_icon(mm, j);
        }
    }
    for (i = 0u; i < AROS_WINDOW_MAX; i++)
        if (aros_win[i].used && aros_win[i].win != NULL) {
            if (!SetMenuStrip(aros_win[i].win, aros_strip))
                Printf((STRPTR)"aros: SetMenuStrip refused\n");
        }
    Printf((STRPTR)"aros: menu bar %ld menu(s), %ld item(s) attached\n",
           (LONG)aros_strip_count, (LONG)(n - aros_strip_count));
    menu_self_test();
}

/* ------------------------------------------------------------------ windows */

static void gfx_window(void *ctx, int op, int32_t top, int32_t left,
                       int32_t bottom, int32_t right, const char *title,
                       uint32_t len, uint32_t id);

/* The IDCMP port of every window becomes a MacOS event: buttons and keys go
 * straight through, the close gadget arrives as a click in its corner and
 * activation as activateEvt. */
static void gfx_poll(void *ctx)
{
    struct IntuiMessage *msg;
    struct aros_window *w;
    unsigned i;

    (void)ctx;
    if (IntuitionBase == NULL || guest_machine == NULL)
        return;
    for (i = 0; i < AROS_WINDOW_MAX; i++) {
        w = &aros_win[i];
        if (!w->used || w->win == NULL)
            continue;
        while ((msg = (struct IntuiMessage *)GetMsg(w->win->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            WORD mx = msg->MouseX, my = msg->MouseY;
            uint16_t what = 0u, mods = 0u;
            uint32_t message = 0u;

            ReplyMsg((struct Message *)msg);
            if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(w->win);
                SetAPen(w->win->RPort, 1);
                EndRefresh(w->win, TRUE);
                continue;
            }
            if (cls == IDCMP_MENUPICK) {
                menu_pick(code);
                continue;
            }
            if (cls == IDCMP_CLOSEWINDOW) {
                what = 1u;                      /* mouseDown */
                mx = 7;
                my = -8;                        /* the go-away box */
            } else if (cls == IDCMP_MOUSEBUTTONS) {
                what = (code == SELECTDOWN || code == MENUDOWN) ? 1u : 2u;
            } else if (cls == IDCMP_VANILLAKEY) {
                what = 3u;                      /* keyDown */
                message = code;
                if (my < 0)
                    my = 0;
            } else if (cls == IDCMP_ACTIVEWINDOW) {
                what = 8u;                      /* activateEvt */
                mods = 1u;                      /* active */
            } else if (cls == IDCMP_INACTIVEWINDOW) {
                what = 8u;
                mods = 0u;
            }
            if (what != 0u)
                (void)qrx_m68k_post_event(guest_machine, what, message, my, mx,
                                          mods);
        }
    }
}

/* SetPort names the window QuickDraw draws into. */
static void gfx_target(void *ctx, uint32_t id)
{
    struct aros_window *w = aros_find(id);

    (void)ctx;
    draw_win = (w != NULL && w->win != NULL && !w->hidden) ? w->win : NULL;
}

/* QuickDraw output.  The drawing calls are printed next to the trap trace and
 * go into the RastPort of the window the pen belongs to. */
static const char *rect_op_name(int op)
{
    switch (op) {
    case QRX_M68K_RECT_FILL: return "fill";
    case QRX_M68K_RECT_FRAME: return "frame";
    case QRX_M68K_RECT_PAINT: return "paint";
    case QRX_M68K_RECT_ERASE: return "erase";
    case QRX_M68K_RECT_INVERT: return "invert";
    default: return "?";
    }
}

static void gfx_rect(void *ctx, int32_t top, int32_t left, int32_t bottom,
                     int32_t right, int op, const uint8_t *pattern)
{
    (void)ctx;
    (void)pattern;
    Printf((STRPTR)"qd rect %s %ld,%ld,%ld,%ld\n", (LONG)rect_op_name(op),
           (LONG)top, (LONG)left, (LONG)bottom, (LONG)right);
    if (draw_win == NULL)
        return;
    SetAPen(draw_win->RPort, 1);
    if (op == QRX_M68K_RECT_FILL || op == QRX_M68K_RECT_PAINT ||
        op == QRX_M68K_RECT_ERASE) {
        RectFill(draw_win->RPort, (WORD)left, (WORD)top, (WORD)(right - 1),
                 (WORD)(bottom - 1));
    } else {
        Move(draw_win->RPort, (WORD)left, (WORD)top);
        Draw(draw_win->RPort, (WORD)(right - 1), (WORD)top);
        Draw(draw_win->RPort, (WORD)(right - 1), (WORD)(bottom - 1));
        Draw(draw_win->RPort, (WORD)left, (WORD)(bottom - 1));
        Draw(draw_win->RPort, (WORD)left, (WORD)top);
    }
}

static void gfx_line(void *ctx, int32_t from_y, int32_t from_x, int32_t to_y,
                     int32_t to_x, uint16_t mode, uint16_t width,
                     uint16_t height)
{
    (void)ctx;
    Printf((STRPTR)"qd line %ld,%ld -> %ld,%ld pen %ldx%ld mode %ld\n",
           (LONG)from_x, (LONG)from_y, (LONG)to_x, (LONG)to_y, (LONG)width,
           (LONG)height, (LONG)mode);
    if (draw_win == NULL)
        return;
    SetAPen(draw_win->RPort, 1);
    Move(draw_win->RPort, (WORD)from_x, (WORD)from_y);
    Draw(draw_win->RPort, (WORD)to_x, (WORD)to_y);
}

static void gfx_text(void *ctx, int32_t y, int32_t x, const char *text,
                     uint32_t len, uint16_t font, uint16_t size, uint16_t face)
{
    char buf[80];

    (void)ctx;
    if (len > sizeof buf - 1u)
        len = (uint32_t)sizeof buf - 1u;
    memcpy(buf, text, len);
    buf[len] = '\0';
    Printf((STRPTR)"qd text %ld,%ld \"%s\" font %ld/%ld face %ld\n", (LONG)x,
           (LONG)y, (LONG)buf, (LONG)font, (LONG)size, (LONG)face);
    if (draw_win == NULL)
        return;
    SetAPen(draw_win->RPort, 1);
    Move(draw_win->RPort, (WORD)x, (WORD)y);
    Text(draw_win->RPort, (CONST_STRPTR)buf, (LONG)len);
}

static const char *win_op_name(int op)
{
    switch (op) {
    case QRX_M68K_WIN_CREATE: return "new";
    case QRX_M68K_WIN_SHOW: return "show";
    case QRX_M68K_WIN_HIDE: return "hide";
    case QRX_M68K_WIN_SELECT: return "select";
    case QRX_M68K_WIN_MOVE: return "move";
    case QRX_M68K_WIN_DISPOSE: return "dispose";
    case QRX_M68K_WIN_UPDATE_BEGIN: return "update-begin";
    case QRX_M68K_WIN_UPDATE_END: return "update-end";
    case QRX_M68K_WIN_INVAL: return "inval";
    case QRX_M68K_WIN_TITLE_CHANGED: return "title";
    case QRX_M68K_WIN_HILITE: return "hilite";
    default: return "?";
    }
}

static const char *menu_op_name(int op)
{
    switch (op) {
    case QRX_M68K_MENU_NEW: return "new";
    case QRX_M68K_MENU_ITEM: return "item";
    case QRX_M68K_MENU_DELETE_ITEM: return "delete-item";
    case QRX_M68K_MENU_BAR: return "bar";
    case QRX_M68K_MENU_DISPOSE: return "dispose";
    case QRX_M68K_MENU_ITEM_TEXT: return "item-text";
    case QRX_M68K_MENU_ITEM_STATE: return "item-state";
    case QRX_M68K_MENU_HILITE: return "hilite";
    case QRX_M68K_MENU_ITEM_CMD: return "item-cmd";
    case QRX_M68K_MENU_ITEM_STYLE: return "item-style";
    case QRX_M68K_MENU_ITEM_ICON: return "item-icon";
    case QRX_M68K_MENU_ITEM_MARK: return "item-mark";
    default: return "?";
    }
}

static void gfx_window(void *ctx, int op, int32_t top, int32_t left,
                       int32_t bottom, int32_t right, const char *title,
                       uint32_t len, uint32_t id)
{
    char buf[40];
    struct aros_window *w;

    (void)ctx;
    if (len > sizeof buf - 1u)
        len = (uint32_t)sizeof buf - 1u;
    memcpy(buf, title, len);
    buf[len] = '\0';
    Printf((STRPTR)"win %s %ld,%ld,%ld,%ld \"%s\"\n", (LONG)win_op_name(op),
           (LONG)top, (LONG)left, (LONG)bottom, (LONG)right, (LONG)buf);
    if (IntuitionBase == NULL)
        return;
    switch (op) {
    case QRX_M68K_WIN_CREATE:
        w = aros_free_slot();
        if (w == NULL) {
            Printf((STRPTR)"aros: no free window slot\n");
            break;
        }
        w->win = OpenWindowTags(NULL,
            WA_Title, (IPTR)buf,
            WA_Left, (WORD)left, WA_Top, (WORD)top,
            WA_Width, (WORD)(right - left), WA_Height, (WORD)(bottom - top),
            WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                      WFLG_ACTIVATE | WFLG_SMART_REFRESH,
            WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_VANILLAKEY |
                      IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MENUPICK |
                      IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW,
            TAG_DONE);
        if (w->win == NULL) {
            Printf((STRPTR)"aros: window \"%s\" was refused\n", (LONG)buf);
            break;
        }
        w->id = id;
        w->used = 1;
        w->hidden = 0;
        draw_win = w->win;
        SetRast(w->win->RPort, 0);
        /* A window that appears after DrawMenuBar still belongs to the bar,
         * so the strip built earlier goes onto it as well. */
        if (aros_strip != NULL) {
            if (SetMenuStrip(w->win, aros_strip))
                Printf((STRPTR)"aros: menu strip attached to \"%s\"\n",
                       (LONG)buf);
            else
                Printf((STRPTR)"aros: SetMenuStrip refused for \"%s\"\n",
                       (LONG)buf);
        }
        aros_windows++;
        aros_created++;
        Printf((STRPTR)"aros: window \"%s\" on the public screen\n", (LONG)buf);
        break;
    case QRX_M68K_WIN_DISPOSE:
        if ((w = aros_find(id)) != NULL)
            aros_close(w);
        break;
    case QRX_M68K_WIN_SHOW:
        if ((w = aros_find(id)) == NULL)
            break;
        if (w->hidden) {
            w->hidden = 0;
            SetRast(w->win->RPort, 0);
        }
        draw_win = w->win;
        break;
    case QRX_M68K_WIN_HIDE:
        if ((w = aros_find(id)) != NULL) {
            w->hidden = 1;
            if (draw_win == w->win)
                draw_win = NULL;
        }
        break;
    case QRX_M68K_WIN_SELECT:
        if ((w = aros_find(id)) != NULL) {
            WindowToFront(w->win);
            ActivateWindow(w->win);
            w->hidden = 0;
            draw_win = w->win;
        }
        break;
    case QRX_M68K_WIN_MOVE:
        if ((w = aros_find(id)) != NULL)
            MoveWindow(w->win, (WORD)(left - w->win->LeftEdge),
                       (WORD)(top - w->win->TopEdge));
        break;
    case QRX_M68K_WIN_TITLE_CHANGED:
        if ((w = aros_find(id)) != NULL)
            SetWindowTitles(w->win, (STRPTR)buf, (STRPTR)-1);
        break;
    case QRX_M68K_WIN_HILITE:
        if ((w = aros_find(id)) != NULL)
            ActivateWindow(w->win);
        break;
    default:
        break;
    }
}

static int16_t menu_hilite;   /* title HiliteMenu last asked for, 0 = none */

static void gfx_menu(void *ctx, int op, const char *title, uint32_t title_len,
                     int32_t item, const char *text, uint32_t text_len,
                     int32_t value, uint32_t id)
{
    char t[40], s[40];
    struct aros_menu *mm;
    unsigned i;

    (void)ctx;
    if (title_len > sizeof t - 1u)
        title_len = (uint32_t)sizeof t - 1u;
    memcpy(t, title, title_len);
    t[title_len] = '\0';
    s[0] = '\0';
    if (text != 0) {
        if (text_len > sizeof s - 1u)
            text_len = (uint32_t)sizeof s - 1u;
        memcpy(s, text, text_len);
        s[text_len] = '\0';
    }
    Printf((STRPTR)"menu %s \"%s\" item %ld \"%s\" value %ld\n",
           (LONG)menu_op_name(op), (LONG)t, (LONG)item, (LONG)s, (LONG)value);
    /* Intuition highlights a menu title only while the strip is being tracked,
     * so HiliteMenu is a state the model keeps rather than something to draw. */
    if (op == QRX_M68K_MENU_HILITE) {
        menu_hilite = (int16_t)value;
        if (value == 0)
            Printf((STRPTR)"aros: menu highlight cleared (HiliteMenu 0)\n");
        else
            Printf((STRPTR)"aros: menu %ld hilited (\"%s\")\n", (LONG)value,
                   (LONG)t);
        return;
    }
    /* Text and flags of an existing item are patched into the live strip and
     * redrawn; only a new or removed item rebuilds the menus. */
    if (op == QRX_M68K_MENU_ITEM_TEXT || op == QRX_M68K_MENU_ITEM_STATE ||
        op == QRX_M68K_MENU_ITEM_CMD || op == QRX_M68K_MENU_ITEM_STYLE ||
        op == QRX_M68K_MENU_ITEM_ICON || op == QRX_M68K_MENU_ITEM_MARK) {
        struct MenuItem *mi;

        mm = menu_by_handle(id);
        if (mm == NULL || item < 0 || (unsigned)item >= mm->nitems)
            return;
        if (op == QRX_M68K_MENU_ITEM_STYLE) {
            mm->item[item].style = (int16_t)value;
            mi = menu_patch_style(mm, (unsigned)item);
            if (mi == NULL)
                Printf((STRPTR)"aros: item %ld style not patchable\n",
                       (LONG)item);
            else if (mm->item[item].style == 0)
                Printf((STRPTR)"aros: item %ld style cleared\n", (LONG)item);
            else
                Printf((STRPTR)"aros: item %ld \"%s\" style %ld (patched, "
                       "font style 0x%lx)\n", (LONG)item,
                       (LONG)mm->item[item].text, (LONG)value,
                       (LONG)(unsigned)mac_style_bits((int16_t)value));
            menu_redraw();
            return;
        }
        if (op == QRX_M68K_MENU_ITEM_MARK) {
            mm->item[item].mark = (char)value;
            mi = menu_patch_mark(mm, (unsigned)item);
            if (mi == NULL)
                Printf((STRPTR)"aros: item %ld mark not patchable\n",
                       (LONG)item);
            else if (mm->item[item].mark == 0)
                Printf((STRPTR)"aros: item %ld mark cleared\n", (LONG)item);
            else
                Printf((STRPTR)"aros: item %ld \"%s\" mark '%lc' (patched as "
                       "check)\n", (LONG)item, (LONG)mm->item[item].text,
                       (LONG)(unsigned char)mm->item[item].mark);
            menu_redraw();
            return;
        }
        if (op == QRX_M68K_MENU_ITEM_ICON) {
            mm->item[item].icon = (int16_t)value;
            mi = menu_patch_icon(mm, (unsigned)item);
            if (mi == NULL) {
                Printf((STRPTR)"aros: item %ld icon not patchable\n",
                       (LONG)item);
                return;
            }
            if (mm->item[item].icon == 0)
                Printf((STRPTR)"aros: item %ld icon cleared, text again\n",
                       (LONG)item);
            else
                Printf((STRPTR)"aros: item %ld \"%s\" icon %ld (digit plus text"
                       " in a %ldx%ld image, %ld text pixels, ITEMTEXT %ld)\n",
                       (LONG)item, (LONG)mm->item[item].text, (LONG)value,
                       (LONG)((struct Image *)mi->ItemFill)->Width,
                       (LONG)((struct Image *)mi->ItemFill)->Height,
                       (LONG)icon_text_pixels(&mm->item[item]),
                       (LONG)((mi->Flags & ITEMTEXT) ? 1 : 0));
            menu_redraw();
            return;
        }
        if (op == QRX_M68K_MENU_ITEM_CMD) {
            mm->item[item].cmd = (char)value;
            mi = menu_patch_cmd(mm, (unsigned)item);
            if (mi == NULL)
                Printf((STRPTR)"aros: item %ld command key not patchable\n",
                       (LONG)item);
            else if ((mi->Flags & COMMSEQ) != 0)
                Printf((STRPTR)"aros: item %ld \"%s\" command key '%lc'"
                       " (patched)\n", (LONG)item,
                       (LONG)mm->item[item].text, (LONG)mi->Command);
            else
                Printf((STRPTR)"aros: item %ld command key cleared\n",
                       (LONG)item);
            menu_redraw();
            return;
        }
        if (op == QRX_M68K_MENU_ITEM_TEXT) {
            menu_copy(mm->item[item].text, s, (uint32_t)strlen(s));
            if (mm->item[item].icon >= 1 && mm->item[item].icon <= 9) {
                if (menu_render_icon(&mm->item[item]))
                    Printf((STRPTR)"aros: item %ld text \"%s\" redrawn with"
                           " icon %ld\n", (LONG)item,
                           (LONG)mm->item[item].text,
                           (LONG)mm->item[item].icon);
                else
                    Printf((STRPTR)"aros: item %ld text not drawable with"
                           " icon\n", (LONG)item);
                menu_redraw();
                return;
            }
            if (menu_patch_text(mm, (unsigned)item))
                Printf((STRPTR)"aros: item %ld text \"%s\" (patched)\n",
                       (LONG)item, (LONG)mm->item[item].text);
            else
                Printf((STRPTR)"aros: item %ld text not patchable\n",
                       (LONG)item);
        } else {
            mm->item[item].enabled = (value & 1) != 0;
            mm->item[item].checked = (value & 2) != 0;
            mi = menu_patch_state(mm, (unsigned)item);
            if (mi == NULL) {
                Printf((STRPTR)"aros: item %ld state not patchable\n",
                       (LONG)item);
            } else {
                Printf((STRPTR)"aros: item %ld \"%s\" checked %ld enabled %ld"
                       " (patched, %s)\n", (LONG)item,
                       (LONG)mm->item[item].text,
                       (LONG)((mi->Flags & CHECKED) ? 1 : 0),
                       (LONG)((mi->Flags & ITEMENABLED) ? 1 : 0),
                       (LONG)((mi->Flags & CHECKIT) ? "check column" : "no check"));
            }
        }
        menu_redraw();
        return;
    }
    switch (op) {
    case QRX_M68K_MENU_NEW:
        if ((mm = menu_by_handle(id)) == NULL)
            mm = menu_free_slot();
        if (mm == NULL) {
            Printf((STRPTR)"aros: no free menu slot\n");
            break;
        }
        memset(mm, 0, sizeof *mm);
        mm->used = 1;
        mm->id = id;
        mm->num = (int16_t)value;
        menu_copy(mm->title, t, (uint32_t)strlen(t));
        break;
    case QRX_M68K_MENU_ITEM:
        mm = menu_by_handle(id);
        if (mm == NULL || item < 0 || (unsigned)item >= AROS_MENU_ITEM_MAX)
            break;
        menu_copy(mm->item[item].text, s, (uint32_t)strlen(s));
        mm->item[item].enabled = value != 0;
        if ((unsigned)item >= mm->nitems)
            mm->nitems = (unsigned)item + 1u;
        break;
    case QRX_M68K_MENU_DELETE_ITEM:
        mm = menu_by_handle(id);
        if (mm == NULL || item < 0 || (unsigned)item >= mm->nitems)
            break;
        /* The removed item takes its bitmap with it. */
        if (mm->item[item].icon_raster != NULL) {
            menu_free_icon(&mm->item[item]);
            Printf((STRPTR)"aros: item %ld deleted, icon bitmap freed\n",
                   (LONG)item);
        }
        for (i = (unsigned)item; i + 1u < mm->nitems; i++)
            mm->item[i] = mm->item[i + 1u];
        if (mm->nitems != 0u)
            mm->nitems--;
        /* The slot the shift vacated still holds a copy of the last item,
         * including its bitmap pointer; clear it so nothing is freed twice. */
        if (mm->nitems < AROS_MENU_ITEM_MAX)
            memset(&mm->item[mm->nitems], 0, sizeof mm->item[0]);
        break;
    case QRX_M68K_MENU_BAR:
        if ((mm = menu_by_handle(id)) != NULL)
            mm->x = value;
        break;
    case QRX_M68K_MENU_DISPOSE:
        if ((mm = menu_by_handle(id)) != NULL) {
            unsigned freed = 0u, j;

            for (j = 0u; j < mm->nitems; j++)
                if (mm->item[j].icon_raster != NULL) {
                    menu_free_icon(&mm->item[j]);
                    freed++;
                }
            Printf((STRPTR)"aros: menu \"%s\" disposed, %ld icon bitmap(s) "
                   "freed\n", (LONG)mm->title, (LONG)freed);
            memset(mm, 0, sizeof *mm);
        }
        break;
    default:
        break;
    }
    menu_rebuild();
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint8_t *slurp(const char *path, uint32_t *len_out)
{
    BPTR f;
    int32_t len;
    uint8_t *buf;
    int32_t got = 0;

    f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f)
        return 0;
    if (Seek(f, 0, OFFSET_END) < 0) {
        Close(f);
        return 0;
    }
    len = Seek(f, 0, OFFSET_BEGINNING);
    if (len <= 0) {
        Close(f);
        return 0;
    }
    buf = (uint8_t *)AllocVec((uint32_t)len, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) {
        Close(f);
        return 0;
    }
    while (got < len) {
        int32_t n = Read(f, buf + got, (uint32_t)(len - got));
        if (n <= 0)
            break;
        got += n;
    }
    Close(f);
    if (got != len) {
        FreeVec(buf);
        return 0;
    }
    *len_out = (uint32_t)len;
    return buf;
}

/* Wrap a data and resource fork into a MacBinary image (version 0, no CRC). */
static uint8_t *wrap_macbinary(const uint8_t *data, uint32_t dlen,
                               const uint8_t *rsrc, uint32_t rlen,
                               const char *name, uint32_t *out_len)
{
    uint32_t pad_d = ((dlen + 127u) / 128u) * 128u;
    uint32_t pad_r = ((rlen + 127u) / 128u) * 128u;
    uint32_t total = QRX_MACBIN_HEADER + pad_d + pad_r;
    uint8_t *img = (uint8_t *)AllocVec(total, MEMF_PUBLIC | MEMF_CLEAR);
    unsigned len = 0u;
    const char *base = name;
    const char *p;

    if (!img)
        return 0;
    for (p = name; *p != '\0'; p++)
        if (*p == ':' || *p == '/')
            base = p + 1;
    while (base[len] != '\0' && len < QRX_MACBIN_NAME_MAX)
        len++;
    img[0] = 0u;
    img[1] = (uint8_t)len;
    memcpy(img + 2u, base, len);
    put32(img + 65u, 0x4150504Cu);          /* 'APPL' */
    put32(img + 69u, 0x71525841u);          /* 'qRXA' */
    put32(img + 83u, dlen);
    put32(img + 87u, rlen);
    memcpy(img + QRX_MACBIN_HEADER, data, dlen);
    if (rlen != 0u)
        memcpy(img + QRX_MACBIN_HEADER + pad_d, rsrc, rlen);
    *out_len = total;
    return img;
}

int main(int argc, char **argv)
{
    uint32_t dlen = 0u, rlen = 0u, ilen = 0u, mem_limit = 0u;
    uint8_t *data, *rsrc = 0, *img;
    static qrx_m68k_t m;
    struct dos_fs fsctx;
    qrx_m68k_fs_t fs;
    qrx_m68k_gfx_t gfx;
    qrx_m68k_status_t st;
    unsigned i;

    if (argc < 2 || argc > 5) {
        PutStr((STRPTR)"Usage: QRX68KRun <application> [resource-fork] [mem-kib] "
                        "[host-root]\n");
        return 20;
    }
    if (argc == 4) {
        uint32_t kib = (uint32_t)strtoul(argv[3], 0, 0);
        if (kib >= 256u && kib <= 0x003FFFFFu)   /* 256 KiB .. 4 GiB - 1 */
            mem_limit = kib * 1024u;
    }
    data = slurp(argv[1], &dlen);
    if (!data) {
        Printf((STRPTR)"QRX68KRun: cannot read %s\n", (LONG)argv[1]);
        return 20;
    }
    if (argc >= 3 && argv[2][0] != '\0') {
        rsrc = slurp(argv[2], &rlen);
        if (!rsrc) {
            Printf((STRPTR)"QRX68KRun: cannot read %s\n", (LONG)argv[2]);
            FreeVec(data);
            return 20;
        }
        img = wrap_macbinary(data, dlen, rsrc, rlen, argv[1], &ilen);
        FreeVec(data);
        FreeVec(rsrc);
        if (!img) {
            PutStr((STRPTR)"QRX68KRun: out of memory\n");
            return 20;
        }
    } else {
        img = data;
        ilen = dlen;
    }

    memset(&fsctx, 0, sizeof fsctx);
    memset(&fs, 0, sizeof fs);
    if (argc >= 5)
        strncpy(fsctx.root, argv[4], sizeof fsctx.root - 1u);
    fs.ctx = &fsctx;
    fs.open = dosfs_open;
    fs.read = dosfs_read;
    fs.write = dosfs_write;
    fs.seek = dosfs_seek;
    fs.close = dosfs_close;

    st = qrx_m68k_load_limited(&m, img, ilen, mem_limit);
    if (st != QRX_M68K_OK) {
        Printf((STRPTR)"QRX68KRun: %s\n", (LONG)qrx_m68k_status_name(st));
        if (m.detail[0] != '\0')
            Printf((STRPTR)"QRX68KRun: %s\n", (LONG)m.detail);
        FreeVec(img);
        if (st == QRX_M68K_ERR_NO_CODE)
            return 3;
        if (st == QRX_M68K_ERR_PACKED)
            return 4;
        return 1;
    }
    Printf((STRPTR)"QRX68KRun: %ld segment(s), A5=0x%08lx, entry=0x%08lx, %ld KiB\n",
           (LONG)m.code_count, (LONG)m.a5, (LONG)m.entry,
           (LONG)(m.mem_size / 1024u));
    Printf((STRPTR)"QRX68KRun: MemTop=0x%08lx, zone=0x%08lx, heap end=0x%08lx\n",
           (LONG)m.mem_size, (LONG)m.heap, (LONG)m.heap_end);
    if (fsctx.root[0] != '\0')
        Printf((STRPTR)"QRX68KRun: files below %s\n", (LONG)fsctx.root);
    qrx_m68k_set_fs(&m, &fs);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39);
    if (GadToolsBase == NULL)
        Printf((STRPTR)"QRX68KRun: no gadtools, the menu bar stays empty\n");
    if (IntuitionBase == NULL || GfxBase == NULL) {
        Printf((STRPTR)"QRX68KRun: no Intuition, windows stay log-only\n");
        IntuitionBase = NULL;
    }
    clip_open();
    memset(&gfx, 0, sizeof gfx);
    gfx.rect = gfx_rect;
    gfx.line = gfx_line;
    gfx.text = gfx_text;
    gfx.window = gfx_window;
    gfx.menu = gfx_menu;
    gfx.poll = gfx_poll;
    gfx.target = gfx_target;
    qrx_m68k_set_gfx(&m, &gfx);
    qrx_m68k_set_clip(&m, &clip_hooks);
    guest_machine = &m;
    FreeVec(img);

    st = qrx_m68k_start(&m, APP_CYCLES);
    {
        unsigned i;

        for (i = 0; i < AROS_WINDOW_MAX; i++)
            if (aros_win[i].used)
                aros_close(&aros_win[i]);
        if (aros_strip != NULL) {
            FreeMenus(aros_strip);
            aros_strip = NULL;
            aros_strip_count = 0u;
        }
        clip_close();
        if (aros_menu_picks != 0u)
            Printf((STRPTR)"aros: %ld menu pick(s)\n", (LONG)aros_menu_picks);
        if (aros_created != 0u)
            Printf((STRPTR)"aros: %ld window(s) created, %ld left open\n",
                   (LONG)aros_created, (LONG)aros_windows);
        if (GadToolsBase != NULL) {
            CloseLibrary(GadToolsBase);
            GadToolsBase = NULL;
        }
        if (IntuitionBase != NULL) {
            CloseLibrary((struct Library *)GfxBase);
            CloseLibrary((struct Library *)IntuitionBase);
            IntuitionBase = NULL;
        }
    }
    for (i = 0; i < m.ntrace; i++) {
        Printf((STRPTR)"trap 0x%04lX pc=0x%06lX d0=0x%08lx a0=0x%08lx -> 0x%08lx %s\n",
               (LONG)(0xA000u | m.trace[i].trap), (LONG)m.trace[i].pc,
               (LONG)m.trace[i].d0, (LONG)m.trace[i].a0,
               (LONG)m.trace[i].result,
               (LONG)(m.trace[i].implemented ? "ok" : "MISSING"));
    }
    if (st == QRX_M68K_UNIMPLEMENTED) {
        Printf((STRPTR)"QRX68KRun: stopped at trap 0x%04lX after %ld trap(s), %ld implemented\n",
               (LONG)(0xA000u | m.stop_trap), (LONG)m.ntrace, (LONG)m.nimplemented);
        Printf((STRPTR)"QRX68KRun: first missing trap 0x%04lX\n",
               (LONG)(0xA000u | m.stop_trap));
        return 2;
    }
    Printf((STRPTR)"QRX68KRun: %s (%s), %ld trap(s), %ld implemented\n",
           (LONG)qrx_m68k_status_name(st), (LONG)m.detail,
           (LONG)m.ntrace, (LONG)m.nimplemented);
    return st == QRX_M68K_EXITED ? 0 : 1;
}
