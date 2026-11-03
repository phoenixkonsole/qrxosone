/* Native view of the actual AROS RTG framebuffer. No host-rendered desktop. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AudioToolbox/AudioToolbox.h>
#import "qrx_metal_renderer.h"
#include "qrx_aros_window.h"
#include <stdatomic.h>
#include "qrxmedia.h"
#include "qrx_hostfs.h"
static QRXMediaDevice media;
static NSWindow *window;
static BOOL running;
static QRXHostFS appsDisk;
static QRXVM *guestVM;
static NSString *mountedAppsPath;
static NSUserDefaults *diskPreferences;

/* The fragment shader reads the guest's framebuffer straight from a no-copy
 * Metal buffer over guest RAM, so RTG presentation costs the CPU nothing and
 * big-endian conversion happens per pixel on the GPU. */
static NSString *const shaderSource=@""
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct V{float4 pos[[position]];float2 uv;};\n"
"vertex V vs(uint i[[vertex_id]]){float2 p=float2((i<<1)&2,i&2);V o;o.pos=float4(p*2.0-1.0,0,1);o.uv=float2(p.x,1.0-p.y);return o;}\n"
"struct U{uint fb,stride,w,h,fmt;};\n"
"fragment float4 fs(V in[[stage_in]],device const uchar *ram[[buffer(0)]],constant U&u[[buffer(1)]],constant uint *clut[[buffer(2)]]){\n"
" uint x=min(uint(in.uv.x*float(u.w)),u.w-1),y=min(uint(in.uv.y*float(u.h)),u.h-1),o=u.fb+y*u.stride,r=0,g=0,b=0;\n"
" switch(u.fmt){\n"
" case 1:{uint c=clut[ram[o+x]];r=(c>>16)&255;g=(c>>8)&255;b=c&255;break;}\n"
" case 2:{uint p=o+x*2,v=(uint(ram[p])<<8)|ram[p+1];r=((v>>11)&31)*255/31;g=((v>>5)&63)*255/63;b=(v&31)*255/31;break;}\n"
" case 3:{uint p=o+x*2,v=(uint(ram[p])<<8)|ram[p+1];r=((v>>10)&31)*255/31;g=((v>>5)&31)*255/31;b=(v&31)*255/31;break;}\n"
" case 4:{uint p=o+x*3;r=ram[p];g=ram[p+1];b=ram[p+2];break;}\n"
" case 5:{uint p=o+x*4;r=ram[p+1];g=ram[p+2];b=ram[p+3];break;}\n"
" }\n"
" return float4(float(r),float(g),float(b),255.0)/255.0;\n"
"}\n";
typedef struct {uint32_t fb,stride,w,h,fmt;} QRXPresent;
static atomic_uint_fast64_t presentFb,presentGeom;
static const uint32_t *presentClut;
static id<MTLDevice> gpu;
static id<MTLCommandQueue> gpuQueue;
static id<MTLRenderPipelineState> gpuPipeline;
static id<MTLBuffer> gpuRam;
/* Guest 3D (QRX3D door) reaches the Mac's GPU as validated commands; while they
 * keep coming they replace the RTG picture, as a Voodoo pass-through card did. */
static QRXAccelStream accelStream;
static QRXMetalRenderer *accelRenderer;
static QRXAccelFrame *accelFrame;
static NSDate *accelSeen;
static id<MTLTexture> accelDepth;
static MTLRenderPassDescriptor *accel_pass(id<MTLTexture> color){
 if(!accelDepth||accelDepth.width!=color.width||accelDepth.height!=color.height){
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:color.width height:color.height mipmapped:NO];
  d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModePrivate;
  accelDepth=[gpu newTextureWithDescriptor:d];
 }
 MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
 rp.colorAttachments[0].texture=color;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 rp.depthAttachment.texture=accelDepth;rp.depthAttachment.storeAction=MTLStoreActionDontCare;
 return rp;
}
static BOOL accel_active(void){
 QRXAccelFrame *latest=qrx_accel_take_latest(&accelStream);
 if(latest){qrx_accel_frame_release(accelFrame);accelFrame=latest;accelSeen=[NSDate date];}
 return accelFrame&&accelSeen&&-[accelSeen timeIntervalSinceNow]<0.5;
}

@interface QRXAROSView : NSView
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
- (BOOL)wantsUpdateLayer{return YES;}
- (CALayer*)makeBackingLayer{
 CAMetalLayer *l=[CAMetalLayer layer];l.device=gpu;l.pixelFormat=MTLPixelFormatBGRA8Unorm;
 l.framebufferOnly=YES;l.displaySyncEnabled=YES;
 return l;
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
/* AHI (DEVS:AHI/qrxhost.audio) fills the media ring; CoreAudio drains it. */
static AudioQueueRef audioQueue;
static void audio_callback(void *opaque,AudioQueueRef q,AudioQueueBufferRef buffer){
 (void)opaque;
 size_t frames=buffer->mAudioDataBytesCapacity/4u;
 size_t got=qrx_media_pull_audio(&media,(int16_t*)buffer->mAudioData,frames);
 if(got<frames)memset((int16_t*)buffer->mAudioData+got*2u,0,(frames-got)*4u);
 buffer->mAudioDataByteSize=(UInt32)(frames*4u);
 AudioQueueEnqueueBuffer(q,buffer,0,NULL);
}
static void audio_open(void){
 AudioStreamBasicDescription fmt={0};
 fmt.mSampleRate=QRX_AUDIO_RATE;fmt.mFormatID=kAudioFormatLinearPCM;
 fmt.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked;
 fmt.mBytesPerPacket=fmt.mBytesPerFrame=4;fmt.mFramesPerPacket=1;
 fmt.mChannelsPerFrame=2;fmt.mBitsPerChannel=16;
 OSStatus st=AudioQueueNewOutput(&fmt,audio_callback,NULL,NULL,kCFRunLoopCommonModes,0,&audioQueue);
 if(st!=noErr){fprintf(stderr,"[AHI] AudioQueue failed: %d\n",(int)st);audioQueue=NULL;return;}
 for(int i=0;i<3;i++){
  AudioQueueBufferRef b;
  if(AudioQueueAllocateBuffer(audioQueue,1024u*4u,&b)!=noErr)continue;
  audio_callback(NULL,audioQueue,b);
 }
 AudioQueueStart(audioQueue,NULL);
}
int qrx_aros_window_open(QRXVM *vm){@autoreleasepool{
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
 qrx_media_init(&media,NULL,NULL);qrx_vm_bind_media(vm,&media);
 gpu=MTLCreateSystemDefaultDevice();
 if(!gpu){fprintf(stderr,"[RTG] no Metal device\n");return 0;}
 gpuQueue=[gpu newCommandQueue];
 NSError *shaderError=nil;
 id<MTLLibrary> lib=[gpu newLibraryWithSource:shaderSource options:nil error:&shaderError];
 if(!lib){fprintf(stderr,"[RTG] shader: %s\n",shaderError.localizedDescription.UTF8String);return 0;}
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
 pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=[lib newFunctionWithName:@"fs"];
 pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 gpuPipeline=[gpu newRenderPipelineStateWithDescriptor:pd error:&shaderError];
 if(!gpuPipeline){fprintf(stderr,"[RTG] pipeline: %s\n",shaderError.localizedDescription.UTF8String);return 0;}
 gpuRam=[gpu newBufferWithBytesNoCopy:vm->ram.data length:vm->ram.size options:MTLResourceStorageModeShared deallocator:nil];
 if(!gpuRam){fprintf(stderr,"[RTG] guest RAM is not GPU-mappable\n");return 0;}
 window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,1024,768) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
 window.title=@"QRX OS One — ARMithlon / AROS RTG";window.releasedWhenClosed=NO;
 view=[[QRXAROSView alloc]initWithFrame:NSMakeRect(0,0,1024,768)];view.guestWidth=1024;view.guestHeight=768;
 view.wantsLayer=YES;
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
 if(qrx_accel_init(&accelStream)){
  accelRenderer=[[QRXMetalRenderer alloc]initWithDevice:gpu pixelFormat:MTLPixelFormatBGRA8Unorm depthFormat:MTLPixelFormatDepth32Float];
  if(accelRenderer)media.accel=&accelStream;
 }
 audio_open();
 NSMenu *bar=[NSMenu new];NSMenuItem *appItem=[NSMenuItem new];[bar addItem:appItem];
 NSMenu *appMenu=[[NSMenu alloc]initWithTitle:@"ARMithlon"];appItem.submenu=appMenu;
 NSMenuItem *choose=[[NSMenuItem alloc]initWithTitle:@"Zweites Laufwerk auswählen…" action:@selector(chooseApps:) keyEquivalent:@"o"];choose.target=delegate;[appMenu addItem:choose];
 NSMenuItem *show=[[NSMenuItem alloc]initWithTitle:@"APPS:-Ordner im Finder öffnen" action:@selector(showApps:) keyEquivalent:@""];show.target=delegate;show.enabled=mountedAppsPath!=nil;[appMenu addItem:show];
 [appMenu addItem:[NSMenuItem separatorItem]];
 NSMenuItem *quit=[[NSMenuItem alloc]initWithTitle:@"ARMithlon beenden" action:@selector(quit:) keyEquivalent:@"q"];quit.target=delegate;[appMenu addItem:quit];
 NSApp.mainMenu=bar;

 [window center];[window makeKeyAndOrderFront:nil];[window makeFirstResponder:view];[NSApp finishLaunching];[NSApp activateIgnoringOtherApps:YES];running=YES;return 1;
}}
static void present_guest(void){
 if(accel_active()){
  CAMetalLayer *layer=(CAMetalLayer*)view.layer;
  CGFloat scale=window.backingScaleFactor;NSSize size=view.bounds.size;
  CGSize want=CGSizeMake(size.width*scale,size.height*scale);
  if(!CGSizeEqualToSize(layer.drawableSize,want)){layer.drawableSize=want;layer.contentsScale=scale;}
  id<CAMetalDrawable> drawable=[layer nextDrawable];
  if(drawable)[accelRenderer drawFrame:accelFrame pass:accel_pass(drawable.texture) drawable:drawable commandQueue:gpuQueue wait:NO];
  return;
 }
 uint64_t a=atomic_load_explicit(&presentFb,memory_order_acquire),g=atomic_load_explicit(&presentGeom,memory_order_acquire);
 QRXPresent u={(uint32_t)(a>>32),(uint32_t)a,(uint32_t)(g>>32)>>16,(uint32_t)(g>>32)&0xFFFFu,(uint32_t)g};
 if(!u.w||!u.h||!u.fmt)return;
 view.guestWidth=u.w;view.guestHeight=u.h;
 CAMetalLayer *layer=(CAMetalLayer*)view.layer;
 CGFloat scale=window.backingScaleFactor;NSSize size=view.bounds.size;
 CGSize want=CGSizeMake(size.width*scale,size.height*scale);
 if(!CGSizeEqualToSize(layer.drawableSize,want)){layer.drawableSize=want;layer.contentsScale=scale;}
 id<CAMetalDrawable> drawable=[layer nextDrawable];
 if(!drawable)return;
 MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
 rp.colorAttachments[0].texture=drawable.texture;rp.colorAttachments[0].loadAction=MTLLoadActionDontCare;
 rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLCommandBuffer> cb=[gpuQueue commandBuffer];
 id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];
 [enc setRenderPipelineState:gpuPipeline];
 [enc setFragmentBuffer:gpuRam offset:0 atIndex:0];
 [enc setFragmentBytes:&u length:sizeof u atIndex:1];
 if(presentClut)[enc setFragmentBytes:presentClut length:1024 atIndex:2];
 else{static const uint32_t none[256];[enc setFragmentBytes:none length:1024 atIndex:2];}
 [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 [enc endEncoding];
 [cb presentDrawable:drawable];
 [cb commit];
}
/* Renders one frame off screen through the real shader and writes a PPM. For
 * checks without screen-recording rights: QRX_RTG_CAPTURE=path, QRX_RTG_CAPTURE_AT=seconds. */
static void capture_guest(const char *path){
 uint64_t a=atomic_load(&presentFb),g=atomic_load(&presentGeom);
 QRXPresent u={(uint32_t)(a>>32),(uint32_t)a,(uint32_t)(g>>32)>>16,(uint32_t)(g>>32)&0xFFFFu,(uint32_t)g};
 BOOL accel=accel_active();
 if(accel){u.w=800;u.h=450;}
 else if(!u.w||!u.h||!u.fmt)return;
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:u.w height:u.h mipmapped:NO];
 d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
 id<MTLTexture> tex=[gpu newTextureWithDescriptor:d];
 if(accel)[accelRenderer drawFrame:accelFrame pass:accel_pass(tex) drawable:nil commandQueue:gpuQueue wait:YES];
 else{
  MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture=tex;rp.colorAttachments[0].loadAction=MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction=MTLStoreActionStore;
  id<MTLCommandBuffer> cb=[gpuQueue commandBuffer];
  id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];
  [enc setRenderPipelineState:gpuPipeline];
  [enc setFragmentBuffer:gpuRam offset:0 atIndex:0];
  [enc setFragmentBytes:&u length:sizeof u atIndex:1];
  static const uint32_t none[256];
  [enc setFragmentBytes:presentClut?presentClut:none length:1024 atIndex:2];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [enc endEncoding];
  [cb commit];[cb waitUntilCompleted];
 }
 uint8_t *px=malloc((size_t)u.w*u.h*4);
 FILE *f=px?fopen(path,"wb"):NULL;
 if(f){
  [tex getBytes:px bytesPerRow:u.w*4 fromRegion:MTLRegionMake2D(0,0,u.w,u.h) mipmapLevel:0];
  fprintf(f,"P6\n%u %u\n255\n",u.w,u.h);
  for(size_t i=0;i<(size_t)u.w*u.h;i++){uint8_t rgb[3]={px[i*4+2],px[i*4+1],px[i*4]};fwrite(rgb,1,3,f);}
  fclose(f);
  fprintf(stderr,"[RTG] captured %ux%u fmt=%u to %s\n",u.w,u.h,u.fmt,path);
 }
 free(px);
}
int qrx_aros_window_pump(void){@autoreleasepool{
 {
  static NSDate *captureAt;static BOOL captureDone;
  const char *path=getenv("QRX_RTG_CAPTURE");
  if(path&&!captureDone){
   if(!captureAt){const char *at=getenv("QRX_RTG_CAPTURE_AT");captureAt=[NSDate dateWithTimeIntervalSinceNow:at?atof(at):20.0];}
   if([captureAt compare:[NSDate date]]==NSOrderedAscending){captureDone=YES;capture_guest(path);}
  }
 }
 /* Blocks in the event queue until the next frame is due; the guest CPU has
  * its own thread and never waits for this. */
 static NSDate *due;
 NSDate *now=[NSDate date];
 if(!due||[due compare:now]==NSOrderedAscending){present_guest();due=[now dateByAddingTimeInterval:1.0/60.0];}
 NSEvent *event;
 while((event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:due inMode:NSDefaultRunLoopMode dequeue:YES])){
  [NSApp sendEvent:event];
  if([due compare:[NSDate date]]==NSOrderedAscending)break;
 }
 [NSApp updateWindows];return running;
}}
void qrx_aros_window_publish(uint32_t fb,uint32_t stride,uint32_t w,uint32_t h,uint32_t fmt,const uint32_t *clut){
 presentClut=clut;
 atomic_store_explicit(&presentFb,((uint64_t)fb<<32)|stride,memory_order_release);
 atomic_store_explicit(&presentGeom,((uint64_t)((w<<16)|(h&0xFFFFu))<<32)|fmt,memory_order_release);
}
void qrx_aros_window_close(void){if(accelStream.submitted)fprintf(stderr,"[3D] frames submitted=%llu dropped=%llu\n",(unsigned long long)accelStream.submitted,(unsigned long long)accelStream.dropped);if(audioQueue){AudioQueueStop(audioQueue,true);AudioQueueDispose(audioQueue,true);audioQueue=NULL;}[window orderOut:nil];running=NO;if(guestVM)qrx_hostfs_attach2(guestVM,NULL);qrx_hostfs_unmount(&appsDisk);guestVM=NULL;}
