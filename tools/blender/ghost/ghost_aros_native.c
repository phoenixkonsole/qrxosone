/* The Intuition side of GHOST for AROS: windows on the Workbench screen, IDCMP events, and
 * frames from the GL door put into a window with cybergraphics. */
#include "ghost_aros_native.h"
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/modeid.h>
#include <devices/inputevent.h>
#include <devices/rawkeycodes.h>
#include <devices/timer.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/keymap.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

struct QAWin {
	struct Window *win;
	void *user;
	char *title;
	struct QAWin *next;
};

static struct MsgPort *port;
static struct QAWin *windows;
static struct Screen *screen;
static int own_screen;

/* Blender needs room and true colour. A Workbench screen smaller than 1024x600 or with a palette gets
 * a screen of Blender's own: QRX_BLENDER_SCREEN=WxH, 1280x800 when unset. */
static struct Screen *open_own_screen(void)
{
	int w = 1280, h = 800;
	const char *want = getenv("QRX_BLENDER_SCREEN");
	if (want)
		sscanf(want, "%dx%d", &w, &h);
	ULONG id = BestCModeIDTags(CYBRBIDTG_NominalWidth, w, CYBRBIDTG_NominalHeight, h, CYBRBIDTG_Depth, 32, TAG_DONE);
	if (id == INVALID_ID)
		id = BestCModeIDTags(CYBRBIDTG_NominalWidth, w, CYBRBIDTG_NominalHeight, h, CYBRBIDTG_Depth, 24, TAG_DONE);
	if (id == INVALID_ID)
		return NULL;
	static UWORD pens[] = {(UWORD)~0};
	/* C:QRXHostInput places the host's pointer on the public screen "QRX", which the QRX desktop opens.
	 * Without the desktop this screen takes the name, so the pointer reaches Blender. */
	struct Screen *qrx = LockPubScreen("QRX");
	const char *name = qrx ? "Blender" : "QRX";
	if (qrx)
		UnlockPubScreen(NULL, qrx);
	struct Screen *s = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, w, SA_Height, h, SA_Depth, 24, SA_Title, (IPTR)"Blender",
	                                  SA_Type, PUBLICSCREEN, SA_PubName, (IPTR)name, SA_Pens, (IPTR)pens, SA_SharePens, TRUE,
	                                  SA_ShowTitle, TRUE, TAG_DONE);
	if (s)
		PubScreenStatus(s, 0);
	return s;
}

int qa_init(void)
{
	if (port)
		return 1;
	port = CreateMsgPort();
	screen = LockPubScreen(NULL);
	int small = !screen || screen->Width < 1024 || screen->Height < 600 ||
	            (CyberGfxBase && !GetCyberMapAttr(screen->RastPort.BitMap, CYBRMATTR_ISCYBERGFX)) ||
	            (CyberGfxBase && GetCyberMapAttr(screen->RastPort.BitMap, CYBRMATTR_DEPTH) < 15);
	if (small) {
		struct Screen *s = open_own_screen();
		if (s) {
			if (screen)
				UnlockPubScreen(NULL, screen);
			screen = s;
			own_screen = 1;
		}
	}
	return port && screen;
}

void qa_exit(void)
{
	while (windows)
		qa_close(windows);
	if (screen) {
		if (own_screen)
			CloseScreen(screen);
		else
			UnlockPubScreen(NULL, screen);
		screen = NULL;
	}
	if (port) {
		DeleteMsgPort(port);
		port = NULL;
	}
}

void qa_screen_size(int *w, int *h)
{
	*w = screen ? screen->Width : 640;
	*h = screen ? screen->Height : 480;
}

#define IDCMP_ALL (IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY | IDCMP_NEWSIZE | IDCMP_CLOSEWINDOW | \
                   IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_CHANGEWINDOW)

QAWin *qa_open(const char *title, int left, int top, int width, int height, void *user)
{
	if (!qa_init())
		return NULL;
	QAWin *w = calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->title = strdup(title ? title : "Blender");
	w->user = user;
	if (left < 0) left = 0;
	if (top < 0) top = screen->BarHeight + 1;
	struct Window *win = OpenWindowTags(NULL,
		WA_Left, left, WA_Top, top, WA_InnerWidth, width, WA_InnerHeight, height,
		WA_Title, (IPTR)w->title, own_screen ? WA_CustomScreen : WA_PubScreen, (IPTR)screen,
		WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE, WA_SizeBBottom, TRUE,
		WA_MinWidth, 320, WA_MinHeight, 200, WA_MaxWidth, -1, WA_MaxHeight, -1,
		WA_SmartRefresh, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE, WA_Activate, TRUE,
		WA_IDCMP, 0, TAG_DONE);
	if (!win) {
		free(w->title);
		free(w);
		return NULL;
	}
	/* One port for every window: the usual way to share it, then the events we want. */
	win->UserPort = port;
	ModifyIDCMP(win, IDCMP_ALL);
	win->UserData = (APTR)w;
	w->win = win;
	w->next = windows;
	windows = w;
	return w;
}

/* Replies to and drops the messages of a window about to close, as the RKM describes. */
static void strip_messages(struct Window *win)
{
	struct Node *n, *next;
	Forbid();
	for (n = port->mp_MsgList.lh_Head; (next = n->ln_Succ); n = next) {
		struct IntuiMessage *m = (struct IntuiMessage *)n;
		if (m->IDCMPWindow == win) {
			Remove(n);
			ReplyMsg(&m->ExecMessage);
		}
	}
	win->UserPort = NULL;
	ModifyIDCMP(win, 0);
	Permit();
}

void qa_close(QAWin *w)
{
	if (!w)
		return;
	for (QAWin **p = &windows; *p; p = &(*p)->next)
		if (*p == w) {
			*p = w->next;
			break;
		}
	if (w->win) {
		strip_messages(w->win);
		CloseWindow(w->win);
	}
	free(w->title);
	free(w);
}

void qa_inner(const QAWin *w, int *x, int *y, int *width, int *height)
{
	struct Window *win = w->win;
	*x = win->LeftEdge + win->BorderLeft;
	*y = win->TopEdge + win->BorderTop;
	*width = win->Width - win->BorderLeft - win->BorderRight;
	*height = win->Height - win->BorderTop - win->BorderBottom;
}

void qa_outer(const QAWin *w, int *x, int *y, int *width, int *height)
{
	*x = w->win->LeftEdge;
	*y = w->win->TopEdge;
	*width = w->win->Width;
	*height = w->win->Height;
}

void qa_set_title(QAWin *w, const char *title)
{
	char *t = strdup(title ? title : "");
	if (!t)
		return;
	SetWindowTitles(w->win, t, (CONST_STRPTR)~0);
	free(w->title);
	w->title = t;
}

void qa_set_box(QAWin *w, int x, int y, int width, int height)
{
	ChangeWindowBox(w->win, x, y, width, height);
}

void qa_set_inner_size(QAWin *w, int width, int height)
{
	struct Window *win = w->win;
	ChangeWindowBox(win, win->LeftEdge, win->TopEdge, width + win->BorderLeft + win->BorderRight,
	                height + win->BorderTop + win->BorderBottom);
}

void qa_to_front(QAWin *w)
{
	WindowToFront(w->win);
	ActivateWindow(w->win);
}

void qa_pointer_visible(QAWin *w, int visible)
{
	(void)w; (void)visible; /* an empty sprite needs chip memory handling: left for later */
}

void qa_busy_pointer(QAWin *w, int busy)
{
	SetWindowPointer(w->win, WA_BusyPointer, busy ? TRUE : FALSE, TAG_DONE);
}

void qa_present(QAWin *w, const void *argb, int bytes_per_row, int width, int height)
{
	struct Window *win = w->win;
	int iw = win->Width - win->BorderLeft - win->BorderRight;
	int ih = win->Height - win->BorderTop - win->BorderBottom;
	if (width > iw) width = iw;
	if (height > ih) height = ih;
	if (width <= 0 || height <= 0)
		return;
	WritePixelArray((APTR)argb, 0, 0, bytes_per_row, win->RPort, win->BorderLeft, win->BorderTop, width, height, RECTFMT_ARGB);
}

static void key_event(struct IntuiMessage *m, QAEvent *ev)
{
	ev->type = QA_EV_KEY;
	ev->code = m->Code & ~IECODE_UP_PREFIX;
	ev->down = !(m->Code & IECODE_UP_PREFIX);
	ev->ascii = 0;
	memset(ev->utf8, 0, sizeof ev->utf8);
	if (ev->code == RAWKEY_NM_WHEEL_UP || ev->code == RAWKEY_NM_WHEEL_DOWN) {
		ev->type = ev->down ? QA_EV_WHEEL : QA_EV_NONE;
		ev->code = ev->code == RAWKEY_NM_WHEEL_UP ? 1 : -1;
		return;
	}
	if (ev->down) {
		struct InputEvent ie;
		UBYTE buf[8];
		memset(&ie, 0, sizeof ie);
		ie.ie_Class = IECLASS_RAWKEY;
		ie.ie_Code = m->Code;
		ie.ie_Qualifier = m->Qualifier;
		ie.ie_EventAddress = m->IAddress ? *(APTR *)m->IAddress : NULL;
		LONG n = MapRawKey(&ie, (STRPTR)buf, sizeof buf, NULL);
		if (n == 1 && (buf[0] >= 32 || buf[0] == 8 || buf[0] == 9 || buf[0] == 13 || buf[0] == 27)) {
			ev->ascii = (char)buf[0];
			/* Latin-1 to UTF-8. */
			if (buf[0] < 0x80) {
				ev->utf8[0] = (char)buf[0];
			} else {
				ev->utf8[0] = (char)(0xC0 | (buf[0] >> 6));
				ev->utf8[1] = (char)(0x80 | (buf[0] & 0x3F));
				ev->ascii = 0;
			}
		}
	}
}

int qa_next_event(QAEvent *ev)
{
	struct IntuiMessage *m;
	memset(ev, 0, sizeof *ev);
	if (!port)
		return 0;
	while ((m = (struct IntuiMessage *)GetMsg(port))) {
		struct Window *win = m->IDCMPWindow;
		QAWin *w = (QAWin *)win->UserData;
		ev->win = w;
		ev->user = w ? w->user : NULL;
		ev->qual = m->Qualifier;
		ev->x = win->LeftEdge + m->MouseX;
		ev->y = win->TopEdge + m->MouseY;
		ev->type = QA_EV_NONE;
		switch (m->Class) {
		case IDCMP_MOUSEMOVE: ev->type = QA_EV_MOVE; break;
		case IDCMP_MOUSEBUTTONS:
			ev->type = QA_EV_BUTTON;
			switch (m->Code & ~IECODE_UP_PREFIX) {
			case IECODE_LBUTTON: ev->code = 1; break;
			case IECODE_RBUTTON: ev->code = 2; break;
			case IECODE_MBUTTON: ev->code = 3; break;
			default: ev->type = QA_EV_NONE; break;
			}
			ev->down = !(m->Code & IECODE_UP_PREFIX);
			break;
		case IDCMP_RAWKEY: key_event(m, ev); break;
		case IDCMP_NEWSIZE: case IDCMP_CHANGEWINDOW: ev->type = QA_EV_SIZE; break;
		case IDCMP_CLOSEWINDOW: ev->type = QA_EV_CLOSE; break;
		case IDCMP_ACTIVEWINDOW: ev->type = QA_EV_ACTIVE; break;
		case IDCMP_INACTIVEWINDOW: ev->type = QA_EV_INACTIVE; break;
		case IDCMP_REFRESHWINDOW:
			BeginRefresh(win);
			EndRefresh(win, TRUE);
			ev->type = QA_EV_REFRESH;
			break;
		default: break;
		}
		ReplyMsg(&m->ExecMessage);
		if (ev->type != QA_EV_NONE)
			return 1;
	}
	return 0;
}

void qa_wait(unsigned ms)
{
	if (!port)
		return;
	if (!ms) {
		Wait(1UL << port->mp_SigBit);
		return;
	}
	/* A DOS tick is 20 ms; shorter waits poll once. */
	unsigned ticks = ms / 20;
	if (!ticks) ticks = 1;
	while (ticks-- && !(SetSignal(0, 0) & (1UL << port->mp_SigBit)))
		Delay(1);
}

void qa_mouse(int *x, int *y)
{
	*x = screen ? screen->MouseX : 0;
	*y = screen ? screen->MouseY : 0;
}

unsigned long long qa_millis(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)(tv.tv_usec / 1000);
}
