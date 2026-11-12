#include "GHOST_ContextAROS.h"
#include "qgl_api.h"
#include <cstdio>
#include <cstdlib>

unsigned GHOST_ContextAROS::s_shared = 0;
unsigned GHOST_ContextAROS::s_current = 0;
int GHOST_ContextAROS::s_count = 0;

GHOST_ContextAROS::GHOST_ContextAROS(bool stereoVisual, GHOST_TUns16 numOfAASamples, QAWin *win)
    : GHOST_Context(stereoVisual, numOfAASamples), m_win(win), m_ctx(0), m_frame(NULL), m_frame_w(0), m_frame_h(0)
{
}

GHOST_ContextAROS::~GHOST_ContextAROS()
{
	if (m_ctx) {
		if (s_current == m_ctx) {
			qgl_make_current(0, 0, 0);
			s_current = 0;
		}
		qgl_destroy_context(m_ctx);
		if (--s_count == 0)
			s_shared = 0;
	}
	free(m_frame);
}

/* The window's present size goes with every switch, so a resized window gets a matching framebuffer. */
bool GHOST_ContextAROS::makeCurrent()
{
	if (!m_ctx || !m_win)
		return false;
	int x, y, w, h;
	qa_inner(m_win, &x, &y, &w, &h);
	if (!qgl_make_current(m_ctx, w, h))
		return false;
	s_current = m_ctx;
	return true;
}

GHOST_TSuccess GHOST_ContextAROS::initializeDrawingContext()
{
	if (!qgl_available()) {
		fprintf(stderr, "GHOST: this engine has no GL door (ARMithlon door service 8)\n");
		return GHOST_kFailure;
	}
	m_ctx = qgl_create_context(s_shared);
	if (!m_ctx)
		return GHOST_kFailure;
	if (!s_shared)
		s_shared = m_ctx;
	s_count++;
	if (!makeCurrent())
		return GHOST_kFailure;
	initContextGLEW();
	initClearGL();
	swapBuffers();
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextAROS::activateDrawingContext()
{
	if (!m_ctx)
		return GHOST_kFailure;
	activateGLEW();
	return makeCurrent() ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_ContextAROS::releaseNativeHandles()
{
	m_win = NULL;
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextAROS::swapBuffers()
{
	if (!m_ctx || !m_win)
		return GHOST_kFailure;
	int x, y, w, h;
	qa_inner(m_win, &x, &y, &w, &h);
	if (w <= 0 || h <= 0)
		return GHOST_kSuccess;
	if (s_current != m_ctx && !makeCurrent())
		return GHOST_kFailure;
	if (w != m_frame_w || h != m_frame_h) {
		free(m_frame);
		m_frame = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
		m_frame_w = m_frame ? w : 0;
		m_frame_h = m_frame ? h : 0;
	}
	if (!m_frame)
		return GHOST_kFailure;
	/* The framebuffer has the size of the last switch; a window resized since then shows that much. */
	qgl_read_frame(m_frame, w * 4, w, h);
	qa_present(m_win, m_frame, w * 4, w, h);
	return GHOST_kSuccess;
}
