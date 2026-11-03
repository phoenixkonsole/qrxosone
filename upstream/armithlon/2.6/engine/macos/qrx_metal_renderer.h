#import <MetalKit/MetalKit.h>
#include "qrx_accel.h"
/* GPU draw path for validated QRXGPU + QRX3D command frames.
 * Display dimensions fixed to QRX GFX ABI; host window may be resized. */
@interface QRXMetalRenderer : NSObject
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                               pixelFormat:(MTLPixelFormat)pixelFormat
                               depthFormat:(MTLPixelFormat)depthFormat;
- (BOOL)drawFrame:(const QRXAccelFrame *)frame onView:(MTKView *)view
       commandQueue:(id<MTLCommandQueue>)queue;
/* For targets without an MTKView. A nil drawable leaves presenting to nobody;
 * wait makes the pixels readable as soon as this returns. */
- (BOOL)drawFrame:(const QRXAccelFrame *)frame pass:(MTLRenderPassDescriptor *)base
           drawable:(nullable id<CAMetalDrawable>)drawable
       commandQueue:(id<MTLCommandQueue>)queue wait:(BOOL)wait;
@end
