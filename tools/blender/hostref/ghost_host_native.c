/* The Mac reference for Blender's interface: ghost_aros_native.h without a screen. Windows are
 * rectangles, presented frames go to files, input comes from a script. Built into the native
 * reference (configure-host-gui.sh), so the same GHOST code runs on both sides.
 *
 *   QRX_REF_INNER=WxH+X+Y   drawable area of the window on the screen (as the guest had it)
 *   QRX_REF_CAPTURE=path    every presented frame as path.NNNN (PPM), like the door's QRX_GL68K_CAPTURE
 *   QRX_REF_EVENTS=list     F:c:X:Y click, F:k:CODE key (AROS raw key), F:q quit; F is the number of frames
 *                           presented before the event, X and Y are screen pixels
 */
#include "ghost_aros_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

struct QAWin {
	int x, y, w, h;
	void *user;
	struct QAWin *next;
};

static struct QAWin *windows;
static int mouse_x, mouse_y;
static unsigned presented;
static QAEvent queue[64];
static unsigned queued;

int qa_init(void) { return 1; }
void qa_exit(void) {}
void qa_screen_size(int *w, int *h) { *w = 1280; *h = 800; }

QAWin *qa_open(const char *title, int left, int top, int width, int height, void *user)
{
	(void)title;
	QAWin *w = calloc(1, sizeof *w);
	w->x = left < 0 ? 4 : left;
	w->y = top < 0 ? 29 : top;
	w->w = width;
	w->h = height;
	const char *inner = getenv("QRX_REF_INNER");
	if (inner)
		sscanf(inner, "%dx%d+%d+%d", &w->w, &w->h, &w->x, &w->y);
	w->user = user;
	w->next = windows;
	windows = w;
	return w;
}

void qa_close(QAWin *w)
{
	for (QAWin **p = &windows; *p; p = &(*p)->next)
		if (*p == w) {
			*p = w->next;
			break;
		}
	free(w);
}

void qa_inner(const QAWin *w, int *x, int *y, int *width, int *height) { *x = w->x; *y = w->y; *width = w->w; *height = w->h; }
void qa_outer(const QAWin *w, int *x, int *y, int *width, int *height) { *x = w->x - 4; *y = w->y - 25; *width = w->w + 8; *height = w->h + 29; }
void qa_set_title(QAWin *w, const char *title) { (void)w; (void)title; }
void qa_set_inner_size(QAWin *w, int width, int height) { w->w = width; w->h = height; }
void qa_set_box(QAWin *w, int x, int y, int width, int height) { (void)w; (void)x; (void)y; (void)width; (void)height; }
void qa_to_front(QAWin *w) { (void)w; }
void qa_pointer_visible(QAWin *w, int visible) { (void)w; (void)visible; }
void qa_busy_pointer(QAWin *w, int busy) { (void)w; (void)busy; }

void qa_present(QAWin *w, const void *argb, int bytes_per_row, int width, int height)
{
	(void)w;
	const char *path = getenv("QRX_REF_CAPTURE");
	if (path) {
		char name[1024];
		snprintf(name, sizeof name, "%s.%04u", path, presented);
		FILE *f = fopen(name, "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", width, height);
			for (int y = 0; y < height; y++) {
				const unsigned char *row = (const unsigned char *)argb + (size_t)y * bytes_per_row;
				for (int x = 0; x < width; x++)
					fwrite(row + x * 4 + 1, 1, 3, f);
			}
			fclose(f);
		}
	}
	presented++;
}

static void push(int type, int x, int y, int code, int down)
{
	if (queued >= 64 || !windows)
		return;
	QAEvent *e = &queue[queued++];
	memset(e, 0, sizeof *e);
	e->type = type;
	e->win = windows;
	e->user = windows->user;
	e->x = x;
	e->y = y;
	e->code = code;
	e->down = down;
	if (type == QA_EV_KEY && down && code == 0x42) {
		e->ascii = '\t';
		e->utf8[0] = '\t';
	}
}

/* Takes the script's events that are due after this many presented frames. */
static void script(void)
{
	static const char *next;
	static int started;
	if (!started) {
		next = getenv("QRX_REF_EVENTS");
		started = 1;
	}
	while (next && *next) {
		char *e;
		unsigned frame = (unsigned)strtoul(next, &e, 10);
		if (frame > presented || *e != ':')
			break;
		char kind = e[1];
		e += 2;
		if (kind == 'c') {
			int x = (int)strtol(e + 1, &e, 0), y = (int)strtol(e + 1, &e, 0);
			mouse_x = x;
			mouse_y = y;
			push(QA_EV_MOVE, x, y, 0, 0);
			push(QA_EV_BUTTON, x, y, 1, 1);
			push(QA_EV_BUTTON, x, y, 1, 0);
		} else if (kind == 'k') {
			int code = (int)strtol(e + 1, &e, 0);
			push(QA_EV_KEY, mouse_x, mouse_y, code, 1);
			push(QA_EV_KEY, mouse_x, mouse_y, code, 0);
		} else if (kind == 'q') {
			exit(0);
		}
		next = *e == ',' ? e + 1 : "";
	}
}

int qa_next_event(QAEvent *ev)
{
	script();
	if (!queued)
		return 0;
	*ev = queue[0];
	memmove(queue, queue + 1, (queued - 1) * sizeof queue[0]);
	queued--;
	return 1;
}

void qa_wait(unsigned ms) { usleep((ms ? ms : 5) * 1000); }
void qa_mouse(int *x, int *y) { *x = mouse_x; *y = mouse_y; }

unsigned long long qa_millis(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)(tv.tv_usec / 1000);
}
