/* QRX OS One 22.5.12 - qrx-win32 headless backend: deterministic virtual clock,
 * input from a test script, PPM snapshots of all top-level windows.
 *
 * Script commands (one per line, '#' comments):
 *   wait MS            advance the virtual clock (timers fire, painting happens)
 *   click X Y [l|r|m]  mouse click at client coordinates of the active window
 *   down X Y [b] / up X Y [b] / move X Y
 *   clickid ID         click the centre of dialog control ID in the active window
 *   key VK [CHAR]      key press (VK hex like 0x0D or decimal), optional character
 *   type TEXT          type characters (digits, letters, + - * / . = etc.)
 *   menu ID            WM_COMMAND ID to the active window (menu selection)
 *   snap FILE.ppm      render all windows (relative to the output directory)
 *   text ID            print the text of control ID (0 = active window title)
 *   expect ID TEXT     compare the text of control ID, counted as OK / FAIL
 *   close              WM_CLOSE to the active window
 *   quit               post WM_QUIT
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "qrxwin.h"

typedef struct hl {
    FILE *script;
    uint64_t now;
    uint64_t pending;
    int line;
    char typebuf[256];
    int typepos, typeup;
} hl;

static hl g_hl;
static qevent *hl_pending_up;             /* second half of click / key */

int headless_set_script(qwin *w, const char *path)
{
    (void)w;
    g_hl.script = fopen(path, "r");
    return g_hl.script != NULL;
}

static int hl_init(qwin *w) { (void)w; g_hl.now = 1000; return 1; }
static void hl_create(qwin *w, qwnd *t) { (void)w; t->be = (void *)1; }
static void hl_destroy(qwin *w, qwnd *t) { (void)w; t->be = NULL; }
static void hl_present(qwin *w, qwnd *t) { (void)w; (void)t; }
static void hl_title(qwin *w, qwnd *t) { (void)w; (void)t; }
static void hl_menu(qwin *w, qwnd *t) { (void)w; (void)t; }
static uint64_t hl_now(qwin *w) { (void)w; return g_hl.now; }

static int vk_of_char(int ch)
{
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    if (ch >= 'A' && ch <= 'Z') return ch;
    if (ch >= '0' && ch <= '9') return ch;
    switch (ch) {
    case '+': return 0x6B; case '-': return 0x6D; case '*': return 0x6A; case '/': return 0x6F;
    case '.': return 0x6E; case ',': return 0xBC; case '=': return 0xBB; case '\n': return 0x0D;
    case ' ': return 0x20; case 8: return 0x08; case 27: return 0x1B;
    default: return 0;
    }
}

static int hl_wait(qwin *w, uint32_t timeout, qevent *ev)
{
    char line[512], *cmd, *rest;
    for (;;) {
        if (g_hl.pending) {
            uint64_t adv = timeout < g_hl.pending ? timeout : g_hl.pending;
            g_hl.now += adv;
            g_hl.pending -= adv;
            return 0;
        }
        if (g_hl.typebuf[g_hl.typepos]) {            /* typing in progress: down, then up */
            int ch = (unsigned char)g_hl.typebuf[g_hl.typepos];
            ev->vk = vk_of_char(ch);
            ev->ch = ch;
            if (!g_hl.typeup) { ev->type = QE_KEYDOWN; g_hl.typeup = 1; }
            else { ev->type = QE_KEYUP; g_hl.typeup = 0; g_hl.typepos++; g_hl.pending = 30; }
            return 1;
        }
        if (!g_hl.script || !fgets(line, sizeof(line), g_hl.script)) return -1;
        g_hl.line++;
        line[strcspn(line, "\r\n")] = 0;
        cmd = line;
        while (isspace((unsigned char)*cmd)) cmd++;
        if (!*cmd || *cmd == '#') continue;
        rest = cmd;
        while (*rest && !isspace((unsigned char)*rest)) rest++;
        if (*rest) *rest++ = 0;
        while (isspace((unsigned char)*rest)) rest++;
        memset(ev, 0, sizeof(*ev));
        if (!strcmp(cmd, "wait")) { g_hl.pending = strtoull(rest, NULL, 0); if (!g_hl.pending) g_hl.pending = 1; continue; }
        if (!strcmp(cmd, "click") || !strcmp(cmd, "down") || !strcmp(cmd, "up") || !strcmp(cmd, "move")) {
            char b = 'l';
            sscanf(rest, "%d %d %c", &ev->x, &ev->y, &b);
            ev->button = b == 'r' ? 2 : b == 'm' ? 3 : 1;
            if (!strcmp(cmd, "click")) {             /* button down now, button up on the next call */
                static qevent up;
                ev->type = QE_MOUSEDOWN;
                up = *ev;
                up.type = QE_MOUSEUP;
                hl_pending_up = &up;
                return 1;
            }
            ev->type = !strcmp(cmd, "down") ? QE_MOUSEDOWN : !strcmp(cmd, "up") ? QE_MOUSEUP : QE_MOUSEMOVE;
            return 1;
        }
        if (!strcmp(cmd, "clickid")) { ev->type = QE_CLICKID; ev->id = atoi(rest); return 1; }
        if (!strcmp(cmd, "key")) {
            char *e;
            ev->vk = (int)strtol(rest, &e, 0);
            while (isspace((unsigned char)*e)) e++;
            ev->ch = *e ? (unsigned char)*e : 0;
            ev->type = QE_KEYDOWN;
            {
                static qevent ku;
                ku = *ev; ku.type = QE_KEYUP;
                hl_pending_up = &ku;
            }
            return 1;
        }
        if (!strcmp(cmd, "type")) { snprintf(g_hl.typebuf, sizeof(g_hl.typebuf), "%s", rest); g_hl.typepos = 0; g_hl.typeup = 0; continue; }
        if (!strcmp(cmd, "menu")) { ev->type = QE_MENU; ev->id = (int)strtol(rest, NULL, 0); return 1; }
        if (!strcmp(cmd, "snap")) {
            ev->type = QE_SNAP;
            if (rest[0] == '/') snprintf(ev->arg, sizeof(ev->arg), "%s", rest);
            else snprintf(ev->arg, sizeof(ev->arg), "%s/%s", w->outdir[0] ? w->outdir : ".", rest);
            return 1;
        }
        if (!strcmp(cmd, "text")) { ev->type = QE_TEXT; ev->id = atoi(rest); return 1; }
        if (!strcmp(cmd, "expect")) {
            char *e;
            ev->type = QE_EXPECT;
            ev->id = (int)strtol(rest, &e, 0);
            while (isspace((unsigned char)*e)) e++;
            snprintf(ev->arg, sizeof(ev->arg), "%s", e);
            return 1;
        }
        if (!strcmp(cmd, "close")) { ev->type = QE_CLOSE; return 1; }
        if (!strcmp(cmd, "quit")) { ev->type = QE_QUIT; return 1; }
        qw_log(w, "script line %d: unknown command '%s'", g_hl.line, cmd);
    }
}

static int hl_wait_outer(qwin *w, uint32_t timeout, qevent *ev)
{
    if (hl_pending_up) { *ev = *hl_pending_up; hl_pending_up = NULL; return 1; }
    return hl_wait(w, timeout, ev);
}

static void hl_shutdown(qwin *w) { (void)w; if (g_hl.script) fclose(g_hl.script); g_hl.script = NULL; }

const qbackend qw_headless = { "headless", hl_init, hl_create, hl_destroy, hl_present, hl_title, hl_menu, hl_wait_outer, hl_now, hl_shutdown };
