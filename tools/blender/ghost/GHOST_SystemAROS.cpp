#include "GHOST_SystemAROS.h"
#include "GHOST_WindowAROS.h"
#include "GHOST_WindowManager.h"
#include "GHOST_DisplayManager.h"
#include "GHOST_EventButton.h"
#include "GHOST_EventCursor.h"
#include "GHOST_EventKey.h"
#include "GHOST_EventWheel.h"
#include "GHOST_TimerManager.h"
#include "ghost_aros_native.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

GHOST_SystemAROS::GHOST_SystemAROS() : GHOST_System(), m_clipboard(NULL) {}

GHOST_SystemAROS::~GHOST_SystemAROS()
{
	free(m_clipboard);
	qa_exit();
}

GHOST_TSuccess GHOST_SystemAROS::init()
{
	if (!qa_init())
		return GHOST_kFailure;
	if (GHOST_System::init() != GHOST_kSuccess)
		return GHOST_kFailure;
	m_displayManager = new GHOST_DisplayManager();
	m_displayManager->initialize();
	return GHOST_kSuccess;
}

GHOST_TUns64 GHOST_SystemAROS::getMilliSeconds() const { return qa_millis(); }

void GHOST_SystemAROS::getMainDisplayDimensions(GHOST_TUns32 &width, GHOST_TUns32 &height) const
{
	int w, h;
	qa_screen_size(&w, &h);
	width = (GHOST_TUns32)w;
	height = (GHOST_TUns32)h;
}

void GHOST_SystemAROS::getAllDisplayDimensions(GHOST_TUns32 &width, GHOST_TUns32 &height) const
{
	getMainDisplayDimensions(width, height);
}

GHOST_IWindow *GHOST_SystemAROS::createWindow(const STR_String &title, GHOST_TInt32 left, GHOST_TInt32 top,
                                              GHOST_TUns32 width, GHOST_TUns32 height, GHOST_TWindowState state,
                                              GHOST_TDrawingContextType type, GHOST_GLSettings glSettings,
                                              const bool exclusive, const GHOST_TEmbedderWindowID)
{
	GHOST_WindowAROS *window = new GHOST_WindowAROS(this, title, left, top, width, height, state, type,
	                                                (glSettings.flags & GHOST_glStereoVisual) != 0, exclusive,
	                                                glSettings.numOfAASamples);
	if (!window->getValid()) {
		delete window;
		return NULL;
	}
	m_windowManager->addWindow(window);
	m_windowManager->setActiveWindow(window);
	pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowSize, window));
	return window;
}

GHOST_TSuccess GHOST_SystemAROS::getModifierKeys(GHOST_ModifierKeys &keys) const
{
	keys = m_modifiers;
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemAROS::getButtons(GHOST_Buttons &buttons) const
{
	buttons = m_buttons;
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemAROS::getCursorPosition(GHOST_TInt32 &x, GHOST_TInt32 &y) const
{
	int mx, my;
	qa_mouse(&mx, &my);
	x = mx;
	y = my;
	return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemAROS::setCursorPosition(GHOST_TInt32, GHOST_TInt32)
{
	return GHOST_kFailure; /* moving the pointer needs input.device; continuous grab works without */
}

/* The clipboard stays inside the program for now. */
GHOST_TUns8 *GHOST_SystemAROS::getClipboard(bool) const
{
	if (!m_clipboard)
		return NULL;
	size_t n = strlen(m_clipboard) + 1;
	GHOST_TUns8 *s = (GHOST_TUns8 *)malloc(n);
	if (s)
		memcpy(s, m_clipboard, n);
	return s;
}

void GHOST_SystemAROS::putClipboard(GHOST_TInt8 *buffer, bool selection) const
{
	if (selection || !buffer)
		return;
	free(m_clipboard);
	m_clipboard = strdup((const char *)buffer);
}

static GHOST_TKey convert_key(int code)
{
	static const struct { unsigned char raw; GHOST_TKey key; } map[] = {
		{0x00, GHOST_kKeyAccentGrave}, {0x0B, GHOST_kKeyMinus}, {0x0C, GHOST_kKeyEqual}, {0x0D, GHOST_kKeyBackslash},
		{0x1A, GHOST_kKeyLeftBracket}, {0x1B, GHOST_kKeyRightBracket}, {0x29, GHOST_kKeySemicolon}, {0x2A, GHOST_kKeyQuote},
		{0x2B, GHOST_kKeyBackslash}, {0x30, GHOST_kKeyGrLess}, {0x38, GHOST_kKeyComma}, {0x39, GHOST_kKeyPeriod},
		{0x3A, GHOST_kKeySlash}, {0x40, GHOST_kKeySpace}, {0x41, GHOST_kKeyBackSpace}, {0x42, GHOST_kKeyTab},
		{0x43, GHOST_kKeyNumpadEnter}, {0x44, GHOST_kKeyEnter}, {0x45, GHOST_kKeyEsc}, {0x46, GHOST_kKeyDelete},
		{0x47, GHOST_kKeyInsert}, {0x48, GHOST_kKeyUpPage}, {0x49, GHOST_kKeyDownPage}, {0x4A, GHOST_kKeyNumpadMinus},
		{0x4B, GHOST_kKeyF11}, {0x4C, GHOST_kKeyUpArrow}, {0x4D, GHOST_kKeyDownArrow}, {0x4E, GHOST_kKeyRightArrow},
		{0x4F, GHOST_kKeyLeftArrow}, {0x5C, GHOST_kKeyNumpadSlash}, {0x5D, GHOST_kKeyNumpadAsterisk},
		{0x5E, GHOST_kKeyNumpadPlus}, {0x3C, GHOST_kKeyNumpadPeriod}, {0x60, GHOST_kKeyLeftShift},
		{0x61, GHOST_kKeyRightShift}, {0x62, GHOST_kKeyCapsLock}, {0x63, GHOST_kKeyLeftControl},
		{0x64, GHOST_kKeyLeftAlt}, {0x65, GHOST_kKeyRightAlt}, {0x66, GHOST_kKeyOS}, {0x67, GHOST_kKeyOS},
		{0x6B, GHOST_kKeyScrollLock}, {0x6C, GHOST_kKeyPrintScreen}, {0x6D, GHOST_kKeyNumLock}, {0x6E, GHOST_kKeyPause},
		{0x6F, GHOST_kKeyF12}, {0x70, GHOST_kKeyHome}, {0x71, GHOST_kKeyEnd},
		{0x0F, GHOST_kKeyNumpad0}, {0x1D, GHOST_kKeyNumpad1}, {0x1E, GHOST_kKeyNumpad2}, {0x1F, GHOST_kKeyNumpad3},
		{0x2D, GHOST_kKeyNumpad4}, {0x2E, GHOST_kKeyNumpad5}, {0x2F, GHOST_kKeyNumpad6}, {0x3D, GHOST_kKeyNumpad7},
		{0x3E, GHOST_kKeyNumpad8}, {0x3F, GHOST_kKeyNumpad9},
	};
	static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
	if (code >= 0x01 && code <= 0x09)
		return (GHOST_TKey)(GHOST_kKey1 + (code - 0x01));
	if (code == 0x0A)
		return GHOST_kKey0;
	if (code >= 0x10 && code <= 0x19)
		return (GHOST_TKey)(GHOST_kKeyA + (row1[code - 0x10] - 'a'));
	if (code >= 0x20 && code <= 0x28)
		return (GHOST_TKey)(GHOST_kKeyA + (row2[code - 0x20] - 'a'));
	if (code >= 0x31 && code <= 0x37)
		return (GHOST_TKey)(GHOST_kKeyA + (row3[code - 0x31] - 'a'));
	if (code >= 0x50 && code <= 0x59)
		return (GHOST_TKey)(GHOST_kKeyF1 + (code - 0x50));
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
		if (map[i].raw == code)
			return map[i].key;
	return GHOST_kKeyUnknown;
}

static void set_modifier(GHOST_ModifierKeys &m, GHOST_TKey key, bool down)
{
	switch (key) {
	case GHOST_kKeyLeftShift: m.set(GHOST_kModifierKeyLeftShift, down); break;
	case GHOST_kKeyRightShift: m.set(GHOST_kModifierKeyRightShift, down); break;
	case GHOST_kKeyLeftControl: m.set(GHOST_kModifierKeyLeftControl, down); break;
	case GHOST_kKeyRightControl: m.set(GHOST_kModifierKeyRightControl, down); break;
	case GHOST_kKeyLeftAlt: m.set(GHOST_kModifierKeyLeftAlt, down); break;
	case GHOST_kKeyRightAlt: m.set(GHOST_kModifierKeyRightAlt, down); break;
	case GHOST_kKeyOS: m.set(GHOST_kModifierKeyOS, down); break;
	default: break;
	}
}

bool GHOST_SystemAROS::processEvents(bool waitForEvent)
{
	bool any = false;
	GHOST_TimerManager *timers = getTimerManager();
	/* QRX_GHOST_TRACE: a line per event and a sign of life every 500 calls, on stderr. */
	static int trace = -1;
	static unsigned long calls;
	if (trace < 0)
		trace = getenv("QRX_GHOST_TRACE") != NULL;
	if (trace && (++calls % 500 == 0 || calls < 20))
		{ fprintf(stderr, "ghost: processEvents %lu\n", calls); fflush(stderr); }
	if (waitForEvent && !m_eventManager->getNumEvents()) {
		GHOST_TUns64 next = timers->nextFireTime();
		if (next == GHOST_kFireTimeNever) {
			qa_wait(0);
		} else {
			GHOST_TUns64 now = getMilliSeconds();
			if (next > now)
				qa_wait((unsigned)(next - now));
		}
	}
	if (timers->fireTimers(getMilliSeconds()))
		any = true;
	QAEvent ev;
	while (qa_next_event(&ev)) {
		GHOST_WindowAROS *win = (GHOST_WindowAROS *)ev.user;
		if (trace) {
			fprintf(stderr, "ghost: event %d at %d,%d code %d down %d window %p known %d\n", ev.type, ev.x, ev.y, ev.code, ev.down,
			        (void *)win, win ? (int)m_windowManager->getWindowFound(win) : -1);
			fflush(stderr);
		}
		if (!win || !m_windowManager->getWindowFound(win))
			continue;
		GHOST_TUns64 t = getMilliSeconds();
		switch (ev.type) {
		case QA_EV_MOVE:
			pushEvent(new GHOST_EventCursor(t, GHOST_kEventCursorMove, win, ev.x, ev.y));
			break;
		case QA_EV_BUTTON: {
			GHOST_TButtonMask b = ev.code == 1 ? GHOST_kButtonMaskLeft : ev.code == 2 ? GHOST_kButtonMaskRight : GHOST_kButtonMaskMiddle;
			m_buttons.set(b, ev.down != 0);
			/* Where the button went down matters more than the last move. */
			pushEvent(new GHOST_EventCursor(t, GHOST_kEventCursorMove, win, ev.x, ev.y));
			pushEvent(new GHOST_EventButton(t, ev.down ? GHOST_kEventButtonDown : GHOST_kEventButtonUp, win, b));
			break;
		}
		case QA_EV_WHEEL:
			pushEvent(new GHOST_EventWheel(t, win, ev.code));
			break;
		case QA_EV_KEY: {
			GHOST_TKey key = convert_key(ev.code);
			set_modifier(m_modifiers, key, ev.down != 0);
			pushEvent(new GHOST_EventKey(t, ev.down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp, win, key, ev.ascii, ev.utf8));
			break;
		}
		case QA_EV_SIZE:
			pushEvent(new GHOST_Event(t, GHOST_kEventWindowSize, win));
			break;
		case QA_EV_CLOSE:
			pushEvent(new GHOST_Event(t, GHOST_kEventWindowClose, win));
			break;
		case QA_EV_ACTIVE:
			m_windowManager->setActiveWindow(win);
			pushEvent(new GHOST_Event(t, GHOST_kEventWindowActivate, win));
			break;
		case QA_EV_INACTIVE:
			m_windowManager->setWindowInactive(win);
			pushEvent(new GHOST_Event(t, GHOST_kEventWindowDeactivate, win));
			break;
		case QA_EV_REFRESH:
			win->invalidate();
			break;
		default:
			break;
		}
		any = true;
	}
	/* Windows asked to be drawn again. */
	std::vector<GHOST_IWindow *> &wins = m_windowManager->getWindows();
	for (size_t i = 0; i < wins.size(); i++) {
		GHOST_WindowAROS *w = (GHOST_WindowAROS *)wins[i];
		if (w->takeDirty()) {
			pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowUpdate, w));
			any = true;
		}
	}
	return any;
}
