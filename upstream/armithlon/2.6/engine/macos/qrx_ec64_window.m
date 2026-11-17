/* The Mac window of the 68100EC64 machine, see qrx_ec64_window.h */
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
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
/* The board's sound ring, drained by CoreAudio at 48 kHz */
static AudioQueueRef audio_queue;
static void audio_callback(void *opaque, AudioQueueRef q, AudioQueueBufferRef buffer)
{
    (void)opaque;
    size_t frames = buffer->mAudioDataBytesCapacity / 4u;
    qec64_board *b = board;
    size_t got = b ? qec64_board_pull_audio(b, (int16_t *)buffer->mAudioData, frames) : 0;
    if (got < frames) memset((int16_t *)buffer->mAudioData + got * 2u, 0, (frames - got) * 4u);
    buffer->mAudioDataByteSize = (UInt32)(frames * 4u);
    AudioQueueEnqueueBuffer(q, buffer, 0, NULL);
}
static void audio_open(void)
{
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate = 48000; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame = 4; fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&fmt, audio_callback, NULL, NULL, kCFRunLoopCommonModes, 0, &audio_queue) != noErr) {
        fprintf(stderr, "[EC64] no audio output\n");
        audio_queue = NULL;
        return;
    }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef buf;
        if (AudioQueueAllocateBuffer(audio_queue, 1024u * 4u, &buf) == noErr) audio_callback(NULL, audio_queue, buf);
    }
    AudioQueueStart(audio_queue, NULL);
}

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
- (void)flagsChanged:(NSEvent *)e
{
    /* modifiers come without keyDown: Shift, Control, Option, Command, Caps Lock by their key codes */
    static const struct { unsigned short key; NSEventModifierFlags flag; } mods[] = {
        {56, NSEventModifierFlagShift}, {60, NSEventModifierFlagShift}, {59, NSEventModifierFlagControl},
        {62, NSEventModifierFlagControl}, {58, NSEventModifierFlagOption}, {61, NSEventModifierFlagOption},
        {55, NSEventModifierFlagCommand}, {54, NSEventModifierFlagCommand}, {57, NSEventModifierFlagCapsLock}};
    for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; i++)
        if (mods[i].key == e.keyCode && board) {
            unsigned kind = (e.modifierFlags & mods[i].flag) ? QEC64_INPUT_KEY_DOWN : QEC64_INPUT_KEY_UP;
            qec64_board_push_event(board, (kind << 24) | ((uint32_t)e.keyCode << 16));
        }
}
- (BOOL)acceptsFirstResponder { return YES; }
@end

/* The guest's screen: the pointer in framebuffer pixels, buttons 0 left, 1 right, 2 middle */
@interface QEC64Picture : NSImageView
@end
@implementation QEC64Picture
- (void)updateTrackingAreas
{
    for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)sendMove:(NSEvent *)e
{
    NSImage *img = self.image;
    if (!board || !img) return;
    NSSize is = img.size, vs = self.bounds.size;
    if (is.width < 1 || is.height < 1) return;
    double scale = fmin(vs.width / is.width, vs.height / is.height);
    double ox = (vs.width - is.width * scale) / 2, oy = (vs.height - is.height * scale) / 2;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    double x = (p.x - ox) / scale, y = is.height - (p.y - oy) / scale;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x > is.width - 1) x = is.width - 1; if (y > is.height - 1) y = is.height - 1;
    qec64_board_push_event(board, (QEC64_INPUT_MOVE << 24) | ((uint32_t)x & 0xFFF) << 12 | ((uint32_t)y & 0xFFF));
}
- (void)sendButton:(unsigned)b down:(BOOL)down event:(NSEvent *)e
{
    [self sendMove:e];
    if (board) qec64_board_push_event(board, (QEC64_INPUT_BUTTON << 24) | (b << 16) | (down ? 1u : 0u));
}
- (void)mouseMoved:(NSEvent *)e { [self sendMove:e]; }
- (void)mouseDragged:(NSEvent *)e { [self sendMove:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self sendMove:e]; }
- (void)mouseDown:(NSEvent *)e { [self sendButton:0 down:YES event:e]; }
- (void)mouseUp:(NSEvent *)e { [self sendButton:0 down:NO event:e]; }
- (void)rightMouseDown:(NSEvent *)e { [self sendButton:1 down:YES event:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self sendButton:1 down:NO event:e]; }
- (void)otherMouseDown:(NSEvent *)e { [self sendButton:2 down:YES event:e]; }
- (void)otherMouseUp:(NSEvent *)e { [self sendButton:2 down:NO event:e]; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
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
        picture = [[QEC64Picture alloc] initWithFrame:r];
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
        audio_open();
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
        if (audio_queue) { AudioQueueStop(audio_queue, true); AudioQueueDispose(audio_queue, true); audio_queue = NULL; }
        if (!closed) [window close];
        window = nil;
        board = NULL;
    }
}
