/* See qrx_hostgpu.h. One command buffer per frame; draw calls merge whenever the
 * state did not change, so a game's cost is mostly its vertices. QRX_HG_CAPTURE=path
 * and QRX_HG_CAPTURE_FRAME=n write that frame as a PPM. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "qrx_hostgpu.h"
#include <simd/simd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VB_BYTES (64u*1024u*1024u)
#define VB_COUNT 3u
#define MAX_TEXTURES 16384u
typedef struct {
    uint8_t env,alpha_func,fog,textured;
    float alpha_ref;
    float fog_rgb[3];
    float fog_start,fog_end;
} FragUniforms;
static NSString *const hgShaders=@""
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VI{float4 p[[attribute(0)]];float2 t[[attribute(1)]];float4 c[[attribute(2)]];};\n"
"struct VO{float4 p[[position]];float2 t;float4 c;float depth;};\n"
"vertex VO vs(VI i[[stage_in]]){VO o;o.p=i.p;o.t=i.t;o.c=i.c;o.depth=i.p.w;return o;}\n"
"struct U{uchar env,afunc,fog,textured;float aref;packed_float3 frgb;float fstart,fend;};\n"
"fragment float4 fs(VO in[[stage_in]],constant U&u[[buffer(0)]],texture2d<float> tex[[texture(0)]],sampler s[[sampler(0)]]){\n"
" float4 c=in.c;\n"
" if(u.textured){float4 t=tex.sample(s,in.t);\n"
"  if(u.env==1)c=t;\n"
"  else if(u.env==2)c=float4(mix(c.rgb,t.rgb,t.a),c.a);\n"
"  else c*=t;}\n"
" switch(u.afunc){case 0:discard_fragment();break;case 1:if(!(c.a<u.aref))discard_fragment();break;case 2:if(c.a!=u.aref)discard_fragment();break;\n"
"  case 3:if(!(c.a<=u.aref))discard_fragment();break;case 4:if(!(c.a>u.aref))discard_fragment();break;case 5:if(c.a==u.aref)discard_fragment();break;\n"
"  case 6:if(!(c.a>=u.aref))discard_fragment();break;default:break;}\n"
" if(u.fog){float f=clamp((u.fend-in.depth)/(u.fend-u.fstart),0.0,1.0);c.rgb=mix(float3(u.frgb),c.rgb,f);}\n"
" return c;\n"
"}\n"
"vertex float4 bvs(uint i[[vertex_id]],constant float4&fit[[buffer(0)]]){float2 p=float2((i<<1)&2,i&2);return float4((p*2.0-1.0)*fit.xy,0,1);}\n"
"fragment float4 bfs(float4 pos[[position]],constant float4&r[[buffer(0)]],texture2d<float> tex[[texture(0)]]){constexpr sampler s(filter::linear);return tex.sample(s,(pos.xy-r.xy)/r.zw);}\n";

typedef struct { id<MTLTexture> tex; id<MTLSamplerState> sam; } HGTex;
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLLibrary> lib;
static id<MTLBuffer> vbufs[VB_COUNT];
static dispatch_semaphore_t inflight;
static unsigned vb_index;
static size_t vb_used;
static id<MTLTexture> target,depthTex,whiteTex;
static id<MTLSamplerState> defaultSampler;
static unsigned tw,th;
static id<MTLCommandBuffer> cb;
static id<MTLRenderCommandEncoder> enc;
static bool pendingColor,pendingDepth;
static MTLClearColor clearColor;
static double clearDepth=1.0;
static QRXHGState cur;
static bool curValid;
static size_t batchStart;
static unsigned batchCount;
static NSMutableDictionary<NSNumber*,id<MTLRenderPipelineState>> *pipelines;
static NSMutableDictionary<NSNumber*,id<MTLDepthStencilState>> *depthStates;
static HGTex textures[MAX_TEXTURES];
static uint32_t nextTexture=1;
static id<MTLRenderPipelineState> blitPipeline;
static CAMetalLayer *layer;
static uint64_t frames;
static _Atomic uint64_t lastPresentUs;

void qrx_hg_state_default(QRXHGState *s){
    memset(s,0,sizeof(*s));
    s->src=HG_BLEND_ONE;s->dst=HG_BLEND_ZERO;s->depth_func=HG_CMP_LESS;s->depth_write=true;
    s->alpha_func=HG_CMP_ALWAYS;s->color_write=true;s->front_ccw=true;
}
static uint64_t now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;}
static MTLBlendFactor factor(uint8_t f){
    switch(f){
    case HG_BLEND_ZERO:return MTLBlendFactorZero;
    case HG_BLEND_ONE:return MTLBlendFactorOne;
    case HG_BLEND_SRC_COLOR:return MTLBlendFactorSourceColor;
    case HG_BLEND_INV_SRC_COLOR:return MTLBlendFactorOneMinusSourceColor;
    case HG_BLEND_SRC_ALPHA:return MTLBlendFactorSourceAlpha;
    case HG_BLEND_INV_SRC_ALPHA:return MTLBlendFactorOneMinusSourceAlpha;
    case HG_BLEND_DST_ALPHA:return MTLBlendFactorDestinationAlpha;
    case HG_BLEND_INV_DST_ALPHA:return MTLBlendFactorOneMinusDestinationAlpha;
    case HG_BLEND_DST_COLOR:return MTLBlendFactorDestinationColor;
    default:return MTLBlendFactorOneMinusDestinationColor;
    }
}
static id<MTLRenderPipelineState> pipeline_for(const QRXHGState *s){
    uint32_t key=(s->blend?1u:0u)|(uint32_t)s->src<<1|(uint32_t)s->dst<<5|(s->color_write?1u<<9:0u);
    id<MTLRenderPipelineState> p=pipelines[@(key)];
    if(p)return p;
    MTLVertexDescriptor *vd=[MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format=MTLVertexFormatFloat4;vd.attributes[0].offset=0;vd.attributes[0].bufferIndex=0;
    vd.attributes[1].format=MTLVertexFormatFloat2;vd.attributes[1].offset=16;vd.attributes[1].bufferIndex=0;
    vd.attributes[2].format=MTLVertexFormatFloat4;vd.attributes[2].offset=24;vd.attributes[2].bufferIndex=0;
    vd.layouts[0].stride=sizeof(QRXHGVertex);
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=[lib newFunctionWithName:@"vs"];d.fragmentFunction=[lib newFunctionWithName:@"fs"];
    d.vertexDescriptor=vd;
    d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
    d.colorAttachments[0].writeMask=s->color_write?MTLColorWriteMaskAll:MTLColorWriteMaskNone;
    if(s->blend){
        d.colorAttachments[0].blendingEnabled=YES;
        d.colorAttachments[0].sourceRGBBlendFactor=d.colorAttachments[0].sourceAlphaBlendFactor=factor(s->src);
        d.colorAttachments[0].destinationRGBBlendFactor=d.colorAttachments[0].destinationAlphaBlendFactor=factor(s->dst);
    }
    NSError *e=nil;
    p=[dev newRenderPipelineStateWithDescriptor:d error:&e];
    if(!p){fprintf(stderr,"[HG] pipeline: %s\n",e.localizedDescription.UTF8String);return nil;}
    pipelines[@(key)]=p;
    return p;
}
static id<MTLDepthStencilState> depth_for(const QRXHGState *s){
    uint32_t key=(s->depth_test?1u:0u)|(s->depth_write?2u:0u)|(uint32_t)s->depth_func<<2;
    id<MTLDepthStencilState> o=depthStates[@(key)];
    if(o)return o;
    static const MTLCompareFunction f[]={MTLCompareFunctionNever,MTLCompareFunctionLess,MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,
        MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};
    MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];
    d.depthCompareFunction=s->depth_test?f[s->depth_func&7u]:MTLCompareFunctionAlways;
    d.depthWriteEnabled=s->depth_test&&s->depth_write;
    o=[dev newDepthStencilStateWithDescriptor:d];
    depthStates[@(key)]=o;
    return o;
}
bool qrx_hg_open(unsigned width,unsigned height){@autoreleasepool{
    if(!dev){
        dev=MTLCreateSystemDefaultDevice();
        if(!dev)return false;
        queue=[dev newCommandQueue];
        NSError *e=nil;
        lib=[dev newLibraryWithSource:hgShaders options:nil error:&e];
        if(!lib){fprintf(stderr,"[HG] shaders: %s\n",e.localizedDescription.UTF8String);dev=nil;return false;}
        for(unsigned i=0;i<VB_COUNT;i++)vbufs[i]=[dev newBufferWithLength:VB_BYTES options:MTLResourceStorageModeShared];
        inflight=dispatch_semaphore_create(VB_COUNT);
        pipelines=[NSMutableDictionary new];depthStates=[NSMutableDictionary new];
        MTLTextureDescriptor *wd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        whiteTex=[dev newTextureWithDescriptor:wd];
        uint32_t white=0xFFFFFFFFu;[whiteTex replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
        MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];
        sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest;
        defaultSampler=[dev newSamplerStateWithDescriptor:sd];
        MTLRenderPipelineDescriptor *bd=[MTLRenderPipelineDescriptor new];
        bd.vertexFunction=[lib newFunctionWithName:@"bvs"];bd.fragmentFunction=[lib newFunctionWithName:@"bfs"];
        bd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        blitPipeline=[dev newRenderPipelineStateWithDescriptor:bd error:&e];
    }
    if(width==tw&&height==th&&target)return true;
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;td.storageMode=MTLStorageModeShared;
    target=[dev newTextureWithDescriptor:td];
    MTLTextureDescriptor *dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width height:height mipmapped:NO];
    dd.usage=MTLTextureUsageRenderTarget;dd.storageMode=MTLStorageModePrivate;
    depthTex=[dev newTextureWithDescriptor:dd];
    tw=width;th=height;
    pendingColor=pendingDepth=true;
    clearColor=MTLClearColorMake(0,0,0,1);clearDepth=1.0;
    return target&&depthTex;
}}
static void flush_batch(void){
    if(!batchCount||!enc)return;
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:batchStart/sizeof(QRXHGVertex) vertexCount:batchCount];
    batchStart+=batchCount*sizeof(QRXHGVertex);
    batchCount=0;
}
static void end_pass(void){
    flush_batch();
    if(enc){[enc endEncoding];enc=nil;}
    curValid=false;
}
static void begin_pass(void){
    if(enc)return;
    if(!cb){
        dispatch_semaphore_wait(inflight,DISPATCH_TIME_FOREVER);
        cb=[queue commandBuffer];
        vb_index=(vb_index+1u)%VB_COUNT;
        vb_used=0;batchStart=0;
    }
    MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture=target;
    rp.colorAttachments[0].loadAction=pendingColor?MTLLoadActionClear:MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor=clearColor;
    rp.colorAttachments[0].storeAction=MTLStoreActionStore;
    rp.depthAttachment.texture=depthTex;
    rp.depthAttachment.loadAction=pendingDepth?MTLLoadActionClear:MTLLoadActionLoad;
    rp.depthAttachment.clearDepth=clearDepth;
    rp.depthAttachment.storeAction=MTLStoreActionStore;
    pendingColor=pendingDepth=false;
    enc=[cb renderCommandEncoderWithDescriptor:rp];
    [enc setVertexBuffer:vbufs[vb_index] offset:0 atIndex:0];
    curValid=false;
}
void qrx_hg_clear(bool color,bool depth,float r,float g,float b,float a,float z){
    if(!target)return;
    end_pass();
    /* A pass that has not started just takes the new clear values. */
    if(color){pendingColor=true;clearColor=MTLClearColorMake(r,g,b,a);}
    if(depth){pendingDepth=true;clearDepth=z;}
}
static bool state_same(const QRXHGState *a,const QRXHGState *b){return !memcmp(a,b,sizeof(*a));}
static void apply_state(const QRXHGState *s){
    id<MTLRenderPipelineState> p=pipeline_for(s);
    if(p)[enc setRenderPipelineState:p];
    [enc setDepthStencilState:depth_for(s)];
    [enc setCullMode:s->cull==HG_CULL_FRONT?MTLCullModeFront:s->cull==HG_CULL_BACK?MTLCullModeBack:MTLCullModeNone];
    [enc setFrontFacingWinding:s->front_ccw?MTLWindingCounterClockwise:MTLWindingClockwise];
    MTLViewport vp={0,0,tw,th,0,1};
    if(s->vw>0&&s->vh>0){vp.originX=s->vx;vp.originY=s->vy;vp.width=s->vw;vp.height=s->vh;}
    [enc setViewport:vp];
    FragUniforms u;
    memset(&u,0,sizeof(u));
    id<MTLTexture> t=whiteTex;id<MTLSamplerState> sam=defaultSampler;
    if(s->texture&&s->texture<MAX_TEXTURES&&textures[s->texture].tex){t=textures[s->texture].tex;sam=textures[s->texture].sam;u.textured=1;}
    u.env=s->env;
    u.alpha_func=s->alpha_test?s->alpha_func:HG_CMP_ALWAYS;
    u.alpha_ref=s->alpha_ref;
    u.fog=s->fog;memcpy(u.fog_rgb,s->fog_rgb,sizeof(u.fog_rgb));u.fog_start=s->fog_start;u.fog_end=s->fog_end;
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [enc setFragmentTexture:t atIndex:0];
    [enc setFragmentSamplerState:sam atIndex:0];
    cur=*s;curValid=true;
}
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
static void *framePool;
static void submit(id<CAMetalDrawable> drawable){
    if(drawable)[cb presentDrawable:drawable];
    dispatch_semaphore_t sem=inflight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b){(void)b;dispatch_semaphore_signal(sem);}];
    [cb commit];
}
void qrx_hg_triangles(const QRXHGState *s,const QRXHGVertex *v,unsigned count){
    if(!target||!count)return;
    size_t bytes=(size_t)count*sizeof(QRXHGVertex);
    if(bytes>VB_BYTES)return;
    if(cb&&vb_used+bytes>VB_BYTES){  /* a very large frame: hand over what exists */
        end_pass();
        submit(nil);
        cb=nil;
    }
    if(!framePool)framePool=objc_autoreleasePoolPush();
    begin_pass();
    if(!curValid||!state_same(&cur,s)){
        flush_batch();
        apply_state(s);
    }
    memcpy((uint8_t*)vbufs[vb_index].contents+vb_used,v,bytes);
    vb_used+=bytes;
    batchCount+=count;
}
static void capture_frame(void){
    static int want=-2;
    if(want==-2){const char *f=getenv("QRX_HG_CAPTURE_FRAME");want=getenv("QRX_HG_CAPTURE")?(f?atoi(f):1):-1;}
    if(want<0||frames!=(uint64_t)want)return;
    [cb waitUntilCompleted];
    FILE *o=fopen(getenv("QRX_HG_CAPTURE"),"wb");
    uint8_t *px=malloc((size_t)tw*th*4);
    if(o&&px){
        [target getBytes:px bytesPerRow:tw*4 fromRegion:MTLRegionMake2D(0,0,tw,th) mipmapLevel:0];
        fprintf(o,"P6\n%u %u\n255\n",tw,th);
        for(size_t i=0;i<(size_t)tw*th;i++)fwrite(px+i*4,1,3,o);
        fprintf(stderr,"[HG] captured frame %llu, %ux%u\n",(unsigned long long)frames,tw,th);
    }
    if(o)fclose(o);
    free(px);
}
void qrx_hg_present(void){
    if(!target)return;
    if(!framePool)framePool=objc_autoreleasePoolPush();
    begin_pass();
    end_pass();
    id<CAMetalDrawable> drawable=nil;
    if(layer){
        drawable=[layer nextDrawable];
        if(drawable){
            MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture=drawable.texture;
            rp.colorAttachments[0].loadAction=MTLLoadActionClear;
            rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
            rp.colorAttachments[0].storeAction=MTLStoreActionStore;
            id<MTLRenderCommandEncoder> b=[cb renderCommandEncoderWithDescriptor:rp];
            float dw=(float)drawable.texture.width,dh=(float)drawable.texture.height;
            float scale=dw/tw<dh/th?dw/tw:dh/th;
            float w=tw*scale,h=th*scale;
            vector_float4 fit={w/dw,h/dh,0,0};
            vector_float4 rect={(dw-w)/2,(dh-h)/2,w,h};
            [b setRenderPipelineState:blitPipeline];
            [b setVertexBytes:&fit length:sizeof(fit) atIndex:0];
            [b setFragmentBytes:&rect length:sizeof(rect) atIndex:0];
            [b setFragmentTexture:target atIndex:0];
            [b drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [b endEncoding];
        }
    }
    submit(drawable);
    frames++;
    lastPresentUs=now_us();
    capture_frame();
    cb=nil;
    objc_autoreleasePoolPop(framePool);
    framePool=NULL;
}
uint64_t qrx_hg_frames(void){return frames;}
void qrx_hg_set_layer(void *l){layer=(__bridge CAMetalLayer*)l;}
bool qrx_hg_active(void){uint64_t t=lastPresentUs;return t&&now_us()-t<500000u;}
static id<MTLSamplerState> sampler_for(bool linear,bool wrap_s,bool wrap_t){
    static id<MTLSamplerState> cache[8];
    unsigned k=(linear?1u:0u)|(wrap_s?2u:0u)|(wrap_t?4u:0u);
    if(!cache[k]){
        MTLSamplerDescriptor *d=[MTLSamplerDescriptor new];
        d.minFilter=d.magFilter=linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
        d.sAddressMode=wrap_s?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeClampToEdge;
        d.tAddressMode=wrap_t?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeClampToEdge;
        cache[k]=[dev newSamplerStateWithDescriptor:d];
    }
    return cache[k];
}
uint32_t qrx_hg_texture(uint32_t id,unsigned w,unsigned h,const void *rgba,bool linear,bool wrap_s,bool wrap_t){@autoreleasepool{
    if(!dev&&!qrx_hg_open(640,480))return 0;
    if(!id){
        if(nextTexture>=MAX_TEXTURES)return 0;
        id=nextTexture++;
    }
    if(id>=MAX_TEXTURES||!w||!h)return 0;
    HGTex *t=&textures[id];
    if(!t->tex||t->tex.width!=w||t->tex.height!=h){
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        d.usage=MTLTextureUsageShaderRead;
        t->tex=[dev newTextureWithDescriptor:d];
        if(!t->tex)return 0;
    }
    if(rgba)[t->tex replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:rgba bytesPerRow:w*4];
    t->sam=sampler_for(linear,wrap_s,wrap_t);
    curValid=false;
    return id;
}}
void qrx_hg_texture_sub(uint32_t id,unsigned x,unsigned y,unsigned w,unsigned h,const void *rgba){
    if(id>=MAX_TEXTURES||!textures[id].tex||!w||!h)return;
    if(x+w>textures[id].tex.width||y+h>textures[id].tex.height)return;
    [textures[id].tex replaceRegion:MTLRegionMake2D(x,y,w,h) mipmapLevel:0 withBytes:rgba bytesPerRow:w*4];
}
void qrx_hg_texture_sampler(uint32_t id,bool linear,bool wrap_s,bool wrap_t){
    if(id>=MAX_TEXTURES||!textures[id].tex)return;
    textures[id].sam=sampler_for(linear,wrap_s,wrap_t);
    curValid=false;
}
void qrx_hg_texture_free(uint32_t id){
    if(id<MAX_TEXTURES){textures[id].tex=nil;textures[id].sam=nil;curValid=false;}
}
