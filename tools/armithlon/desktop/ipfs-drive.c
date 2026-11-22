/*
 * ipfs-drive.c - IPFS actions inside QRX Drive.
 *
 * This file is included by the generated desktop source (build-desktop.py)
 * after qrx_icons.c, so it can use the desktop's file manager state and
 * drawing helpers directly.  It deliberately shells out to the small
 * QRXIPFSPub/QRXVolImage binaries instead of linking the network stack
 * into the desktop process.
 */

#include <dos/var.h>
#include <stdlib.h>

#define QRX_IPFS_BTN_W      70
#define QRX_IPFS_MENU_W    236
#define QRX_IPFS_MENU_ROW   22
#define QRX_IPFS_PIN_MAP    "IPFS:.qrx-node/path-pins"
#define QRX_IPFS_STATUS     "IPFS:.qrx-node/p2p-status"
#define QRX_IPFS_HASH_MAX   96

static int g_qrx_ipfs_menu = 0;
static int g_qrx_ipfs_confirm = 0;
static char g_qrx_ipfs_target[FILE_PATH_LEN + 160];

static int g_qrx_ipfs_hash_active = 0;
static char g_qrx_ipfs_hash[QRX_IPFS_HASH_MAX];
static int g_qrx_ipfs_hash_len = 0;

static void qrx_ipfs_drive_draw(struct RastPort *rp);
static int qrx_ipfs_drive_click(WORD x, WORD y);

static int qrx_ipfs_in_volume(void)
{
    return !strncmp(g_files_path, "IPFS:", 5);
}

static int qrx_ipfs_hash_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
}

static void qrx_ipfs_status_read(int *online, int *peers)
{
    BPTR f;
    char line[192];

    *online = 0;
    *peers = 0;
    f = Open((CONST_STRPTR)QRX_IPFS_STATUS, MODE_OLDFILE);
    if (!f) return;
    while (FGets(f, line, sizeof(line))) {
        if (!strncmp(line, "online=", 7))
            *online = atoi(line + 7) != 0;
        else if (!strncmp(line, "peers=", 6))
            *peers = atoi(line + 6);
    }
    Close(f);
}

static void qrx_ipfs_draw_dot(struct RastPort *rp, int x, int y, UBYTE pen)
{
    fill(rp, pen, x, y, x + 6, y + 6);
    outline(rp, PEN_EDGE, x, y, x + 6, y + 6);
}

static void qrx_ipfs_draw_pin(struct RastPort *rp, int x, int y)
{
    fill(rp, PEN_GREEN, x + 2, y + 1, x + 8, y + 5);
    outline(rp, PEN_EDGE, x + 2, y + 1, x + 8, y + 5);
    fill(rp, PEN_WHITE, x + 4, y + 2, x + 6, y + 3);
    fill(rp, PEN_GREEN, x + 5, y + 6, x + 6, y + 10);
}

static void qrx_ipfs_pins_load(void)
{
    BPTR f;
    char line[640];
    int i;

    if (!qrx_ipfs_in_volume()) return;
    for (i = 0; i < g_file_count; ++i) {
        g_file_entries[i].ipfs_pinned = 0;
        g_file_entries[i].ipfs_cid[0] = 0;
    }
    f = Open((CONST_STRPTR)QRX_IPFS_PIN_MAP, MODE_OLDFILE);
    if (!f) return;
    while (FGets(f, line, sizeof(line))) {
        char *tab;
        char *mapped;
        char sign;
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (line[0] != '+' && line[0] != '-') continue;
        tab = strchr(line, '\t');
        if (!tab) continue;
        sign = line[0];
        *tab = 0;
        mapped = tab + 1;
        for (i = 0; i < g_file_count; ++i) {
            char full[FILE_PATH_LEN + 160];
            qrx_join_path(full, sizeof(full), g_files_path,
                          g_file_entries[i].name);
            if (qrx_ci_cmp(full, mapped)) continue;
            if (sign == '+') {
                strncpy(g_file_entries[i].ipfs_cid, line + 1,
                        sizeof(g_file_entries[i].ipfs_cid) - 1);
                g_file_entries[i].ipfs_cid[
                    sizeof(g_file_entries[i].ipfs_cid) - 1] = 0;
                g_file_entries[i].ipfs_pinned = 1;
            } else {
                g_file_entries[i].ipfs_pinned = 0;
                g_file_entries[i].ipfs_cid[0] = 0;
            }
        }
    }
    Close(f);
}

static void qrx_ipfs_draw_pins(struct RastPort *rp)
{
    int visible, row, idx;

    if (!qrx_ipfs_in_volume()) return;
    visible = qrx_file_visible_count();
    if (visible < 1) visible = 1;

    if (g_file_icon_view) {
        int cols = (g_files->Width - FILE_LEFT_W - 20) / FILE_GRID_W;
        if (cols < 1) cols = 1;
        for (row = 0; row < visible; ++row) {
            WORD x, y;
            idx = g_file_scroll + row;
            if (idx >= g_file_count) break;
            if (!g_file_entries[idx].ipfs_pinned) continue;
            x = FILE_LEFT_W + 8 + (row % cols) * FILE_GRID_W;
            y = FILE_LIST_TOP + (row / cols) * FILE_GRID_H;
            qrx_ipfs_draw_pin(rp, x + FILE_GRID_W - 19, y + 5);
        }
    } else {
        WORD y = FILE_LIST_TOP;
        for (row = 0; row < visible; ++row) {
            idx = g_file_scroll + row;
            if (idx >= g_file_count) break;
            if (g_file_entries[idx].ipfs_pinned)
                qrx_ipfs_draw_pin(rp, g_files->Width - 116, y + 9);
            y += FILE_ROW_H;
        }
    }
}

static void qrx_ipfs_draw_status(struct RastPort *rp)
{
    char msg[180];
    int online, peers;

    if (!qrx_ipfs_in_volume()) return;
    qrx_ipfs_status_read(&online, &peers);
    fill(rp, PEN_PANEL, 8, 101, g_files->Width - 10, 111);
    if (online) {
        qrx_ipfs_draw_dot(rp, 14, 103, PEN_GREEN);
        snprintf(msg, sizeof(msg),
                 "IPFS BEREIT | P2P: %d | INTERNET ERFORDERLICH", peers);
        text_at(rp, PEN_TEXT, 26, 110, msg);
    } else {
        qrx_ipfs_draw_dot(rp, 14, 103, PEN_RED);
        text_at(rp, PEN_RED, 26, 110,
                "IPFS OFFLINE | P2P-KNOTEN ODER INTERNET PRUEFEN");
    }
}

static int qrx_ipfs_drive_hit_button(WORD x, WORD y)
{
    WORD w;
    if (!g_files) return 0;
    w = g_files->Width;
    return x >= w - QRX_IPFS_BTN_W - 38 && x <= w - 38 &&
           y >= 5 && y <= 23;
}

static int qrx_ipfs_cid_button_hit(WORD x, WORD y)
{
    WORD w;
    if (!g_files || !qrx_ipfs_in_volume()) return 0;
    w = g_files->Width;
    if (w < 460) return 0;
    return x >= 402 && x <= w - 12 && y >= 64 && y <= 84;
}

static int qrx_ipfs_menu_rows(void)
{
    return qrx_ipfs_in_volume() ? 8 : 4;
}

static void qrx_ipfs_menu_box(WORD *mx, WORD *my, WORD *mh)
{
    WORD w = g_files ? g_files->Width : 0;
    WORD bx = w - QRX_IPFS_BTN_W - 38;
    *mx = bx - 150;
    *my = 26;
    if (*mx < 4) *mx = 4;
    if (*mx + QRX_IPFS_MENU_W > w - 4) *mx = w - 4 - QRX_IPFS_MENU_W;
    *mh = (WORD)(qrx_ipfs_menu_rows() * QRX_IPFS_MENU_ROW + 8);
}

static void qrx_ipfs_selected_path(char *dest, size_t cap, int force_drawer)
{
    dest[0] = 0;
    if (!force_drawer && g_file_selected >= 0 && g_file_selected < g_file_count) {
        qrx_join_path(dest, cap, g_files_path, g_file_entries[g_file_selected].name);
    } else {
        snprintf(dest, cap, "%s", g_files_path);
    }
}

static int qrx_ipfs_start_cmd(const char *cmd, const char *okmsg)
{
    if (SystemTags((CONST_STRPTR)cmd, SYS_Asynch, TRUE, TAG_DONE) != -1) {
        qrx_set_status(okmsg);
        return 1;
    }
    qrx_set_status("IPFS: Start von QRXIPFSPub fehlgeschlagen.");
    return 0;
}

static void qrx_ipfs_launch_publish(const char *path)
{
    char cmd[FILE_PATH_LEN + 240];
    char msg[220];

    if (!path || !*path) {
        qrx_set_status("IPFS: Kein Objekt ausgewaehlt.");
        return;
    }
    snprintf(cmd, sizeof(cmd),
             "C:QRXIPFSPub ADD \"%s\" >T:QRX-IPFS-last.txt", path);
    snprintf(msg, sizeof(msg),
             "IPFS: %s wird oeffentlich gemacht - Meldung in T:QRX-IPFS-last.txt",
             path);
    qrx_ipfs_start_cmd(cmd, msg);
}

static void qrx_ipfs_launch_volume(void)
{
    char path[FILE_PATH_LEN + 160];

    qrx_ipfs_selected_path(path, sizeof(path), 0);
    SetVar((CONST_STRPTR)"QRX_VOL_SOURCE", (CONST_STRPTR)path, -1, GVF_GLOBAL_ONLY);
    if (SystemTags((CONST_STRPTR)"C:QRXVolImage", SYS_Asynch, TRUE, TAG_DONE) == -1)
        qrx_set_status("IPFS: QRXVolImage nicht startbar.");
    else
        qrx_set_status("Volume Image: Benutzer und Passwort im Fenster setzen.");
}

static void qrx_ipfs_begin_hash(void)
{
    g_qrx_ipfs_menu = 0;
    g_qrx_ipfs_confirm = 0;
    g_qrx_ipfs_hash_active = 1;
    g_qrx_ipfs_hash[0] = 0;
    g_qrx_ipfs_hash_len = 0;
    qrx_set_status("IPFS: CID eingeben, Enter laedt in den aktuellen Ordner.");
}

static int qrx_ipfs_hash_valid(const char *hash)
{
    int i, n;
    if (!hash) return 0;
    n = (int)strlen(hash);
    if (n < 8 || n >= QRX_IPFS_HASH_MAX) return 0;
    for (i = 0; i < n; ++i)
        if (!qrx_ipfs_hash_char(hash[i])) return 0;
    return 1;
}

static void qrx_ipfs_start_download(void)
{
    char target[FILE_PATH_LEN + 160];
    char name[128];
    char cmd[FILE_PATH_LEN + 330];
    char msg[260];
    int i;

    if (!qrx_ipfs_hash_valid(g_qrx_ipfs_hash)) {
        qrx_set_status("IPFS: Gueltigen CID eingeben (8-95 Zeichen).");
        return;
    }
    snprintf(name, sizeof(name), "%s", g_qrx_ipfs_hash);
    for (i = 2; i < 10; ++i) {
        qrx_join_path(target, sizeof(target), g_files_path, name);
        if (!qrx_path_exists(target)) break;
        snprintf(name, sizeof(name), "%s-%d", g_qrx_ipfs_hash, i);
    }
    if (qrx_path_exists(target)) {
        qrx_set_status("IPFS: Kein freier Zielname im aktuellen Ordner.");
        return;
    }
    snprintf(cmd, sizeof(cmd),
             "C:QRXIPFSPub GETPIN \"%s\" \"%s\" >T:QRX-IPFS-get.txt",
             g_qrx_ipfs_hash, target);
    g_qrx_ipfs_hash_active = 0;
    snprintf(msg, sizeof(msg), "IPFS: %s wird geladen und gepinnt.",
             g_qrx_ipfs_hash);
    qrx_ipfs_start_cmd(cmd, msg);
}

static int qrx_ipfs_text_active(void)
{
    return g_qrx_ipfs_hash_active;
}

static void qrx_ipfs_text_key(int code)
{
    if (!g_qrx_ipfs_hash_active) return;
    if (code == 13 || code == 10) {
        qrx_ipfs_start_download();
    } else if (code == 27) {
        g_qrx_ipfs_hash_active = 0;
        qrx_set_status("IPFS: CID-Eingabe abgebrochen.");
    } else if (code == 8 || code == 127) {
        if (g_qrx_ipfs_hash_len > 0)
            g_qrx_ipfs_hash[--g_qrx_ipfs_hash_len] = 0;
    } else if (code >= 32 && code <= 126 &&
               g_qrx_ipfs_hash_len < QRX_IPFS_HASH_MAX - 1 &&
               qrx_ipfs_hash_char(code)) {
        g_qrx_ipfs_hash[g_qrx_ipfs_hash_len++] = (char)code;
        g_qrx_ipfs_hash[g_qrx_ipfs_hash_len] = 0;
    }
}

static void qrx_ipfs_pin_selected(void)
{
    struct QRXFileEntry *e;
    char path[FILE_PATH_LEN + 160];
    char cmd[FILE_PATH_LEN + 300];
    char msg[220];

    if (g_file_selected < 0 || g_file_selected >= g_file_count) {
        qrx_set_status("IPFS: Objekt zum Pinnen auswaehlen.");
        return;
    }
    e = &g_file_entries[g_file_selected];
    qrx_join_path(path, sizeof(path), g_files_path, e->name);
    if (e->ipfs_pinned) {
        qrx_set_status("IPFS: Objekt ist bereits gepinnt.");
        return;
    }
    if (e->ipfs_cid[0]) {
        snprintf(cmd, sizeof(cmd),
                 "C:QRXIPFSPub PINCID \"%s\" \"%s\" >T:QRX-IPFS-pin.txt",
                 e->ipfs_cid, path);
        snprintf(msg, sizeof(msg), "IPFS: Pin fuer %s wird gesetzt.", e->name);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "C:QRXIPFSPub PINPATH \"%s\" >T:QRX-IPFS-pin.txt", path);
        snprintf(msg, sizeof(msg), "IPFS: %s wird veroeffentlicht und gepinnt.",
                 e->name);
    }
    qrx_ipfs_start_cmd(cmd, msg);
}

static void qrx_ipfs_unpin_selected(void)
{
    struct QRXFileEntry *e;
    char path[FILE_PATH_LEN + 160];
    char cmd[FILE_PATH_LEN + 300];
    char msg[220];

    if (g_file_selected < 0 || g_file_selected >= g_file_count) {
        qrx_set_status("IPFS: Objekt zum Unpinnen auswaehlen.");
        return;
    }
    e = &g_file_entries[g_file_selected];
    if (!e->ipfs_pinned) {
        qrx_set_status("IPFS: Objekt ist nicht gepinnt.");
        return;
    }
    qrx_join_path(path, sizeof(path), g_files_path, e->name);
    snprintf(cmd, sizeof(cmd),
             "C:QRXIPFSPub UNPINPATH \"%s\" >T:QRX-IPFS-pin.txt", path);
    snprintf(msg, sizeof(msg), "IPFS: Pin fuer %s wird entfernt.", e->name);
    qrx_ipfs_start_cmd(cmd, msg);
}

static void qrx_ipfs_open_settings(void)
{
    if (SystemTags((CONST_STRPTR)"SYS:QRX/C/QRXIPFSPrefs",
                   SYS_Asynch, TRUE, TAG_DONE) == -1)
        qrx_set_status("IPFS: QRXIPFSPrefs nicht startbar.");
    else
        qrx_set_status("IPFS-Einstellungen geoeffnet.");
}

static void qrx_ipfs_execute(int item)
{
    char path[FILE_PATH_LEN + 160];

    switch (item) {
    case 0:
        qrx_ipfs_selected_path(path, sizeof(path), 0);
        qrx_ipfs_launch_publish(path);
        break;
    case 1:
        qrx_ipfs_selected_path(path, sizeof(path), 1);
        qrx_ipfs_launch_publish(path);
        break;
    case 2:
        qrx_ipfs_launch_volume();
        break;
    case 4:
        qrx_ipfs_begin_hash();
        break;
    case 5:
        qrx_ipfs_pin_selected();
        break;
    case 6:
        qrx_ipfs_unpin_selected();
        break;
    case 7:
        qrx_ipfs_open_settings();
        break;
    default:
        if (SystemTags((CONST_STRPTR)"C:QRXIPFS", SYS_Asynch, TRUE, TAG_DONE) == -1)
            qrx_set_status("IPFS: QRXIPFS nicht startbar.");
        break;
    }
}

static const char *qrx_ipfs_menu_label(int row)
{
    switch (row) {
    case 0: return "In IPFS veroeffentlichen";
    case 1: return "Als Webseite veroeffentlichen";
    case 2: return "Verschluesseltes Volume...";
    case 3: return "IPFS Anwendung oeffnen";
    case 4: return "Datei per CID laden...";
    case 5: return "Objekt pinnen";
    case 6: return "Pin entfernen";
    default: return "IPFS-Einstellungen...";
    }
}

static void qrx_ipfs_dialog_box(WORD *cx, WORD *cy, WORD *dw)
{
    WORD w = g_files ? g_files->Width : 0;
    WORD h = g_files ? g_files->Height : 0;
    *dw = w - 28;
    if (*dw > 500) *dw = 500;
    if (*dw < 360) *dw = w - 12;
    *cx = (w - *dw) / 2;
    if (*cx < 6) *cx = 6;
    *cy = (h - 150) / 2;
    if (*cy < 34) *cy = 34;
}

static void qrx_ipfs_draw_hash_dialog(struct RastPort *rp)
{
    WORD cx, cy, dw;
    char field[QRX_IPFS_HASH_MAX + 8];
    const char *shown = g_qrx_ipfs_hash;
    int maxchars;

    qrx_ipfs_dialog_box(&cx, &cy, &dw);
    fill(rp, PEN_PANEL, cx, cy, cx + dw, cy + 150);
    outline(rp, PEN_ACCENT, cx, cy, cx + dw, cy + 150);
    text_at(rp, PEN_ACCENT, cx + 14, cy + 20, "DATEI PER IPFS-CID LADEN");
    text_at(rp, PEN_TEXT, cx + 14, cy + 40,
            "Ziel ist der aktuelle IPFS-Ordner. Internetverbindung erforderlich.");
    fill(rp, PEN_PANEL_2, cx + 14, cy + 54, cx + dw - 14, cy + 78);
    outline(rp, PEN_EDGE, cx + 14, cy + 54, cx + dw - 14, cy + 78);
    maxchars = (dw - 48) / 8;
    if (maxchars < 1) maxchars = 1;
    if (maxchars > 54) maxchars = 54;
    if (g_qrx_ipfs_hash_len > maxchars)
        shown = g_qrx_ipfs_hash + g_qrx_ipfs_hash_len - maxchars;
    snprintf(field, sizeof(field), "%s_", shown);
    text_at(rp, PEN_TEXT, cx + 20, cy + 72, field);
    fill(rp, PEN_ACCENT, cx + 14, cy + 104, cx + 154, cy + 128);
    outline(rp, PEN_EDGE, cx + 14, cy + 104, cx + 154, cy + 128);
    text_at(rp, PEN_BLACK, cx + 30, cy + 120, "LADEN");
    fill(rp, PEN_BUTTON, cx + 166, cy + 104, cx + 306, cy + 128);
    outline(rp, PEN_EDGE, cx + 166, cy + 104, cx + 306, cy + 128);
    text_at(rp, PEN_TEXT, cx + 196, cy + 120, "ABBRECHEN");
}

static void qrx_ipfs_drive_draw(struct RastPort *rp)
{
    WORD w, bx, by;
    int i;

    if (!g_files) return;
    w = g_files->Width;
    bx = w - QRX_IPFS_BTN_W - 38;
    by = 5;

    fill(rp, PEN_ACCENT, bx, by, bx + QRX_IPFS_BTN_W, by + 18);
    outline(rp, PEN_EDGE, bx, by, bx + QRX_IPFS_BTN_W, by + 18);
    text_at(rp, PEN_BLACK, bx + 8, by + 13, "IPFS");

    if (qrx_ipfs_in_volume()) {
        int online = 0, peers = 0;
        qrx_ipfs_status_read(&online, &peers);
        fill(rp, PEN_RED, 8, 87, w - 10, 99);
        text_at(rp, PEN_WHITE, 14, 96,
                "PUBLIC: Alles hier wird im Interplanetary File System veroeffentlicht.");
        qrx_ipfs_draw_status(rp);
        qrx_ipfs_draw_dot(rp, bx + 54, by + 6,
                          online ? PEN_GREEN : PEN_RED);
        (void)peers;

        if (w >= 460) {
            fill(rp, PEN_ACCENT, 402, 64, w - 12, 84);
            outline(rp, PEN_EDGE, 402, 64, w - 12, 84);
            text_at(rp, PEN_BLACK, 414, 79, "CID");
        }

        qrx_ipfs_pins_load();
        qrx_ipfs_draw_pins(rp);
    }

    if (g_qrx_ipfs_menu) {
        WORD mx, my, mh;
        int rows = qrx_ipfs_menu_rows();
        qrx_ipfs_menu_box(&mx, &my, &mh);
        fill(rp, PEN_PANEL, mx, my, mx + QRX_IPFS_MENU_W, my + mh);
        outline(rp, PEN_EDGE, mx, my, mx + QRX_IPFS_MENU_W, my + mh);
        for (i = 0; i < rows; ++i) {
            WORD ry = my + 4 + i * QRX_IPFS_MENU_ROW;
            fill(rp, i == 2 ? PEN_BUTTON : PEN_ACCENT, mx + 4, ry,
                 mx + QRX_IPFS_MENU_W - 4, ry + QRX_IPFS_MENU_ROW - 2);
            text_at(rp, i == 2 ? PEN_TEXT : PEN_BLACK, mx + 12, ry + 15,
                    qrx_ipfs_menu_label(i));
        }
    }

    if (g_qrx_ipfs_hash_active) {
        qrx_ipfs_draw_hash_dialog(rp);
    } else if (g_qrx_ipfs_confirm) {
        WORD cx = (w - 470) / 2, cy = (g_files->Height - 150) / 2;
        if (cx < 8) cx = 8;
        if (cy < 34) cy = 34;
        fill(rp, PEN_PANEL, cx, cy, cx + 470, cy + 150);
        outline(rp, PEN_RED, cx, cy, cx + 470, cy + 150);
        text_at(rp, PEN_RED, cx + 14, cy + 20, "DATEI IM INTERPLANETARY FILE SYSTEM VEROEFFENTLICHEN?");
        text_at(rp, PEN_TEXT, cx + 14, cy + 42, "Die Datei ist danach weltweit abrufbar und behaelt einen");
        text_at(rp, PEN_TEXT, cx + 14, cy + 56, "unveraenderlichen CID. Loeschen im lokalen Ordner nimmt");
        text_at(rp, PEN_TEXT, cx + 14, cy + 70, "fruehere Kopien nicht zurueck. Keine privaten Daten!");
        fill(rp, PEN_ACCENT, cx + 18, cy + 108, cx + 218, cy + 132);
        text_at(rp, PEN_BLACK, cx + 30, cy + 124, "JA, VEROEFFENTLICHEN");
        fill(rp, PEN_BUTTON, cx + 234, cy + 108, cx + 334, cy + 132);
        text_at(rp, PEN_TEXT, cx + 258, cy + 124, "ABBRECHEN");
    }
}

static int qrx_ipfs_drive_click(WORD x, WORD y)
{
    WORD w = g_files ? g_files->Width : 0;

    if (g_qrx_ipfs_hash_active) {
        WORD cx, cy, dw;
        qrx_ipfs_dialog_box(&cx, &cy, &dw);
        if (y >= cy + 104 && y <= cy + 128 &&
            x >= cx + 14 && x <= cx + 154) {
            qrx_ipfs_start_download();
        } else if (y >= cy + 104 && y <= cy + 128 &&
                   x >= cx + 166 && x <= cx + 306) {
            g_qrx_ipfs_hash_active = 0;
            qrx_set_status("IPFS: CID-Eingabe abgebrochen.");
        }
        redraw_filemanager();
        return 1;
    }

    if (g_qrx_ipfs_confirm) {
        WORD cx = (w - 470) / 2, cy = (g_files->Height - 150) / 2;
        if (cx < 8) cx = 8;
        if (cy < 34) cy = 34;
        if (y >= cy + 108 && y <= cy + 132 &&
            x >= cx + 18 && x <= cx + 218) {
            g_qrx_ipfs_confirm = 0;
            qrx_ipfs_launch_publish(g_qrx_ipfs_target);
        } else if (y >= cy + 108 && y <= cy + 132 &&
                   x >= cx + 234 && x <= cx + 334) {
            g_qrx_ipfs_confirm = 0;
            qrx_set_status("IPFS: Veroeffentlichung abgebrochen.");
        }
        redraw_filemanager();
        return 1;
    }

    if (g_qrx_ipfs_menu) {
        WORD mx, my, mh;
        int rows = qrx_ipfs_menu_rows();
        qrx_ipfs_menu_box(&mx, &my, &mh);
        if (x >= mx && x <= mx + QRX_IPFS_MENU_W && y >= my + 4 &&
            y <= my + 4 + rows * QRX_IPFS_MENU_ROW) {
            int row = (y - (my + 4)) / QRX_IPFS_MENU_ROW;
            g_qrx_ipfs_menu = 0;
            if (row == 0) {
                qrx_ipfs_selected_path(g_qrx_ipfs_target,
                                       sizeof(g_qrx_ipfs_target), 0);
                g_qrx_ipfs_confirm = 1;
                redraw_filemanager();
                return 1;
            }
            qrx_ipfs_execute(row);
        } else {
            g_qrx_ipfs_menu = 0;
        }
        redraw_filemanager();
        return 1;
    }

    if (qrx_ipfs_cid_button_hit(x, y)) {
        qrx_ipfs_begin_hash();
        redraw_filemanager();
        return 1;
    }

    if (qrx_ipfs_drive_hit_button(x, y)) {
        g_archive_menu = 0;
        g_qrx_ipfs_menu = !g_qrx_ipfs_menu;
        redraw_filemanager();
        return 1;
    }
    return 0;
}
