/* Native view of the actual AROS RTG framebuffer. No host-rendered desktop. */
#import <Cocoa/Cocoa.h>
#include "qrx_aros_window.h"
#include "qrxmedia.h"
#include "qrx_hostfs.h"
static QRXMediaDevice media;
static NSWindow *window;
static BOOL running;
static QRXHostFS appsDisk;
static QRXVM *guestVM;
static NSString *mountedAppsPath;
static NSUserDefaults *diskPreferences;

@interface QRXAROSView : NSView
@property(strong) NSBitmapImageRep *bitmap;
@property unsigned guestWidth;
@property unsigned guestHeight;
@end
/* macOS physical key positions -> Amiga raw keys. */
static unsigned rawkey(unsigned k){
 static const unsigned char table[128]={
 0x20,0x21,0x22,0x23,0x25,0x24,0x31,0x32,0x33,0x34,0x30,0x35,0x10,0x11,0x12,0x13,
 0x15,0x14,0x01,0x02,0x03,0x04,0x06,0x05,0x0c,0x09,0x07,0x0b,0x08,0x0a,0x1b,0x18,
 0x16,0x1a,0x17,0x19,0x44,0x26,0x28,0x29,0x27,0x2a,0x0d,0x38,0x3a,0x36,0x37,0x39,
 0x42,0x40,0x00,0x41,0xff,0x45,0x67,0x66,0x60,0x62,0x64,0x63,0x61,0x65,0x63,0xff,
 0xff,0x3c,0xff,0x5d,0xff,0x5e,0xff,0xff,0xff,0xff,0xff,0x5c,0x43,0xff,0x4a,0xff,
 0xff,0xff,0x1d,0x1e,0x1f,0x2d,0x2e,0x2f,0x3d,0x3e,0xff,0x3f,0x4f,0xff,0xff,0xff,
 0x54,0x55,0x56,0x52,0x57,0x58,0xff,0x5a,0xff,0xff,0xff,0xff,0xff,0x59,0xff,0x5b,
 0xff,0xff,0x5f,0x70,0x48,0x46,0x53,0x71,0x51,0x49,0x50,0x4f,0x4e,0x4d,0x4c,0xff};
 return k<128?table[k]:0xff;
}
@implementation QRXAROSView
- (BOOL)acceptsFirstResponder{return YES;}
- (BOOL)isFlipped{return NO;}
- (void)drawRect:(NSRect)rect {
 [[NSColor blackColor]setFill];NSRectFill(self.bounds);
 if(self.bitmap)[self.bitmap drawInRect:self.bounds];
}
- (void)key:(NSEvent*)e down:(BOOL)down {
 unsigned code=rawkey(e.keyCode);if(code!=0xff)qrx_media_push_input(&media,((down?1u:2u)<<24)|(code<<16));
}
- (void)keyDown:(NSEvent*)e{[self key:e down:YES];}
- (void)keyUp:(NSEvent*)e{[self key:e down:NO];}
- (void)flagsChanged:(NSEvent*)e {
 unsigned k=e.keyCode;NSEventModifierFlags flag=(k==56||k==60)?NSEventModifierFlagShift:(k==58||k==61)?NSEventModifierFlagOption:(k==59||k==62)?NSEventModifierFlagControl:(k==57)?NSEventModifierFlagCapsLock:NSEventModifierFlagCommand;
 [self key:e down:(e.modifierFlags&flag)!=0];
}
- (void)position:(NSEvent*)e {
 NSPoint p=[self convertPoint:e.locationInWindow fromView:nil];
 unsigned x=MAX(0,MIN(self.guestWidth-1,p.x*self.guestWidth/self.bounds.size.width));
 unsigned y=MAX(0,MIN(self.guestHeight-1,(self.bounds.size.height-p.y)*self.guestHeight/self.bounds.size.height));
 qrx_media_set_mouse(&media,x,y,(unsigned)[NSEvent pressedMouseButtons]);
 /* Snapshot coordinates with each event: the FIFO may lag behind the host. */
 qrx_media_push_input(&media,(3u<<24)|((x&4095u)<<12)|(y&4095u));
}
- (void)mouseMoved:(NSEvent*)e{[self position:e];}
- (void)mouseDragged:(NSEvent*)e{[self position:e];}
- (void)rightMouseDragged:(NSEvent*)e{[self position:e];}
- (void)button:(NSEvent*)e down:(BOOL)down{[self position:e];qrx_media_push_input(&media,(4u<<24)|((unsigned)e.buttonNumber<<16)|(down?1:0));}
- (void)mouseDown:(NSEvent*)e{[self button:e down:YES];}
- (void)mouseUp:(NSEvent*)e{[self button:e down:NO];}
- (void)rightMouseDown:(NSEvent*)e{[self button:e down:YES];}
- (void)rightMouseUp:(NSEvent*)e{[self button:e down:NO];}
@end
static QRXAROSView *view;
@interface QRXAROSDelegate : NSObject<NSWindowDelegate>
@end
@implementation QRXAROSDelegate
- (BOOL)windowShouldClose:(NSWindow*)w{running=NO;return YES;}
- (void)quit:(id)sender{running=NO;}
- (void)showApps:(id)sender{
 if(mountedAppsPath)[[NSWorkspace sharedWorkspace]openURL:[NSURL fileURLWithPath:mountedAppsPath]];
}
- (void)chooseApps:(id)sender{
 NSOpenPanel *panel=[NSOpenPanel openPanel];panel.canChooseFiles=NO;panel.canChooseDirectories=YES;
 panel.allowsMultipleSelection=NO;panel.prompt=@"Als APPS: verwenden";
 panel.message=@"Beschreibbares zweites Laufwerk für QRX OS One. Die Änderung gilt beim nächsten ARMithlon-Start.";
 if(mountedAppsPath)panel.directoryURL=[NSURL fileURLWithPath:mountedAppsPath];
 if([panel runModal]!=NSModalResponseOK)return;
 NSString *path=panel.URL.path;QRXHostFS probe={0};
 if(!qrx_hostfs_mount(&probe,path.fileSystemRepresentation,false)){
  NSAlert *alert=[NSAlert new];alert.messageText=@"Verzeichnis konnte nicht geöffnet werden";
  alert.informativeText=path;[alert runModal];return;
 }
 qrx_hostfs_unmount(&probe);
 [diskPreferences setObject:path forKey:@"AppsDirectory"];
 NSAlert *alert=[NSAlert new];alert.messageText=@"APPS: für den nächsten Start gespeichert";
 alert.informativeText=[NSString stringWithFormat:@"%@\n\nSpeichere offene Arbeiten in QRX OS One und starte ARMithlon anschließend neu.",path];
 [alert runModal];
}

@end
static QRXAROSDelegate *delegate;
int qrx_aros_window_open(QRXVM *vm){@autoreleasepool{
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
 qrx_media_init(&media,NULL,NULL);qrx_vm_bind_media(vm,&media);
 window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,1024,768) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
 window.title=@"QRX OS One — ARMithlon / AROS RTG";window.releasedWhenClosed=NO;
 view=[[QRXAROSView alloc]initWithFrame:NSMakeRect(0,0,1024,768)];view.guestWidth=1024;view.guestHeight=768;
 window.contentView=view;window.acceptsMouseMovedEvents=YES;delegate=[QRXAROSDelegate new];window.delegate=delegate;
 guestVM=vm;diskPreferences=[[NSUserDefaults alloc]initWithSuiteName:@"org.qrxosone.armithlon.aros"];
 NSString *defaultPath=[[[NSBundle mainBundle]bundlePath].stringByDeletingLastPathComponent stringByAppendingPathComponent:@"Apps"];
 [[NSFileManager defaultManager]createDirectoryAtPath:defaultPath withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *appsPath=[diskPreferences stringForKey:@"AppsDirectory"]?:defaultPath;
 if(qrx_hostfs_mount(&appsDisk,appsPath.fileSystemRepresentation,false)){
  mountedAppsPath=appsPath;qrx_hostfs_attach2(vm,&appsDisk);
  fprintf(stderr,"[APPS] writable second disk: %s\n",appsPath.fileSystemRepresentation);
 }else{
  NSAlert *alert=[NSAlert new];alert.messageText=@"APPS: nicht verfügbar";
  alert.informativeText=appsPath;[alert runModal];
 }
 NSMenu *bar=[NSMenu new];NSMenuItem *appItem=[NSMenuItem new];[bar addItem:appItem];
 NSMenu *appMenu=[[NSMenu alloc]initWithTitle:@"ARMithlon"];appItem.submenu=appMenu;
 NSMenuItem *choose=[[NSMenuItem alloc]initWithTitle:@"Zweites Laufwerk auswählen…" action:@selector(chooseApps:) keyEquivalent:@"o"];choose.target=delegate;[appMenu addItem:choose];
 NSMenuItem *show=[[NSMenuItem alloc]initWithTitle:@"APPS:-Ordner im Finder öffnen" action:@selector(showApps:) keyEquivalent:@""];show.target=delegate;show.enabled=mountedAppsPath!=nil;[appMenu addItem:show];
 [appMenu addItem:[NSMenuItem separatorItem]];
 NSMenuItem *quit=[[NSMenuItem alloc]initWithTitle:@"ARMithlon beenden" action:@selector(quit:) keyEquivalent:@"q"];quit.target=delegate;[appMenu addItem:quit];
 NSApp.mainMenu=bar;

 [window center];[window makeKeyAndOrderFront:nil];[window makeFirstResponder:view];[NSApp finishLaunching];[NSApp activateIgnoringOtherApps:YES];running=YES;return 1;
}}
int qrx_aros_window_pump(void){@autoreleasepool{
 NSEvent *event;while((event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]))[NSApp sendEvent:event];
 [NSApp updateWindows];return running;
}}
void qrx_aros_window_frame(const unsigned char *rgb,unsigned w,unsigned h){@autoreleasepool{
 /* A representation caches its CGImage; replace it when pixels change. */
 {view.bitmap=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:w*3 bitsPerPixel:24];view.guestWidth=w;view.guestHeight=h;}
 memcpy(view.bitmap.bitmapData,rgb,(size_t)w*h*3);[view setNeedsDisplay:YES];
}}
void qrx_aros_window_close(void){[window orderOut:nil];running=NO;if(guestVM)qrx_hostfs_attach2(guestVM,NULL);qrx_hostfs_unmount(&appsDisk);guestVM=NULL;}
