/* The Intuition side of GHOST for AROS, in C so that the AROS headers stay out of the C++ classes.
 * All windows share one IDCMP port; positions are in screen pixels. */
#ifndef GHOST_AROS_NATIVE_H
#define GHOST_AROS_NATIVE_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct QAWin QAWin;

enum {
	QA_EV_NONE,
	QA_EV_MOVE,     /* x, y: pointer on the screen */
	QA_EV_BUTTON,   /* code: 1 left, 2 right, 3 middle; down */
	QA_EV_KEY,      /* code: raw key without the up bit; down; ascii; utf8 */
	QA_EV_WHEEL,    /* code: +1 up, -1 down */
	QA_EV_SIZE,
	QA_EV_CLOSE,
	QA_EV_ACTIVE,
	QA_EV_INACTIVE,
	QA_EV_REFRESH,
};

typedef struct {
	int type;
	QAWin *win;
	void *user;
	int x, y;
	int code;
	int down;
	unsigned qual;  /* IEQUALIFIER bits as Intuition reported them */
	char ascii;
	char utf8[6];
} QAEvent;

int qa_init(void);
void qa_exit(void);
void qa_screen_size(int *w, int *h);
QAWin *qa_open(const char *title, int left, int top, int width, int height, void *user);
void qa_close(QAWin *w);
/* The drawable area, in screen coordinates. */
void qa_inner(const QAWin *w, int *x, int *y, int *width, int *height);
void qa_outer(const QAWin *w, int *x, int *y, int *width, int *height);
void qa_set_title(QAWin *w, const char *title);
void qa_set_inner_size(QAWin *w, int width, int height);
void qa_set_box(QAWin *w, int x, int y, int width, int height);
void qa_to_front(QAWin *w);
void qa_pointer_visible(QAWin *w, int visible);
void qa_busy_pointer(QAWin *w, int busy);
/* An ARGB frame, top row first, into the drawable area. */
void qa_present(QAWin *w, const void *argb, int bytes_per_row, int width, int height);
/* 1 when an event was taken off the port. */
int qa_next_event(QAEvent *ev);
/* Waits up to ms for a message on the port; 0 waits only for a message. */
void qa_wait(unsigned ms);
void qa_mouse(int *x, int *y);
unsigned long long qa_millis(void);

#ifdef __cplusplus
}
#endif
#endif
