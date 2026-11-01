/* QRX OS One 1.8 – native macOS GUI for the REAL guest 68k firmware shell.
 * AppKit provides the window; all commands pass through guest ROM UART/MMIO.
 * Not AmigaOS, AROS or SAGA emulation. No CPU/GPU performance claims.
 */
#import <Cocoa/Cocoa.h>
#import "qrx_bootshell.h"
#import "qrx_hostfs.h"
#import "qrx_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface QRXDesktopController : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate> {
    QRXVM _vm;
    QRXShell *_shell;
    QRXHostFS _share;
    uint8_t *_memory;
    QRXHostMemory _allocation;
    BOOL _ready;
}
@property (strong) NSWindow *window;
@property (strong) NSTextView *terminal;
@property (strong) NSTextField *input;
@property (strong) NSTextField *status;
@property (strong) NSMutableData *consoleBytes;
- (void)receivedByte:(uint8_t)ch;
- (void)flushConsole;
- (void)runCommand:(NSString *)command;
@end

static void qrx_terminal_char(void *ctx,uint8_t ch) {
    QRXDesktopController *controller=(__bridge QRXDesktopController *)ctx;
    [controller receivedByte:ch];
}
static NSTextField *label(NSString *title,CGFloat size,NSColor *color,NSRect frame){
    NSTextField *v=[[NSTextField alloc]initWithFrame:frame];
    v.stringValue=title;v.editable=NO;v.bezeled=NO;v.drawsBackground=NO;
    v.selectable=NO;v.font=[NSFont systemFontOfSize:size weight:NSFontWeightMedium];
    v.textColor=color;
    return v;
}
@implementation QRXDesktopController
- (void)receivedByte:(uint8_t)ch {[_consoleBytes appendBytes:&ch length:1];}
- (void)flushConsole {
    if(!_consoleBytes.length)return;
    NSString *line=[[NSString alloc]initWithData:_consoleBytes encoding:NSUTF8StringEncoding];
    if(!line)line=[[NSString alloc]initWithData:_consoleBytes encoding:NSISOLatin1StringEncoding];
    if(!line)line=@"[binary UART output]";
    NSTextStorage *storage=self.terminal.textStorage;
    NSDictionary *attributes=@{NSFontAttributeName:[NSFont fontWithName:@"Menlo" size:13] ?: [NSFont userFixedPitchFontOfSize:13],
        NSForegroundColorAttributeName:[NSColor colorWithCalibratedRed:0.67 green:0.94 blue:0.76 alpha:1]};
    [storage appendAttributedString:[[NSAttributedString alloc]initWithString:line attributes:attributes]];
    [self.terminal scrollRangeToVisible:NSMakeRange(storage.length,0)];
    [_consoleBytes setLength:0];
}
- (BOOL)startGuest {
    size_t bytes=qrx_ram_auto_bytes();
    if(!qrx_ram_allocate(&_allocation,bytes))return NO;
    _memory=_allocation.addr;
    _shell=calloc(1,sizeof(*_shell));
    if(!_memory||!_shell)return NO;
    qrx_vm_init(&_vm,_memory,bytes);
    qrx_vm_set_host(&_vm,qrx_terminal_char,NULL,(__bridge void*)self);
    if(!qrx_build_boot_shell(_memory,bytes))return NO;
    NSArray<NSString*> *paths=NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES);
    NSString *root=[paths.firstObject stringByAppendingPathComponent:@"QRX OS One"];
    NSError *err=nil;
    if(![[NSFileManager defaultManager]createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:&err]){
        NSLog(@"QRX disk parent failed: %@",err);return NO;
    }
    NSString *disk=[root stringByAppendingPathComponent:@"qrxfs-1.img"];
    /* User-selected filesystem sharing is opt-in; environment only. No implicit
       exposure of ~/Documents or the user's home directory. */
    const char *sharedir=getenv("QRX_HOST_SHARE");
    const char *share_rw=getenv("QRX_HOST_SHARE_RW");
    if(sharedir&&*sharedir){
        if(!qrx_hostfs_mount(&_share,sharedir,share_rw&&strcmp(share_rw,"1")==0?NO:YES)){
            NSLog(@"QRX HOST directory unavailable");return NO;
        }
        qrx_hostfs_attach(&_vm,&_share);
    }
    if(!qrx_shell_mount(_shell,&_vm,disk.fileSystemRepresentation))return NO;
    if(!qrx_vm_reset(&_vm))return NO;
    if(qrx_vm_run(&_vm,3000)<0)return NO;
    [self flushConsole];
    _ready=YES;return YES;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.consoleBytes=[NSMutableData data];
    NSRect rect=NSMakeRect(0,0,900,620);
    self.window=[[NSWindow alloc]initWithContentRect:rect
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [self.window center];self.window.title=@"QRX OS One – QRX68100";
    self.window.delegate=self;
    NSView *content=self.window.contentView;
    content.wantsLayer=YES;
    content.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.066 green:0.083 blue:0.105 alpha:1].CGColor;
    NSTextField *title=label(@"QRX OS ONE  /  68100",22,[NSColor colorWithCalibratedRed:0.90 green:0.96 blue:0.94 alpha:1],NSMakeRect(24,568,500,30));
    title.autoresizingMask=NSViewMinYMargin;
    [content addSubview:title];
    self.status=label(@"BOOTING  •  Single-core compatibility firmware",12,[NSColor colorWithCalibratedRed:0.54 green:0.74 blue:0.65 alpha:1],NSMakeRect(25,540,790,22));
    self.status.autoresizingMask=NSViewMinYMargin;
    [content addSubview:self.status];
    NSScrollView *scroll=[[NSScrollView alloc]initWithFrame:NSMakeRect(24,122,852,408)];
    scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    scroll.borderType=NSNoBorder;scroll.hasVerticalScroller=YES;
    scroll.drawsBackground=YES;
    scroll.backgroundColor=[NSColor colorWithCalibratedRed:0.025 green:0.042 blue:0.046 alpha:1];
    self.terminal=[[NSTextView alloc]initWithFrame:NSMakeRect(0,0,852,408)];
    self.terminal.editable=NO;self.terminal.selectable=YES;
    self.terminal.verticallyResizable=YES;
    self.terminal.horizontallyResizable=NO;
    self.terminal.textContainer.widthTracksTextView=YES;
    self.terminal.backgroundColor=scroll.backgroundColor;
    self.terminal.textColor=[NSColor colorWithCalibratedRed:0.67 green:0.94 blue:0.76 alpha:1];
    self.terminal.font=[NSFont fontWithName:@"Menlo" size:13] ?: [NSFont userFixedPitchFontOfSize:13];
    scroll.documentView=self.terminal;
    [content addSubview:scroll];
    self.input=[[NSTextField alloc]initWithFrame:NSMakeRect(24,75,750,32)];
    self.input.placeholderString=@"QRX-Befehl eingeben, z. B. help, ls, write NOTES.TXT ...";
    self.input.font=[NSFont fontWithName:@"Menlo" size:13] ?: [NSFont userFixedPitchFontOfSize:13];
    self.input.bezelStyle=NSTextFieldRoundedBezel;
    self.input.target=self;self.input.action=@selector(submitCommand:);
    self.input.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin;
    [content addSubview:self.input];
    NSButton *enter=[NSButton buttonWithTitle:@"Ausführen" target:self action:@selector(submitCommand:)];
    enter.frame=NSMakeRect(786,75,90,32);
    enter.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;
    enter.bezelStyle=NSBezelStyleRounded;
    [content addSubview:enter];
    NSArray<NSString*>*actions=@[@"help",@"ls",@"run HELLO.QEX",@"bench",@"reboot",@"shutdown"];
    NSArray<NSString*>*labels=@[@"Hilfe",@"Dateien",@"68k-Demo",@"CPU-Test",@"Neustart",@"Herunterfahren"];
    CGFloat x=24;
    for(NSUInteger i=0;i<actions.count;i++){
        NSButton *button=[NSButton buttonWithTitle:labels[i] target:self action:@selector(quickCommand:)];
        button.frame=NSMakeRect(x,27,(i==2||i==5)?135:115,32);
        button.tag=(NSInteger)i;
        button.bezelStyle=NSBezelStyleRounded;
        button.autoresizingMask=NSViewMaxYMargin;
        [content addSubview:button];x+=button.frame.size.width+8;
    }
    if([self startGuest])self.status.stringValue=[NSString stringWithFormat:
        @"ONLINE  •  68k Firmware  •  %zu MiB RAM (Host/4, 32-bit limit)  •  Core 0",_vm.ram.size/1048576u];
    else{
        self.status.stringValue=@"BOOT FAILED  •  Check QRXFS-1 image and console";
        NSString *error=@"\n[QRX ERROR] Firmware reset or disk mount failed. A corrupted disk is never overwritten.\n";
        [self.terminal.textStorage appendAttributedString:[[NSAttributedString alloc]initWithString:error]];
    }
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if(_ready)[self.window makeFirstResponder:self.input];
}
- (void)runCommand:(NSString *)command {
    if(!_ready)return;
    NSData *utf8=[command dataUsingEncoding:NSUTF8StringEncoding];
    if(utf8.length>124){NSBeep();self.status.stringValue=@"Eingabe zu lang (max. 124 Bytes)";return;}
    if(!qrx_shell_run_line(_shell,command.UTF8String,60000)){
        self.status.stringValue=@"GUEST ERROR  •  Check command or CPU state";
        [self flushConsole];return;
    }
    [self flushConsole];
    if(_shell->reboot){
        _shell->reboot=false;_shell->input_head=_shell->input_tail=0;
        if(qrx_vm_reset(&_vm)&&qrx_vm_run(&_vm,3000)>=0){
            self.status.stringValue=@"ONLINE  •  68k guest restarted  •  QRXFS unchanged";
            [self flushConsole];
        } else{_ready=NO;self.status.stringValue=@"GUEST RESET FAILED";}
    }
    if(_shell->shutdown){_ready=NO;self.status.stringValue=@"OFFLINE  •  QRX68100 shut down";}
}
- (void)submitCommand:(id)sender {
    (void)sender;
    NSString *command=[self.input.stringValue copy];
    self.input.stringValue=@"";
    [self runCommand:command];
}
- (void)quickCommand:(NSButton*)sender {
    NSArray<NSString*> *actions=@[@"help",@"ls",@"run HELLO.QEX",@"bench",@"reboot",@"shutdown"];
    NSInteger index=sender.tag;
    if(index>=0&&(NSUInteger)index<actions.count)[self runCommand:actions[(NSUInteger)index]];
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if(_shell){qrx_shell_detach(_shell);free(_shell);_shell=NULL;}
    qrx_hostfs_attach(&_vm,NULL);qrx_hostfs_unmount(&_share);
    qrx_vm_destroy(&_vm);qrx_ram_release(&_allocation);_memory=NULL;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;return YES;
}
@end
int main(int argc,const char **argv){
    (void)argc;(void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        QRXDesktopController *controller=[[QRXDesktopController alloc]init];
        [NSApp setDelegate:controller];
        [NSApp run];
    }
    return 0;
}
