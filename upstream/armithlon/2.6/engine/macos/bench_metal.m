/* SPDX-License-Identifier: MIT
 * Original ARMITHLON 1.0 native Metal GPU diagnostic (offscreen, 800x450).
 * Measures wall time for command encoding + GPU wait, NOT isolated GPU clocks.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "qrx_bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static NSString *const source=@"#include <metal_stdlib>\n"
@"using namespace metal;\n"
@"struct V { float x,y,z,u,v,r,g,b,a; };\n"
@"struct O { float4 p [[position]]; float4 c; };\n"
@"vertex O qrx_bench_v(uint i [[vertex_id]],constant V* v [[buffer(0)]]) {\n"
@"  V x=v[i]; O o; o.p=float4(x.x*(2./800.)-1.,1.-x.y*(2./450.),x.z,1.);\n"
@"  o.c=float4(x.r,x.g,x.b,x.a);return o;}\n"
@"fragment float4 qrx_bench_f(O i [[stage_in]]) {return i.c;}\n";

static double ns_ms(void){struct timespec t={0};clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void fail(NSString *what,NSError *error){fprintf(stderr,"Metal benchmark %s: %s\n",what.UTF8String,error?error.localizedDescription.UTF8String:"unavailable");}
int main(int argc,const char **argv){
  @autoreleasepool {
    int frames=argc>1?atoi(argv[1]):90,warmup=argc>2?atoi(argv[2]):10;
    const char *image=argc>3?argv[3]:"metal-bench.ppm";
    if(frames<1||frames>2000||warmup<0||warmup>500)return 2;
    QRXAccelFrame *f=qrx_bench_scene();if(!f||f->count!=QRX_BENCH_COMMANDS)return 3;
    id<MTLDevice> gpu=MTLCreateSystemDefaultDevice();if(!gpu){fprintf(stderr,"Metal unavailable\n");return 3;}
    id<MTLCommandQueue> queue=[gpu newCommandQueue];if(!queue)return 3;
    NSError *e=nil;
    id<MTLLibrary> library=[gpu newLibraryWithSource:source options:nil error:&e];
    if(!library){fail(@"shader",e);return 3;}
    id<MTLFunction> vs=[library newFunctionWithName:@"qrx_bench_v"];
    id<MTLFunction> fs=[library newFunctionWithName:@"qrx_bench_f"];
    id<MTLRenderPipelineState> pipe[2]={nil,nil};
    for(int blend=0;blend<2;blend++){
      MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new];
      p.vertexFunction=vs;p.fragmentFunction=fs;
      p.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
      p.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
      p.colorAttachments[0].blendingEnabled=blend!=0;
      if(blend){
        p.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
        p.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        p.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
        p.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
      }
      pipe[blend]=[gpu newRenderPipelineStateWithDescriptor:p error:&e];
      if(!pipe[blend]){fail(@"pipeline",e);return 3;}
    }
    MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];
    d.depthCompareFunction=MTLCompareFunctionLess;d.depthWriteEnabled=YES;
    id<MTLDepthStencilState> depth=[gpu newDepthStencilStateWithDescriptor:d];
    d.depthCompareFunction=MTLCompareFunctionAlways;d.depthWriteEnabled=NO;
    id<MTLDepthStencilState> overlayDepth=[gpu newDepthStencilStateWithDescriptor:d];
    MTLTextureDescriptor *cd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:800 height:450 mipmapped:NO];
    cd.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    cd.storageMode=MTLStorageModeShared;
    id<MTLTexture> color=[gpu newTextureWithDescriptor:cd];
    MTLTextureDescriptor *dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:800 height:450 mipmapped:NO];
    dd.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> z=[gpu newTextureWithDescriptor:dd];
    if(!color||!z||!depth||!overlayDepth)return 3;
    QRXAccelVertex verts[3*256];unsigned nverts=0;
    for(unsigned i=1;i<f->count;i++){
        memcpy(&verts[nverts],f->commands[i].vert,sizeof(QRXAccelVertex)*3);nverts+=3;
    }
    id<MTLBuffer> vb=[gpu newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    if(!vb)return 3;
    double begin=0,end=0;
    for(int i=-warmup;i<frames;i++){
      @autoreleasepool {
        if(i==0)begin=ns_ms();
        MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture=color;
        rp.colorAttachments[0].loadAction=MTLLoadActionClear;
        rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        rp.colorAttachments[0].clearColor=MTLClearColorMake(17./255,24./255,39./255,1);
        rp.depthAttachment.texture=z;rp.depthAttachment.loadAction=MTLLoadActionClear;
        rp.depthAttachment.storeAction=MTLStoreActionDontCare;rp.depthAttachment.clearDepth=1.0;
        id<MTLCommandBuffer> cb=[queue commandBuffer];
        id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];
        if(!enc)return 4;
        [enc setVertexBuffer:vb offset:0 atIndex:0];
        for(unsigned c=1;c<f->count;c++){
          const QRXAccelCmd *cmd=&f->commands[c];
          BOOL blend=(cmd->flags&4u)!=0;
          [enc setRenderPipelineState:pipe[blend?1:0]];
          [enc setDepthStencilState:blend?overlayDepth:depth];
          [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:(c-1)*3 vertexCount:3];
        }
        [enc endEncoding];
        [cb commit];[cb waitUntilCompleted];
        if(cb.status!=MTLCommandBufferStatusCompleted){fail(@"command buffer",cb.error);return 5;}
      }
    }
    end=ns_ms();
    size_t sz=800u*450u*4u;uint8_t *pixels=malloc(sz);if(!pixels)return 3;
    [color getBytes:pixels bytesPerRow:800*4 fromRegion:MTLRegionMake2D(0,0,800,450) mipmapLevel:0];
    if(qrx_bench_save_ppm(image,pixels)){fprintf(stderr,"Cannot save %s\n",image);return 5;}
    size_t center=((size_t)225*800+400)*4;
    printf("{\"api\":\"metal\",\"kind\":\"gpu_submission_and_wait\",\"gpu\":\"%s\",\"frames\":%d,\"warmup\":%d,\"width\":800,\"height\":450,\"triangles_per_frame\":256,\"elapsed_ms\":%.3f,\"fps\":%.3f,\"center_rgb\":[%u,%u,%u],\"status\":\"PASS\"}\n",
           gpu.name.UTF8String,frames,warmup,end-begin,frames*1000./(end-begin),pixels[center],pixels[center+1],pixels[center+2]);
    free(pixels);qrx_accel_frame_release(f);
    return 0;
  }
}
