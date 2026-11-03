// ARMITHLON native macOS developer preview: Metal + AudioQueue + Cocoa.
// This displays diagnostic firmware, not an AROS desktop or SAGA passthrough.
#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#import <AudioToolbox/AudioToolbox.h>
#include "qrx_demo.h"
#import "qrx_metal_renderer.h"
#include "qrx86_ipc.h"
#include "qrx86_input.h"
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    QRXDemo *demo;
    QRXMediaDevice *media;
    QRXAccelStream accel;
    QRXAccelStream qrx86Accel;
    pthread_t qrx86Worker;
    BOOL qrx86Mode;
    char qrx86Socket[104];
    char qrx86InputSocket[104];
    uint8_t *rgba;
    pthread_mutex_t frame_lock;
    pthread_t worker;
    atomic_bool running;
    atomic_uint video_seq;
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[4];
} QRXPlatform;

static void *qrx86_socket_worker(void *ctx) {
    QRXPlatform *p=(QRXPlatform *)ctx;
    int rc=qrx86_ipc_serve(p->qrx86Socket,&p->qrx86Accel,&p->running);
    if(rc!=0)fprintf(stderr,"QRX86 host graphics socket unavailable: %s\n",p->qrx86Socket);
    return NULL;
}

/* All Q86I packets are bounded and sent to the native QRX86 HLE process.
 * No guest 68k memory addresses or Cocoa objects cross this boundary. */
static void qrx86_send_event(QRXPlatform *p,uint32_t type,uint32_t code,
                             uint32_t ch,uint32_t x,uint32_t y,uint32_t button){
    if(!p||!p->qrx86Mode||!p->qrx86InputSocket[0])return;
    QRX86InputEvent e={type,code,ch,x,y,button};
    (void)qrx86_input_send(p->qrx86InputSocket,&e);
}
/* macOS hardware key codes do not equal Win32 virtual-key codes. */
static uint32_t qrx_windows_vk(NSEvent *event,unichar ch){
    switch(event.keyCode){
    case 123:return 0x25;case 124:return 0x27;case 125:return 0x28;case 126:return 0x26;
    case 53:return 0x1B;case 36:return 0x0D;case 51:return 0x08;
    case 48:return 0x09;case 49:return 0x20;case 117:return 0x2E;
    default:break;
    }
    if(ch>='a'&&ch<='z')return (uint32_t)(ch-'a'+'A');
    if((ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9'))return ch;
    return 0;
}
static void qrx_video_present(void *opaque,const uint8_t *src,uint32_t w,uint32_t h,uint32_t pitch) {
    QRXPlatform *p=(QRXPlatform *)opaque;
    if(w!=QRX_GFX_WIDTH || h!=QRX_GFX_HEIGHT || pitch!=QRX_GFX_PITCH || !p->rgba)return;
    pthread_mutex_lock(&p->frame_lock);
    for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
        const uint8_t *pixel=src+(size_t)y*pitch+(size_t)x*4;
        uint8_t *dst=p->rgba+((size_t)y*w+x)*4;
        dst[0]=pixel[1]; dst[1]=pixel[2]; dst[2]=pixel[3]; dst[3]=255;
    }
    atomic_fetch_add_explicit(&p->video_seq,1,memory_order_release);
    pthread_mutex_unlock(&p->frame_lock);
}

static uint64_t qrx_monotonic_ns(void) {
    struct timespec now={0};
    if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 0;
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
}
static void *qrx_worker(void *opaque) {
    QRXPlatform *p=(QRXPlatform *)opaque;
    uint64_t next=qrx_monotonic_ns();
    while(atomic_load_explicit(&p->running,memory_order_acquire)) {
        if(!qrx_demo_frame(p->demo)) {fprintf(stderr,"ARMITHLON: 68k MMIO diagnostic failed\n");break;}
        next+=16666667ull; /* 800 PCM frames @ 48kHz for each 60 Hz render step */
        uint64_t now=qrx_monotonic_ns();
        if(now && now<next) {
            uint64_t remaining=next-now;
            struct timespec delay={(time_t)(remaining/1000000000ull),(long)(remaining%1000000000ull)};
            nanosleep(&delay,NULL);
        } else if(now>next+100000000ull) {
            /* No giant catch-up loops if paused or CPU temporarily stalls. */
            next=now;
        }
    }
    return NULL;
}

static void qrx_audio_callback(void *opaque,AudioQueueRef q,AudioQueueBufferRef buffer) {
    QRXPlatform *p=(QRXPlatform *)opaque;
    if(!atomic_load_explicit(&p->running,memory_order_acquire))return;
    size_t frames=buffer->mAudioDataBytesCapacity/4;
    qrx_media_pull_audio(p->media,(int16_t *)buffer->mAudioData,frames);
    buffer->mAudioDataByteSize=(UInt32)(frames*4);
    AudioQueueEnqueueBuffer(q,buffer,0,NULL);
}
static BOOL qrx_start_audio(QRXPlatform *p) {
    AudioStreamBasicDescription fmt={0};
    fmt.mSampleRate=QRX_AUDIO_RATE;
    fmt.mFormatID=kAudioFormatLinearPCM;
    fmt.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked;
    fmt.mFramesPerPacket=1;
    fmt.mChannelsPerFrame=2;
    fmt.mBitsPerChannel=16;
    fmt.mBytesPerFrame=4;
    fmt.mBytesPerPacket=4;
    OSStatus st=AudioQueueNewOutput(&fmt,qrx_audio_callback,p,NULL,kCFRunLoopCommonModes,0,&p->queue);
    if(st!=noErr) {fprintf(stderr,"AudioQueueNewOutput error: %d\n",(int)st);return NO;}
    for(int i=0;i<4;i++) {
        st=AudioQueueAllocateBuffer(p->queue,4096*4,&p->buffers[i]);
        if(st!=noErr)return NO;
        qrx_audio_callback(p,p->queue,p->buffers[i]);
    }
    st=AudioQueueStart(p->queue,NULL);
    if(st!=noErr){fprintf(stderr,"AudioQueueStart error: %d\n",(int)st);return NO;}
    return YES;
}

static NSString *const kMetalShader =
 @"#include <metal_stdlib>\n"
 @"using namespace metal;\n"
 @"struct VOut { float4 position [[position]]; float2 uv; };\n"
 @"vertex VOut vertex_qrx(uint i [[vertex_id]]) {\n"
 @"  float2 xy[3] = { float2(-1.0,-1.0),float2(3.0,-1.0),float2(-1.0,3.0) };\n"
 @"  VOut o; o.position=float4(xy[i],0,1); o.uv=float2((xy[i].x+1)*0.5,(1-xy[i].y)*0.5); return o;\n"
 @"}\n"
 @"fragment float4 fragment_qrx(VOut i [[stage_in]],\n"
 @" texture2d<float> image [[texture(0)]], sampler texSampler [[sampler(0)]]) {\n"
 @"  return image.sample(texSampler,i.uv); }\n";

@interface QRXDisplayView : MTKView <MTKViewDelegate>
@property(nonatomic,assign) QRXPlatform *platform;
@property(nonatomic,strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic,strong) QRXMetalRenderer *accelerator;
@property(nonatomic,strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic,strong) id<MTLTexture> texture;
@property(nonatomic,strong) id<MTLSamplerState> sampler;
@property(nonatomic,assign) unsigned lastVideoSeq;
@property(nonatomic,assign) QRXAccelFrame *lastAccelFrame;
@property(nonatomic,strong) NSTrackingArea *mouseTracking;
@end
@implementation QRXDisplayView
- (instancetype)initWithFrame:(NSRect)rect device:(id<MTLDevice>)device {
    self=[super initWithFrame:rect device:device];
    if(!self)return nil;
    self.delegate=self;
    self.preferredFramesPerSecond=60;
    self.enableSetNeedsDisplay=NO;
    self.paused=NO;
    self.colorPixelFormat=MTLPixelFormatBGRA8Unorm;
    self.clearColor=MTLClearColorMake(0.025,0.035,0.07,1);
    self.depthStencilPixelFormat=MTLPixelFormatDepth32Float;
    self.clearDepth=1.0;
    self.commandQueue=[device newCommandQueue];
    self.accelerator=[[QRXMetalRenderer alloc]initWithDevice:device pixelFormat:self.colorPixelFormat depthFormat:self.depthStencilPixelFormat];
    if(!self.accelerator)return nil;
    MTLTextureDescriptor *tex=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:QRX_GFX_WIDTH height:QRX_GFX_HEIGHT mipmapped:NO];
    tex.usage=MTLTextureUsageShaderRead;
    self.texture=[device newTextureWithDescriptor:tex];
    NSError *error=nil;
    id<MTLLibrary> lib=[device newLibraryWithSource:kMetalShader options:nil error:&error];
    if(!lib){NSLog(@"ARMITHLON Metal shader: %@",error);return nil;}
    MTLRenderPipelineDescriptor *desc=[[MTLRenderPipelineDescriptor alloc]init];
    desc.vertexFunction=[lib newFunctionWithName:@"vertex_qrx"];
    desc.fragmentFunction=[lib newFunctionWithName:@"fragment_qrx"];
    desc.colorAttachments[0].pixelFormat=self.colorPixelFormat;
    self.pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
    if(!self.pipeline){NSLog(@"ARMITHLON pipeline: %@",error);return nil;}
    MTLSamplerDescriptor *sd=[[MTLSamplerDescriptor alloc]init];
    sd.minFilter=MTLSamplerMinMagFilterNearest;
    sd.magFilter=MTLSamplerMinMagFilterNearest;
    self.sampler=[device newSamplerStateWithDescriptor:sd];
    return self;
}
- (void)dealloc {qrx_accel_frame_release(self.lastAccelFrame);self.lastAccelFrame=NULL;}
- (BOOL)acceptsFirstResponder{return YES;}
- (void)updateTrackingAreas {
    if(self.mouseTracking)[self removeTrackingArea:self.mouseTracking];
    self.mouseTracking=[[NSTrackingArea alloc]initWithRect:self.bounds
        options:NSTrackingMouseMoved|NSTrackingActiveInKeyWindow|NSTrackingInVisibleRect
          owner:self userInfo:nil];
    [self addTrackingArea:self.mouseTracking];
    [super updateTrackingAreas];
}
- (void)drawInMTKView:(MTKView *)view {
    QRXPlatform *p=self.platform;
    if(!p || !p->rgba)return;
    QRXAccelFrame *nativeFrame=qrx_accel_take_latest(p->qrx86Mode?&p->qrx86Accel:&p->accel);
    if(nativeFrame){
        qrx_accel_frame_release(self.lastAccelFrame);
        self.lastAccelFrame=nativeFrame;
    }
    if(self.lastAccelFrame){
        BOOL ok=[self.accelerator drawFrame:self.lastAccelFrame onView:view commandQueue:self.commandQueue];
        if(!ok)NSLog(@"ARMITHLON Metal GPU submission failed");
        return;
    }
    pthread_mutex_lock(&p->frame_lock);
    unsigned seq=atomic_load_explicit(&p->video_seq,memory_order_acquire);
    if(seq!=self.lastVideoSeq){
        [self.texture replaceRegion:MTLRegionMake2D(0,0,QRX_GFX_WIDTH,QRX_GFX_HEIGHT)
                       mipmapLevel:0 withBytes:p->rgba bytesPerRow:QRX_GFX_PITCH];
        self.lastVideoSeq=seq;
    }
    pthread_mutex_unlock(&p->frame_lock);
    id<CAMetalDrawable> drawable=view.currentDrawable;
    MTLRenderPassDescriptor *pass=view.currentRenderPassDescriptor;
    if(!drawable || !pass)return;
    id<MTLCommandBuffer> cb=[self.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:self.pipeline];
    [e setFragmentTexture:self.texture atIndex:0];
    [e setFragmentSamplerState:self.sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {(void)view;(void)size;}
- (void)qrxPushKey:(NSEvent *)event down:(BOOL)down {
    QRXMediaDevice *m=self.platform->media;
    if(!m)return;
    NSString *text=[event charactersIgnoringModifiers];
    unsigned ch=[text length] ? (unsigned)[text characterAtIndex:0] : 0;
    uint32_t packed=((down?QRX_INPUT_KEY_DOWN:QRX_INPUT_KEY_UP)<<24)
                  |(((uint32_t)event.keyCode&255u)<<16)|(ch&65535u);
    qrx_media_push_input(m,packed);
    uint32_t vk=qrx_windows_vk(event,(unichar)ch);
    if(vk)qrx86_send_event(self.platform,down?QRX86_INPUT_KEY_DOWN:QRX86_INPUT_KEY_UP,
                          vk,ch,0,0,0);
}
- (void)keyDown:(NSEvent *)e {if(!e.isARepeat)[self qrxPushKey:e down:YES];}
- (void)keyUp:(NSEvent *)e {[self qrxPushKey:e down:NO];}
- (void)qrxMouse:(NSEvent *)e {
    QRXMediaDevice *m=self.platform->media;
    if(!m)return;
    NSPoint p=[self convertPoint:[e locationInWindow] fromView:nil];
    double fx=fmax(0.0,fmin(1.0,p.x/fmax(1.0,self.bounds.size.width)));
    double fy=fmax(0.0,fmin(1.0,1.0-p.y/fmax(1.0,self.bounds.size.height)));
    uint32_t x=(uint32_t)(fx*(QRX_GFX_WIDTH-1)), y=(uint32_t)(fy*(QRX_GFX_HEIGHT-1));
    qrx_media_set_mouse(m,x,y,(uint32_t)[NSEvent pressedMouseButtons]);
    qrx_media_push_input(m,(QRX_INPUT_MOUSE_MOVE<<24));
    qrx86_send_event(self.platform,QRX86_INPUT_MOUSE_MOVE,0,0,x,y,0);
}
- (void)mouseMoved:(NSEvent *)e {[self qrxMouse:e];}
- (void)mouseDragged:(NSEvent *)e {[self qrxMouse:e];}
- (void)rightMouseDragged:(NSEvent *)e {[self qrxMouse:e];}
- (void)otherMouseDragged:(NSEvent *)e {[self qrxMouse:e];}
- (void)qrxButton:(NSEvent *)e down:(BOOL)down {
    [self qrxMouse:e];
    uint32_t button=(uint32_t)e.buttonNumber;
    qrx_media_push_input(self.platform->media,(QRX_INPUT_MOUSE_BUTTON<<24)|((button&255u)<<16)|(down?1u:0u));
    uint32_t guestButton=button==0?1:button==1?2:3;
    NSPoint pos=[self convertPoint:[e locationInWindow] fromView:nil];
    uint32_t x=(uint32_t)(fmax(0.,fmin(1.,pos.x/fmax(1.,self.bounds.size.width)))*(QRX_GFX_WIDTH-1));
    uint32_t y=(uint32_t)(fmax(0.,fmin(1.,1.-pos.y/fmax(1.,self.bounds.size.height)))*(QRX_GFX_HEIGHT-1));
    qrx86_send_event(self.platform,down?QRX86_INPUT_MOUSE_DOWN:QRX86_INPUT_MOUSE_UP,0,0,x,y,guestButton);
}
- (void)mouseDown:(NSEvent *)e {[self qrxButton:e down:YES];}
- (void)mouseUp:(NSEvent *)e {[self qrxButton:e down:NO];}
- (void)rightMouseDown:(NSEvent *)e {[self qrxButton:e down:YES];}
- (void)rightMouseUp:(NSEvent *)e {[self qrxButton:e down:NO];}
- (void)otherMouseDown:(NSEvent *)e {[self qrxButton:e down:YES];}
- (void)otherMouseUp:(NSEvent *)e {[self qrxButton:e down:NO];}
- (void)scrollWheel:(NSEvent *)e {
    int amount=(int)lround(e.scrollingDeltaY);
    qrx_media_push_input(self.platform->media,(QRX_INPUT_SCROLL<<24)|((uint16_t)amount));
}
@end

@interface QRXAppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation QRXAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {(void)app;return YES;}
@end

int main(int argc,const char **argv) {
    (void)argc;(void)argv;
    @autoreleasepool {
        QRXPlatform p={0};
        pthread_mutex_init(&p.frame_lock,NULL);
        p.rgba=calloc((size_t)QRX_GFX_PITCH,QRX_GFX_HEIGHT);
        if(!p.rgba){fprintf(stderr,"Out of memory\n");return 1;}
        p.demo=qrx_demo_create(qrx_video_present,&p);
        if(!p.demo){fprintf(stderr,"Cannot create QRX68100 media demo\n");free(p.rgba);return 1;}
        p.media=qrx_demo_media(p.demo);
        if(!qrx_accel_init(&p.accel)){fprintf(stderr,"GPU command stream initialization failed\n");qrx_demo_destroy(p.demo);free(p.rgba);return 1;}
        p.media->accel=&p.accel;
        const char *socketPath=getenv("ARMITHLON_QRX86_SOCKET");
        if(socketPath && *socketPath){
            size_t len=strlen(socketPath);
            if(len>=sizeof(p.qrx86Socket)||!qrx_accel_init(&p.qrx86Accel)) {
                fprintf(stderr,"Invalid QRX86 socket path or stream initialization failed\n");return 1;
            }
            memcpy(p.qrx86Socket,socketPath,len+1);
            const char *inputPath=getenv("ARMITHLON_QRX86_INPUT_SOCKET");
            if(inputPath && strlen(inputPath)<sizeof(p.qrx86InputSocket))
                memcpy(p.qrx86InputSocket,inputPath,strlen(inputPath)+1);
            p.qrx86Mode=YES;
        }
        id<MTLDevice> gpu=MTLCreateSystemDefaultDevice();
        if(!gpu){fprintf(stderr,"No Metal GPU available\n");qrx_demo_destroy(p.demo);free(p.rgba);return 1;}
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        QRXAppDelegate *delegate=[[QRXAppDelegate alloc]init];
        [NSApp setDelegate:delegate];
        NSRect rect=NSMakeRect(0,0,1024,618);
        NSWindow *window=[[NSWindow alloc]initWithContentRect:rect
          styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
          backing:NSBackingStoreBuffered defer:NO];
        [window setTitle:p.qrx86Mode?@"ARMITHLON 0.8 — QRX86 Win32 GDI/DirectDraw → Metal":@"ARMITHLON 0.8 — QRX68100 / native Metal GPU pipeline"];
        [window center];
        QRXDisplayView *display=[[QRXDisplayView alloc]initWithFrame:window.contentView.bounds device:gpu];
        if(!display){fprintf(stderr,"Metal initialization failed\n");qrx_demo_destroy(p.demo);free(p.rgba);return 1;}
        display.platform=&p;
        display.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
        [window.contentView addSubview:display];
        NSTextField *hud=[NSTextField labelWithString:@"ARMITHLON 0.8   •   echte Metal 2D/3D Draw Calls / 68k-MMIO / CoreAudio / Cocoa   •   Bewege Maus / drücke Tasten   •   kein AROS-Boot"];
        hud.frame=NSMakeRect(12,12,1000,25);
        hud.autoresizingMask=NSViewWidthSizable|NSViewMaxYMargin;
        hud.textColor=[NSColor whiteColor];
        hud.font=[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium];
        hud.drawsBackground=NO;
        [window.contentView addSubview:hud];
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:display];
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
        atomic_store(&p.running,true);
        if(p.qrx86Mode) {
            if(pthread_create(&p.qrx86Worker,NULL,qrx86_socket_worker,&p)!=0){fprintf(stderr,"QRX86 socket thread failed\n");return 1;}
            fprintf(stderr,"QRX86 GPU socket: %s\n",p.qrx86Socket);
        }
        if(pthread_create(&p.worker,NULL,qrx_worker,&p)!=0){fprintf(stderr,"Cannot start CPU thread\n");return 1;}
        if(!qrx_start_audio(&p))fprintf(stderr,"Audio unavailable, continuing with video + input\n");
        NSLog(@"ARMITHLON macOS developer preview started (guest MMIO, 800x450)");
        [NSApp run];
        atomic_store(&p.running,false);
        pthread_join(p.worker,NULL);
        if(p.qrx86Mode)pthread_join(p.qrx86Worker,NULL);
        if(p.queue){AudioQueueStop(p.queue,true);AudioQueueDispose(p.queue,true);}
        p.media->accel=NULL;
        if(p.qrx86Mode)qrx_accel_destroy(&p.qrx86Accel);
        qrx_accel_destroy(&p.accel);
        qrx_demo_destroy(p.demo);
        pthread_mutex_destroy(&p.frame_lock);
        free(p.rgba);
    }
    return 0;
}
