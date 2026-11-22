/* Keyboard capture for the emulator windows (qrx_aros_window.m, qrx_ec64_window.m).
 * Games hold several keys at once and use Control, Option and Command as fire buttons. macOS gets in
 * the way three times: Control with the arrow keys opens Mission Control or App Exposé, Command
 * combinations go to the menu, and AppKit sends no key up while Command is held. While the window
 * is the key window, its key events go straight to its view, and the system's global hot keys are
 * off; when it loses the focus they come back and every held key is let go. */
#ifndef QRX_KEYCAPTURE_H
#define QRX_KEYCAPTURE_H
#import <Cocoa/Cocoa.h>

/* From the event loop, before [NSApp sendEvent:]: YES when the event was a key event for window and
 * went to view (keyDown:, keyUp:, flagsChanged:). Command-Q stays with the application. */
BOOL qrx_keycapture_take(NSEvent *e, NSWindow *window, NSResponder *view);
/* From the window delegate: becoming the key window turns the global hot keys off, resigning turns
 * them on again (also at exit). */
void qrx_keycapture_focus(BOOL key);
#endif
