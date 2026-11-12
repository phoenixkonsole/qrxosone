#ifndef __GHOST_WINDOWAROS_H__
#define __GHOST_WINDOWAROS_H__

#include "GHOST_Window.h"
#include "ghost_aros_native.h"

class STR_String;
class GHOST_SystemAROS;

class GHOST_WindowAROS : public GHOST_Window {
public:
	GHOST_WindowAROS(GHOST_SystemAROS *system, const STR_String &title, GHOST_TInt32 left, GHOST_TInt32 top,
	                 GHOST_TUns32 width, GHOST_TUns32 height, GHOST_TWindowState state,
	                 GHOST_TDrawingContextType type, const bool stereoVisual, const bool exclusive,
	                 const GHOST_TUns16 numOfAASamples);
	~GHOST_WindowAROS();

	const GHOST_TabletData *GetTabletData() { return NULL; }
	QAWin *getNative() const { return m_win; }
	bool getValid() const;
	void setTitle(const STR_String &title);
	void getTitle(STR_String &title) const;
	void getWindowBounds(GHOST_Rect &bounds) const;
	void getClientBounds(GHOST_Rect &bounds) const;
	GHOST_TSuccess setClientWidth(GHOST_TUns32 width);
	GHOST_TSuccess setClientHeight(GHOST_TUns32 height);
	GHOST_TSuccess setClientSize(GHOST_TUns32 width, GHOST_TUns32 height);
	void screenToClient(GHOST_TInt32 inX, GHOST_TInt32 inY, GHOST_TInt32 &outX, GHOST_TInt32 &outY) const;
	void clientToScreen(GHOST_TInt32 inX, GHOST_TInt32 inY, GHOST_TInt32 &outX, GHOST_TInt32 &outY) const;
	GHOST_TSuccess setState(GHOST_TWindowState state);
	GHOST_TWindowState getState() const { return m_state; }
	GHOST_TSuccess setOrder(GHOST_TWindowOrder order);
	GHOST_TSuccess invalidate();
	bool takeDirty() { bool d = m_dirty; m_dirty = false; return d; }
	GHOST_TSuccess beginFullScreen() const { return GHOST_kFailure; }
	GHOST_TSuccess endFullScreen() const { return GHOST_kFailure; }

protected:
	GHOST_Context *newDrawingContext(GHOST_TDrawingContextType type);
	GHOST_TSuccess setWindowCursorVisibility(bool visible);
	GHOST_TSuccess setWindowCursorGrab(GHOST_TGrabCursorMode) { return GHOST_kSuccess; }
	GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor shape);
	GHOST_TSuccess setWindowCustomCursorShape(GHOST_TUns8[16][2], GHOST_TUns8[16][2], int, int) { return GHOST_kSuccess; }
	GHOST_TSuccess setWindowCustomCursorShape(GHOST_TUns8 *, GHOST_TUns8 *, int, int, int, int, int, int) { return GHOST_kSuccess; }

private:
	GHOST_SystemAROS *m_system;
	QAWin *m_win;
	GHOST_TWindowState m_state;
	int m_normal[4];
	bool m_dirty;
};

#endif
