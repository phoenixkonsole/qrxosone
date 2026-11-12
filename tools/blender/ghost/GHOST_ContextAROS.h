/* A GL context on the Mac for one Intuition window. Every context shares objects with the first. */
#ifndef __GHOST_CONTEXTAROS_H__
#define __GHOST_CONTEXTAROS_H__

#include "GHOST_Context.h"
#include "ghost_aros_native.h"

class GHOST_ContextAROS : public GHOST_Context {
public:
	GHOST_ContextAROS(bool stereoVisual, GHOST_TUns16 numOfAASamples, QAWin *win);
	~GHOST_ContextAROS();
	GHOST_TSuccess swapBuffers();
	GHOST_TSuccess activateDrawingContext();
	GHOST_TSuccess initializeDrawingContext();
	GHOST_TSuccess releaseNativeHandles();
	GHOST_TSuccess setSwapInterval(int) { return GHOST_kFailure; }
	GHOST_TSuccess getSwapInterval(int &interval) { interval = 0; return GHOST_kSuccess; }

private:
	bool makeCurrent();
	QAWin *m_win;
	unsigned m_ctx;
	unsigned char *m_frame;
	int m_frame_w, m_frame_h;
	static unsigned s_shared, s_current;
	static int s_count;
};

#endif
