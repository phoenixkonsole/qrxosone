/* The Mac window of the 68100EC64 machine, see qrx_ec64_window.h */
#import <Cocoa/Cocoa.h>
#include "qrx_ec64_window.h"
#include <pthread.h>

static qec64_board *board;
static qec64_window_input input;
static NSWindow *window;
static NSTextView *console;
static NSImageView *picture;
static NSScrollView *scroller;
static BOOL closed;
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableData *pending;          /* console text not shown yet */
static uint32_t shown_frames = UINT32_MAX;

@interface QEC64Console : NSTextView
@end
@implementation QEC64Console
- (void)keyDown:(NSEvent *)e
{
    NSString *s = e.characters;
    unsigned short k = e.keyCode;
    if (board) qec64_board_push_event(board, (QEC64_INPUT_KEY_DOWN << 24) | ((uint32_t)k << 16));
    if (!input.typed || !s.length) return;
    if (k == 36 || k == 76) { input.typed(input.ctx, "\n", 1); return; }       /* Return, Enter */
    if (k == 51) { input.typed(input.ctx, "\b", 1); return; }                   /* Delete */
    const char *u = s.UTF8String;
    if (u) input.typed(input.ctx, u, strlen(u));
}
- (void)keyUp:(NSEvent *)e
{
    if (board) qec64_board_push_event(board, (QEC64_INPUT_KEY_UP << 24) | ((uint32_t)e.keyCode << 16));
}
- (BOOL)acceptsFirstResponder { return YES; }
@end

@interface QEC64Delegate : NSObject <NSWindowDelegate>
@end
@implementation QEC64Delegate
- (void)windowWillClose:(NSNotification *)n { closed = YES; }
@end
static QEC64Delegate *delegate;

void qec64_window_console(const char *text, size_t len)
{
    pthread_mutex_lock(&pending_lock);
    if (!pending) pending = [NSMutableData data];
    [pending appendBytes:text length:len];
    pthread_mutex_unlock(&pending_lock);
}

int qec64_window_open(const char *title, qec64_board *b, const qec64_window_input *in)
{
    @autoreleasepool {
        board = b;
        if (in) input = *in;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect r = NSMakeRect(0, 0, 900, 600);
        window = [[NSWindow alloc] initWithContentRect:r
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                               backing:NSBackingStoreBuffered defer:NO];
        window.title = [NSString stringWithUTF8String:title ? title : "68100EC64"];
        delegate = [QEC64Delegate new];
        window.delegate = delegate;
        scroller = [[NSScrollView alloc] initWithFrame:r];
        scroller.hasVerticalScroller = YES;
        scroller.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        console = [[QEC64Console alloc] initWithFrame:r];
        console.editable = NO;
        console.selectable = YES;
        console.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
        console.backgroundColor = [NSColor colorWithCalibratedRed:0.08 green:0.10 blue:0.16 alpha:1];
        console.textColor = [NSColor colorWithCalibratedWhite:0.92 alpha:1];
        console.autoresizingMask = NSViewWidthSizable;
        scroller.documentView = console;
        picture = [[NSImageView alloc] initWithFrame:r];
        picture.imageScaling = NSImageScaleProportionallyUpOrDown;
        picture.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        picture.hidden = YES;
        NSView *content = [[NSView alloc] initWithFrame:r];
        [content addSubview:scroller];
        [content addSubview:picture];
        window.contentView = content;
        [window center];
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:console];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp finishLaunching];
        return 1;
    }
}

static void show_console_text(void)
{
    NSData *d = nil;
    pthread_mutex_lock(&pending_lock);
    if (pending.length) { d = pending; pending = nil; }
    pthread_mutex_unlock(&pending_lock);
    if (!d) return;
    /* The guest's console is ISO-8859-1; CR is dropped, backspace removes a character */
    NSMutableString *s = [[NSMutableString alloc] initWithBytes:d.bytes length:d.length encoding:NSISOLatin1StringEncoding];
    NSTextStorage *t = console.textStorage;
    NSDictionary *attrs = @{ NSFontAttributeName: console.font, NSForegroundColorAttributeName: console.textColor };
    NSMutableString *run = [NSMutableString string];
    for (NSUInteger i = 0; i < s.length; i++) {
        unichar ch = [s characterAtIndex:i];
        if (ch == '\r') continue;
        if (ch == '\b') {
            if (run.length) [run deleteCharactersInRange:NSMakeRange(run.length - 1, 1)];
            else if (t.length) [t deleteCharactersInRange:NSMakeRange(t.length - 1, 1)];
            continue;
        }
        [run appendFormat:@"%C", ch];
    }
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:run attributes:attrs]];
    if (t.length > 400000) [t deleteCharactersInRange:NSMakeRange(0, t.length - 300000)];
    [console scrollRangeToVisible:NSMakeRange(t.length, 0)];
}

static void show_framebuffer(void)
{
    const uint8_t *px;
    uint32_t w, h, stride, fmt;
    if (!board || !qec64_board_framebuffer(board, &px, &w, &h, &stride, &fmt)) {
        if (!picture.hidden) { picture.hidden = YES; scroller.hidden = NO; [window makeFirstResponder:console]; }
        return;
    }
    uint32_t frames = board->fb_frames;
    if (!picture.hidden && frames == shown_frames) return;
    shown_frames = frames;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
        bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:w * 4 bitsPerPixel:32];
    uint8_t *out = rep.bitmapData;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *line = px + (uint64_t)y * stride;
        uint8_t *o = out + (uint64_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            if (fmt == QEC64_FB_FMT_XRGB32) { o[0] = line[x * 4 + 1]; o[1] = line[x * 4 + 2]; o[2] = line[x * 4 + 3]; }
            else {
                uint32_t v = (uint32_t)line[x * 2] << 8 | line[x * 2 + 1];
                o[0] = (uint8_t)((v >> 11 & 31) * 255 / 31); o[1] = (uint8_t)((v >> 5 & 63) * 255 / 63); o[2] = (uint8_t)((v & 31) * 255 / 31);
            }
            o[3] = 255;
        }
    }
    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
    [img addRepresentation:rep];
    picture.image = img;
    if (picture.hidden) { picture.hidden = NO; scroller.hidden = YES; [window makeFirstResponder:console]; }
}

int qec64_window_pump(void)
{
    @autoreleasepool {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:1.0 / 60];
        for (;;) {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
        }
        show_console_text();
        show_framebuffer();
        return !closed;
    }
}

void qec64_window_close(void)
{
    @autoreleasepool {
        if (!closed) [window close];
        window = nil;
        board = NULL;
    }
}
