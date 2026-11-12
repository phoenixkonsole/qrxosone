/* GHOST for AROS on QRX OS One: Intuition windows on the Workbench screen, drawn by the Mac's
 * OpenGL through the ARMithlon GL door (tools/qrxgl68k). */
#ifndef __GHOST_SYSTEMAROS_H__
#define __GHOST_SYSTEMAROS_H__

#include "GHOST_System.h"
#include "../GHOST_Types.h"

class GHOST_WindowAROS;

class GHOST_SystemAROS : public GHOST_System {
public:
	GHOST_SystemAROS();
	~GHOST_SystemAROS();

	bool processEvents(bool waitForEvent);
	int toggleConsole(int) { return 0; }
	GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const;
	GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const;
	GHOST_TUns8 *getClipboard(bool selection) const;
	void putClipboard(GHOST_TInt8 *buffer, bool selection) const;
	GHOST_TUns64 getMilliSeconds() const;
	GHOST_TUns8 getNumDisplays() const { return 1; }
	GHOST_TSuccess getCursorPosition(GHOST_TInt32 &x, GHOST_TInt32 &y) const;
	GHOST_TSuccess setCursorPosition(GHOST_TInt32 x, GHOST_TInt32 y);
	void getMainDisplayDimensions(GHOST_TUns32 &width, GHOST_TUns32 &height) const;
	void getAllDisplayDimensions(GHOST_TUns32 &width, GHOST_TUns32 &height) const;
	GHOST_TSuccess init();

	GHOST_IWindow *createWindow(const STR_String &title, GHOST_TInt32 left, GHOST_TInt32 top,
	                            GHOST_TUns32 width, GHOST_TUns32 height, GHOST_TWindowState state,
	                            GHOST_TDrawingContextType type, GHOST_GLSettings glSettings,
	                            const bool exclusive = false, const GHOST_TEmbedderWindowID parentWindow = 0);

private:
	GHOST_ModifierKeys m_modifiers;
	GHOST_Buttons m_buttons;
	mutable char *m_clipboard;
};

#endif
