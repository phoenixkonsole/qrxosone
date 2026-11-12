#include "GHOST_WindowAROS.h"
#include "GHOST_SystemAROS.h"
#include "GHOST_ContextAROS.h"
#include "STR_String.h"

GHOST_WindowAROS::GHOST_WindowAROS(GHOST_SystemAROS *system, const STR_String &title, GHOST_TInt32 left, GHOST_TInt32 top,
                                   GHOST_TUns32 width, GHOST_TUns32 height, GHOST_TWindowState state,
                                   GHOST_TDrawingContextType type, const bool stereoVisual, const bool exclusive,
                                   const GHOST_TUns16 numOfAASamples)
    : GHOST_Window(width, height, state, stereoVisual, exclusive, numOfAASamples),
      m_system(system), m_win(NULL), m_state(GHOST_kWindowStateNormal), m_dirty(true)
{
	int sw, sh;
	qa_screen_size(&sw, &sh);
	/* Blender asks for the whole screen on first start; keep the title bar of the screen visible. */
	if ((int)width > sw) width = (GHOST_TUns32)sw;
	if ((int)height > sh) height = (GHOST_TUns32)sh;
	m_win = qa_open(title.ReadPtr(), left, top, (int)width, (int)height, this);
	if (!m_win)
		return;
	qa_outer(m_win, &m_normal[0], &m_normal[1], &m_normal[2], &m_normal[3]);
	if (state == GHOST_kWindowStateMaximized || state == GHOST_kWindowStateFullScreen)
		setState(GHOST_kWindowStateMaximized);
	setDrawingContextType(type);
}

GHOST_WindowAROS::~GHOST_WindowAROS()
{
	releaseNativeHandles();
	/* The context goes first: it still names this window. */
	setDrawingContextType(GHOST_kDrawingContextTypeNone);
	qa_close(m_win);
}

GHOST_Context *GHOST_WindowAROS::newDrawingContext(GHOST_TDrawingContextType type)
{
	if (type != GHOST_kDrawingContextTypeOpenGL || !m_win)
		return NULL;
	GHOST_Context *context = new GHOST_ContextAROS(m_wantStereoVisual, m_wantNumOfAASamples, m_win);
	if (context->initializeDrawingContext())
		return context;
	delete context;
	return NULL;
}

bool GHOST_WindowAROS::getValid() const { return m_win != NULL && GHOST_Window::getValid(); }

void GHOST_WindowAROS::setTitle(const STR_String &title)
{
	if (m_win)
		qa_set_title(m_win, title.ReadPtr());
}

void GHOST_WindowAROS::getTitle(STR_String &title) const { title = "Blender"; }

void GHOST_WindowAROS::getWindowBounds(GHOST_Rect &bounds) const
{
	int x, y, w, h;
	qa_outer(m_win, &x, &y, &w, &h);
	bounds.m_l = x; bounds.m_t = y; bounds.m_r = x + w; bounds.m_b = y + h;
}

void GHOST_WindowAROS::getClientBounds(GHOST_Rect &bounds) const
{
	int x, y, w, h;
	qa_inner(m_win, &x, &y, &w, &h);
	bounds.m_l = x; bounds.m_t = y; bounds.m_r = x + w; bounds.m_b = y + h;
}

GHOST_TSuccess GHOST_WindowAROS::setClientWidth(GHOST_TUns32 width)
{
	GHOST_Rect r;
	getClientBounds(r);
	return setClientSize(width, (GHOST_TUns32)r.getHeight());
}

GHOST_TSuccess GHOST_WindowAROS::setClientHeight(GHOST_TUns32 height)
{
	GHOST_Rect r;
	getClientBounds(r);
	return setClientSize((GHOST_TUns32)r.getWidth(), height);
}

GHOST_TSuccess GHOST_WindowAROS::setClientSize(GHOST_TUns32 width, GHOST_TUns32 height)
{
	qa_set_inner_size(m_win, (int)width, (int)height);
	return GHOST_kSuccess;
}

void GHOST_WindowAROS::screenToClient(GHOST_TInt32 inX, GHOST_TInt32 inY, GHOST_TInt32 &outX, GHOST_TInt32 &outY) const
{
	int x, y, w, h;
	qa_inner(m_win, &x, &y, &w, &h);
	outX = inX - x;
	outY = inY - y;
}

void GHOST_WindowAROS::clientToScreen(GHOST_TInt32 inX, GHOST_TInt32 inY, GHOST_TInt32 &outX, GHOST_TInt32 &outY) const
{
	int x, y, w, h;
	qa_inner(m_win, &x, &y, &w, &h);
	outX = inX + x;
	outY = inY + y;
}

GHOST_TSuccess GHOST_WindowAROS::setState(GHOST_TWindowState state)
{
	if (state == m_state)
		return GHOST_kSuccess;
	if (state == GHOST_kWindowStateMaximized || state == GHOST_kWindowStateFullScreen) {
		int sw, sh;
		qa_outer(m_win, &m_normal[0], &m_normal[1], &m_normal[2], &m_normal[3]);
		qa_screen_size(&sw, &sh);
		/* Below the screen's title bar, which Intuition keeps. */
		qa_set_box(m_win, 0, 12, sw, sh - 12);
		m_state = GHOST_kWindowStateMaximized;
	} else if (state == GHOST_kWindowStateNormal) {
		qa_set_box(m_win, m_normal[0], m_normal[1], m_normal[2], m_normal[3]);
		m_state = state;
	} else {
		return GHOST_kFailure; /* no minimizing on the Workbench screen */
	}
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowAROS::setOrder(GHOST_TWindowOrder order)
{
	if (order == GHOST_kWindowOrderTop)
		qa_to_front(m_win);
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowAROS::invalidate()
{
	m_dirty = true;
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowAROS::setWindowCursorVisibility(bool visible)
{
	qa_pointer_visible(m_win, visible);
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowAROS::setWindowCursorShape(GHOST_TStandardCursor shape)
{
	qa_busy_pointer(m_win, shape == GHOST_kStandardCursorWait);
	return GHOST_kSuccess;
}
