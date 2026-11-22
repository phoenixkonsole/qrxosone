#import "qrx_metal_renderer.h"
#import <Metal/Metal.h>
#include <string.h>

/* All vertices submitted by ARMITHLON are tightly packed floats. Their origin
 * is the upper-left of the 800x450 virtual screen, x rightwards, y downwards. */
static NSString *const kNative3D =
@"#include <metal_stdlib>\n"
 @"using namespace metal;\n"
 @"struct V {float x,y,z,u,v,r,g,b,a;};\n"
 @"struct O {float4 position [[position]]; float2 uv; float4 color;};\n"
 @"vertex O qrx_v(uint i [[vertex_id]], constant V *vs [[buffer(0)]]) {\n"
 @"  V v=vs[i]; O o; o.position=float4(v.x*(2.0/800.0)-1.0,1.0-v.y*(2.0/450.0),v.z,1);\n"
 @"  o.uv=float2(v.u,v.v);o.color=float4(v.r,v.g,v.b,v.a);return o;}\n"
 @"fragment float4 qrx_solid(O i [[stage_in]]) {return i.color;}\n"
 @"fragment float4 qrx_image(O i [[stage_in]],texture2d<float> img [[texture(0)]], sampler s [[sampler(0)]]) {return img.sample(s,i.uv);}\n";

@interface QRXMetalRenderer () {
    id<MTLRenderPipelineState> _pipelines[2][2]; /* tex? x blend? */
    id<MTLDepthStencilState> _depthStates[4];  /* test, write */
}
@property(nonatomic,strong) id<MTLDevice> gpu;
@property(nonatomic,strong) id<MTLSamplerState> sampler;
@end
@implementation QRXMetalRenderer
- (instancetype)initWithDevice:(id<MTLDevice>)device pixelFormat:(MTLPixelFormat)pixelFormat depthFormat:(MTLPixelFormat)depthFormat {
    self=[super init];if(!self)return nil;
    self.gpu=device;
    NSError *err=nil;
    id<MTLLibrary> lib=[device newLibraryWithSource:kNative3D options:nil error:&err];
    if(!lib){NSLog(@"QRXMetal: shader compilation: %@",err);return nil;}
    id<MTLFunction> vert=[lib newFunctionWithName:@"qrx_v"];
    for(int textured=0;textured<2;textured++)for(int blending=0;blending<2;blending++) {
        MTLRenderPipelineDescriptor *p=[[MTLRenderPipelineDescriptor alloc]init];
        p.vertexFunction=vert;
        p.fragmentFunction=[lib newFunctionWithName:textured?@"qrx_image":@"qrx_solid"];
        p.colorAttachments[0].pixelFormat=pixelFormat;
        p.depthAttachmentPixelFormat=depthFormat;
        p.colorAttachments[0].blendingEnabled=(blending!=0);
        if(blending) {
            p.colorAttachments[0].rgbBlendOperation=MTLBlendOperationAdd;
            p.colorAttachments[0].alphaBlendOperation=MTLBlendOperationAdd;
            p.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
            p.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
            p.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
            p.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        }
        _pipelines[textured][blending]=[device newRenderPipelineStateWithDescriptor:p error:&err];
        if(!_pipelines[textured][blending]){NSLog(@"QRXMetal pipeline: %@",err);return nil;}
    }
    for(int test=0;test<2;test++)for(int write=0;write<2;write++) {
        MTLDepthStencilDescriptor *d=[[MTLDepthStencilDescriptor alloc]init];
        d.depthCompareFunction=test?MTLCompareFunctionLess:MTLCompareFunctionAlways;
        d.depthWriteEnabled=write!=0;
        _depthStates[test*2+write]=[device newDepthStencilStateWithDescriptor:d];
        if(!_depthStates[test*2+write])return nil;
    }
    MTLSamplerDescriptor *sd=[[MTLSamplerDescriptor alloc]init];
    sd.minFilter=MTLSamplerMinMagFilterNearest;sd.magFilter=MTLSamplerMinMagFilterNearest;
    sd.sAddressMode=MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
    self.sampler=[device newSamplerStateWithDescriptor:sd];
    return self;
}
static void quad(QRXAccelVertex out[6],float x,float y,float w,float h,
                 uint32_t color,float z) {
    const float xx[]={x,x+w,x,x+w,x+w,x};
    const float yy[]={y,y,y+h,y,y+h,y+h};
    const float uu[]={0,1,0,1,1,0};
    const float vv[]={0,0,1,0,1,1};
    const float r=((color>>16)&255)/255.f,g=((color>>8)&255)/255.f;
    const float b=(color&255)/255.f,a=((color>>24)&255)/255.f;
    for(int i=0;i<6;i++)out[i]=(QRXAccelVertex){xx[i],yy[i],z,uu[i],vv[i],r,g,b,a};
}
- (BOOL)drawFrame:(const QRXAccelFrame *)frame onView:(MTKView *)view commandQueue:(id<MTLCommandQueue>)queue {
    id<CAMetalDrawable> drawable=view.currentDrawable;
    MTLRenderPassDescriptor *base=view.currentRenderPassDescriptor;
    if(!drawable||!base)return NO;
    return [self drawFrame:frame pass:base drawable:drawable commandQueue:queue wait:NO];
}
- (BOOL)drawFrame:(const QRXAccelFrame *)frame pass:(MTLRenderPassDescriptor *)base
           drawable:(id<CAMetalDrawable>)drawable
       commandQueue:(id<MTLCommandQueue>)queue wait:(BOOL)wait {
    if(!frame || !queue || !base)return NO;
    id<MTLCommandBuffer> cb=[queue commandBuffer];
    if(!cb)return NO;
    id<MTLRenderCommandEncoder> encoder=nil;
    BOOL nextClear=YES;
    MTLClearColor clear=MTLClearColorMake(0.015,0.025,0.045,1);
    double clearDepth=1.0;
    for(uint32_t i=0;i<frame->count;i++){
        const QRXAccelCmd *cmd=&frame->commands[i];
        if(cmd->kind==QRX_AC_CLEAR){
            if(encoder){[encoder endEncoding];encoder=nil;}
            clear=MTLClearColorMake(((cmd->color>>16)&255)/255.,((cmd->color>>8)&255)/255.,(cmd->color&255)/255.,1);
            clearDepth=cmd->depth/65535.0;
            nextClear=YES;
            continue;
        }
        if(!encoder){
            MTLRenderPassDescriptor *desc=[base copy];
            desc.colorAttachments[0].loadAction=nextClear?MTLLoadActionClear:MTLLoadActionLoad;
            desc.colorAttachments[0].clearColor=clear;
            if(desc.depthAttachment.texture){
                desc.depthAttachment.loadAction=nextClear?MTLLoadActionClear:MTLLoadActionLoad;
                desc.depthAttachment.clearDepth=clearDepth;
                desc.depthAttachment.storeAction=MTLStoreActionStore;
            }
            desc.colorAttachments[0].storeAction=MTLStoreActionStore;
            encoder=[cb renderCommandEncoderWithDescriptor:desc];
            if(!encoder)return NO;
            nextClear=NO;
        }
        const BOOL triangle=cmd->kind==QRX_AC_TRIANGLE;
        const BOOL image=cmd->kind==QRX_AC_IMAGE||(triangle&&cmd->image);
        const BOOL blend=triangle&&(cmd->flags & (1u<<2));
        const unsigned depthTest=triangle&&((cmd->flags & 1u)!=0);
        const unsigned depthWrite=triangle&&((cmd->flags & 2u)!=0);
        QRXAccelVertex vertices[6];
        unsigned count=6;
        if(triangle){memcpy(vertices,cmd->vert,sizeof(cmd->vert));count=3;}
        else if(cmd->kind==QRX_AC_RECT||cmd->kind==QRX_AC_IMAGE){
            quad(vertices,(float)cmd->x,(float)cmd->y,(float)cmd->w,(float)cmd->h,cmd->color,0);
        }else continue;
        [encoder setRenderPipelineState:_pipelines[image?1:0][blend?1:0]];
        [encoder setDepthStencilState:_depthStates[depthTest*2+depthWrite]];
        [encoder setVertexBytes:vertices length:count*sizeof(QRXAccelVertex) atIndex:0];
        if(image){
            MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                           width:cmd->w height:cmd->h mipmapped:NO];
            td.usage=MTLTextureUsageShaderRead;
            id<MTLTexture> t=[self.gpu newTextureWithDescriptor:td];
            if(!t)return NO;
            [t replaceRegion:MTLRegionMake2D(0,0,cmd->w,cmd->h) mipmapLevel:0
                       withBytes:cmd->image bytesPerRow:cmd->pitch];
            [encoder setFragmentTexture:t atIndex:0];
            [encoder setFragmentSamplerState:self.sampler atIndex:0];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count];
    }
    /* The final command can be CLEAR; still generate a render pass for it. */
    if(!encoder){
        MTLRenderPassDescriptor *desc=[base copy];
        desc.colorAttachments[0].loadAction=nextClear?MTLLoadActionClear:MTLLoadActionLoad;
        desc.colorAttachments[0].clearColor=clear;
        desc.colorAttachments[0].storeAction=MTLStoreActionStore;
        if(desc.depthAttachment.texture){
            desc.depthAttachment.loadAction=nextClear?MTLLoadActionClear:MTLLoadActionLoad;
            desc.depthAttachment.clearDepth=clearDepth;
            desc.depthAttachment.storeAction=MTLStoreActionStore;
        }
        encoder=[cb renderCommandEncoderWithDescriptor:desc];
    }
    [encoder endEncoding];
    if(drawable)[cb presentDrawable:drawable];
    [cb commit];
    if(wait)[cb waitUntilCompleted];
    /* Descriptor textures and setVertexBytes payloads are copied/retained by
     * Metal command buffers. This is independent of guest RAM lifetime. */
    return YES;
}
@end
