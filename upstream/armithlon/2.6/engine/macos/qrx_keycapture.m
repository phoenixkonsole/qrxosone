/* See qrx_keycapture.h */
#import "qrx_keycapture.h"
#include <dlfcn.h>
#include <stdlib.h>

/* CoreGraphics' switch for the symbolic hot keys (Mission Control, Exposé, Spaces, Spotlight).
 * Not public API, so it is looked up at run time; without it the capture of the events still works.
 * QRX_KEEP_HOTKEYS=1 leaves the system's hot keys alone. */
typedef int (*conn_fn)(void);
typedef int (*mode_fn)(int conn, int mode);
enum { HOTKEYS_ON = 0, HOTKEYS_OFF = 1 };

static void set_hotkeys(int mode)
{
    static conn_fn conn;
    static mode_fn set;
    static int looked, current = HOTKEYS_ON;
    if (!looked) {
        looked = 1;
        if (getenv("QRX_KEEP_HOTKEYS")) return;
        conn = (conn_fn)dlsym(RTLD_DEFAULT, "_CGSDefaultConnection");
        set = (mode_fn)dlsym(RTLD_DEFAULT, "CGSSetGlobalHotKeyOperatingMode");
    }
    if (!conn || !set || mode == current) return;
    set(conn(), mode);
    current = mode;
}
static void hotkeys_on_at_exit(void) { set_hotkeys(HOTKEYS_ON); }

void qrx_keycapture_focus(BOOL key)
{
    static int registered;
    if (!registered) { registered = 1; atexit(hotkeys_on_at_exit); }
    set_hotkeys(key ? HOTKEYS_OFF : HOTKEYS_ON);
}

BOOL qrx_keycapture_take(NSEvent *e, NSWindow *window, NSResponder *view)
{
    NSEventType t = e.type;
    if (t != NSEventTypeKeyDown && t != NSEventTypeKeyUp && t != NSEventTypeFlagsChanged) return NO;
    if (!window || !view || e.window != window || !window.isKeyWindow) return NO;
    if (t == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand) &&
        [e.charactersIgnoringModifiers isEqualToString:@"q"]) return NO;
    if (t == NSEventTypeKeyDown) [view keyDown:e];
    else if (t == NSEventTypeKeyUp) [view keyUp:e];
    else [view flagsChanged:e];
    return YES;
}
