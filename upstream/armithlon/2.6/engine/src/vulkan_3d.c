/* ARMITHLON 0.9 diagnostic: actual Vulkan graphics pipeline (not transfer-only).
 * Draws QRXAccel CLEAR, RECT and TRIANGLE commands with Gouraud colors,
 * Vk depth test/write, src-alpha blending; reads back a 800x450 PPM.
 * Does not yet support images, texture sampling, Glide binary ABI or swapchain.
 * Optional: built only when Vulkan SDK + glslangValidator are detected.
 */
#include "qrx_accel.h"
#include "qrx_gl_compat.h"
#include "qrx_bench.h"
#include <time.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#define W 800u
#define H 450u
#define MAX_VERTS (QRX_ACCEL_MAX_COMMANDS*6u)
#ifndef QRX_VK_VERT_SPV
#error QRX_VK_VERT_SPV must point to the compiled SPIR-V shader
#endif
#define VKCHK(x) do {VkResult _rc=(x);if(_rc!=VK_SUCCESS){fprintf(stderr,"Vulkan3D: %s returned %d\n",#x,(int)_rc);goto done;}}while(0)
typedef struct GPU {
    VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkQueue queue;
    uint32_t family;
    VkImage color,depth;VkDeviceMemory cm,dm;VkImageView cv,dv;
    VkBuffer vertex,readback;VkDeviceMemory vm,rm;
    VkRenderPass pass;VkFramebuffer fb;
    VkShaderModule vs,fs;VkPipelineLayout pipeline_layout;VkPipeline pipeline[8];
    VkCommandPool pool;VkCommandBuffer cmds;
} GPU;
static int memory_type(VkPhysicalDevice phys,uint32_t allowed,VkMemoryPropertyFlags want){
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(phys,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)if((allowed&(1u<<i))&&
        (p.memoryTypes[i].propertyFlags&want)==want)return (int)i;
    return -1;
}
static int alloc_buffer(GPU *g,VkDeviceSize size,VkBufferUsageFlags usage,VkBuffer *buf,VkDeviceMemory *mem){
    VkBufferCreateInfo ci={0};ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size=size;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(g->device,&ci,NULL,buf)!=VK_SUCCESS)return -1;
    VkMemoryRequirements mr;vkGetBufferMemoryRequirements(g->device,*buf,&mr);
    int mt=memory_type(g->physical,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(mt<0)return -1;
    VkMemoryAllocateInfo ai={0};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize=mr.size;ai.memoryTypeIndex=(uint32_t)mt;
    if(vkAllocateMemory(g->device,&ai,NULL,mem)!=VK_SUCCESS)return -1;
    return vkBindBufferMemory(g->device,*buf,*mem,0)==VK_SUCCESS?0:-1;
}
static int alloc_image(GPU *g,VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspect,
                       VkImage *image,VkDeviceMemory *mem,VkImageView *view){
    VkImageCreateInfo ci={0};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent=(VkExtent3D){W,H,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(g->device,&ci,NULL,image)!=VK_SUCCESS)return -1;
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(g->device,*image,&mr);
    int mt=memory_type(g->physical,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(mt<0)mt=memory_type(g->physical,mr.memoryTypeBits,0);
    if(mt<0)return -1;
    VkMemoryAllocateInfo ai={0};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize=mr.size;ai.memoryTypeIndex=(uint32_t)mt;
    if(vkAllocateMemory(g->device,&ai,NULL,mem)!=VK_SUCCESS||
       vkBindImageMemory(g->device,*image,*mem,0)!=VK_SUCCESS)return -1;
    VkImageViewCreateInfo vi={0};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image=*image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange=(VkImageSubresourceRange){aspect,0,1,0,1};
    return vkCreateImageView(g->device,&vi,NULL,view)==VK_SUCCESS?0:-1;
}
static VkShaderModule load_shader(GPU *g,const char *path){
    FILE *f=fopen(path,"rb");if(!f){perror(path);return VK_NULL_HANDLE;}
    if(fseek(f,0,SEEK_END)){fclose(f);return VK_NULL_HANDLE;}
    long length=ftell(f);if(length<20||length>262144||(length&3)||fseek(f,0,SEEK_SET)){
        fclose(f);return VK_NULL_HANDLE;
    }
    uint32_t *data=malloc((size_t)length);if(!data){fclose(f);return VK_NULL_HANDLE;}
    int ok=fread(data,1,(size_t)length,f)==(size_t)length;fclose(f);
    if(!ok || data[0]!=0x07230203u){free(data);return VK_NULL_HANDLE;}
    VkShaderModule m=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci={0};ci.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize=(size_t)length;ci.pCode=data;
    if(vkCreateShaderModule(g->device,&ci,NULL,&m)!=VK_SUCCESS)m=VK_NULL_HANDLE;
    free(data);return m;
}
static void release_gpu(GPU *g){
    if(g->device){
        vkDeviceWaitIdle(g->device);
        if(g->pool)vkDestroyCommandPool(g->device,g->pool,NULL);
        for(int i=0;i<8;i++)if(g->pipeline[i])vkDestroyPipeline(g->device,g->pipeline[i],NULL);
        if(g->pipeline_layout)vkDestroyPipelineLayout(g->device,g->pipeline_layout,NULL);
        if(g->vs)vkDestroyShaderModule(g->device,g->vs,NULL);
        if(g->fs)vkDestroyShaderModule(g->device,g->fs,NULL);
        if(g->fb)vkDestroyFramebuffer(g->device,g->fb,NULL);
        if(g->pass)vkDestroyRenderPass(g->device,g->pass,NULL);
        if(g->cv)vkDestroyImageView(g->device,g->cv,NULL);
        if(g->dv)vkDestroyImageView(g->device,g->dv,NULL);
        if(g->color)vkDestroyImage(g->device,g->color,NULL);
        if(g->depth)vkDestroyImage(g->device,g->depth,NULL);
        if(g->vertex)vkDestroyBuffer(g->device,g->vertex,NULL);
        if(g->readback)vkDestroyBuffer(g->device,g->readback,NULL);
        if(g->cm)vkFreeMemory(g->device,g->cm,NULL);
        if(g->dm)vkFreeMemory(g->device,g->dm,NULL);
        if(g->vm)vkFreeMemory(g->device,g->vm,NULL);
        if(g->rm)vkFreeMemory(g->device,g->rm,NULL);
        vkDestroyDevice(g->device,NULL);
    }
    if(g->instance)vkDestroyInstance(g->instance,NULL);
}
static int init_gpu(GPU *g){
    VkApplicationInfo app={0};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="ARMITHLON 0.9 Vulkan QRX3D";app.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo ic={0};ic.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ic.pApplicationInfo=&app;
    /* Portability drivers on macOS are not enumerated without this extension. */
    uint32_t n=0;VkExtensionProperties *ext=NULL;int portability=0;
    if(vkEnumerateInstanceExtensionProperties(NULL,&n,NULL)==VK_SUCCESS&&n<=1024){
        ext=calloc(n?n:1,sizeof(*ext));
        if(ext&&vkEnumerateInstanceExtensionProperties(NULL,&n,ext)==VK_SUCCESS)
            for(uint32_t i=0;i<n;i++)if(!strcmp(ext[i].extensionName,"VK_KHR_portability_enumeration"))portability=1;
    }
    free(ext);
    const char *port_ext="VK_KHR_portability_enumeration";
    if(portability){ic.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&port_ext;}
    VKCHK(vkCreateInstance(&ic,NULL,&g->instance));
    n=0;VKCHK(vkEnumeratePhysicalDevices(g->instance,&n,NULL));
    if(!n || n>64)goto done;
    VkPhysicalDevice physicals[64];VKCHK(vkEnumeratePhysicalDevices(g->instance,&n,physicals));
    for(uint32_t i=0;i<n&&!g->physical;i++){
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(physicals[i],&count,NULL);
        if(count>128)continue;
        VkQueueFamilyProperties queues[128];vkGetPhysicalDeviceQueueFamilyProperties(physicals[i],&count,queues);
        for(uint32_t j=0;j<count;j++)if(queues[j].queueCount&&(queues[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)){
            VkFormatProperties color,depth;vkGetPhysicalDeviceFormatProperties(physicals[i],VK_FORMAT_R8G8B8A8_UNORM,&color);
            vkGetPhysicalDeviceFormatProperties(physicals[i],VK_FORMAT_D32_SFLOAT,&depth);
            if(!(color.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)||
               !(depth.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))continue;
            g->physical=physicals[i];g->family=j;break;
        }
    }
    if(!g->physical){fprintf(stderr,"Vulkan3D: graphics queue or required formats unavailable\n");goto done;}
    float priority=1.f;
    VkDeviceQueueCreateInfo queue={0};queue.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex=g->family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc={0};dc.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&queue;
    uint32_t extensions=0;
    if(vkEnumerateDeviceExtensionProperties(g->physical,NULL,&extensions,NULL)==VK_SUCCESS && extensions<=1024){
        VkExtensionProperties *names=calloc(extensions?extensions:1,sizeof(*names));
        if(names && vkEnumerateDeviceExtensionProperties(g->physical,NULL,&extensions,names)==VK_SUCCESS)
            for(uint32_t i=0;i<extensions;i++)if(!strcmp(names[i].extensionName,"VK_KHR_portability_subset")){
                static const char *sub="VK_KHR_portability_subset";
                dc.enabledExtensionCount=1;dc.ppEnabledExtensionNames=&sub;break;
            }
        free(names);
    }
    VKCHK(vkCreateDevice(g->physical,&dc,NULL,&g->device));
    vkGetDeviceQueue(g->device,g->family,0,&g->queue);
    if(alloc_image(g,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
       VK_IMAGE_ASPECT_COLOR_BIT,&g->color,&g->cm,&g->cv) ||
       alloc_image(g,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
       VK_IMAGE_ASPECT_DEPTH_BIT,&g->depth,&g->dm,&g->dv))goto done;
    if(alloc_buffer(g,sizeof(QRXAccelVertex)*MAX_VERTS,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,&g->vertex,&g->vm)||
       alloc_buffer(g,(VkDeviceSize)W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&g->readback,&g->rm))goto done;
    return 0;
done:return -1;
}
static int create_pipeline(GPU *g){
    VkAttachmentDescription attachments[2]={0};
    attachments[0].format=VK_FORMAT_R8G8B8A8_UNORM;attachments[0].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format=VK_FORMAT_D32_SFLOAT;attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={0};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&depth;
    VkSubpassDependency deps[2]={0};
    deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;
    deps[0].srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp={0};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount=2;rp.pAttachments=attachments;rp.subpassCount=1;rp.pSubpasses=&sub;
    rp.dependencyCount=2;rp.pDependencies=deps;
    VKCHK(vkCreateRenderPass(g->device,&rp,NULL,&g->pass));
    VkImageView views[2]={g->cv,g->dv};
    VkFramebufferCreateInfo fb={0};fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass=g->pass;fb.attachmentCount=2;fb.pAttachments=views;fb.width=W;fb.height=H;fb.layers=1;
    VKCHK(vkCreateFramebuffer(g->device,&fb,NULL,&g->fb));
    g->vs=load_shader(g,QRX_VK_VERT_SPV);g->fs=load_shader(g,QRX_VK_FRAG_SPV);
    if(!g->vs||!g->fs)goto done;
    VkPipelineLayoutCreateInfo lc={0};lc.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VKCHK(vkCreatePipelineLayout(g->device,&lc,NULL,&g->pipeline_layout));
    VkPipelineShaderStageCreateInfo stages[2]={0};
    stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=g->vs;stages[0].pName="main";
    stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=g->fs;stages[1].pName="main";
    VkVertexInputBindingDescription bind={0,sizeof(QRXAccelVertex),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(QRXAccelVertex,x)},
          {1,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(QRXAccelVertex,r)}};
    VkPipelineVertexInputStateCreateInfo vi={0};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&bind;
    vi.vertexAttributeDescriptionCount=2;vi.pVertexAttributeDescriptions=attr;
    VkPipelineInputAssemblyStateCreateInfo assembly={0};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport={0,0,(float)W,(float)H,0,1};
    VkRect2D scissor={{0,0},{W,H}};
    VkPipelineViewportStateCreateInfo vp={0};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster={0};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;raster.cullMode=VK_CULL_MODE_NONE;
    raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms={0};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    for(uint32_t mask=0;mask<8;mask++){
        VkPipelineDepthStencilStateCreateInfo ds={0};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable=(mask&1)?VK_TRUE:VK_FALSE;
        ds.depthWriteEnable=(mask&2)?VK_TRUE:VK_FALSE;
        ds.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState attachment={0};attachment.colorWriteMask=15;
        attachment.blendEnable=(mask&4)?VK_TRUE:VK_FALSE;
        attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp=VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blending={0};blending.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount=1;blending.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo ci={0};ci.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&assembly;
        ci.pViewportState=&vp;ci.pRasterizationState=&raster;ci.pMultisampleState=&ms;
        ci.pDepthStencilState=&ds;ci.pColorBlendState=&blending;
        ci.layout=g->pipeline_layout;ci.renderPass=g->pass;ci.subpass=0;
        VKCHK(vkCreateGraphicsPipelines(g->device,VK_NULL_HANDLE,1,&ci,NULL,&g->pipeline[mask]));
    }
    return 0;
done:return -1;
}
static void quad(QRXAccelVertex out[6],const QRXAccelCmd *c){
    float r=((c->color>>16)&255u)/255.f,g=((c->color>>8)&255u)/255.f;
    float b=(c->color&255u)/255.f;
    float x=(float)c->x,y=(float)c->y,w=(float)c->w,h=(float)c->h;
    float px[6]={x,x+w,x,x+w,x+w,x},py[6]={y,y,y+h,y,y+h,y+h};
    for(int i=0;i<6;i++)out[i]=(QRXAccelVertex){px[i],py[i],0,0,0,r,g,b,1};
}
static int save_ppm(const char *path,const uint8_t *data){
    FILE *f=fopen(path,"wb");if(!f)return -1;
    int err=fprintf(f,"P6\n%u %u\n255\n",W,H)<0;
    for(uint32_t y=0;y<H;y++)for(uint32_t x=0;x<W;x++)
        if(fwrite(data+((size_t)y*W+x)*4,1,3,f)!=3)err=1;
    if(fclose(f))err=1;
    return err?-1:0;
}
static double qrx_bench_now(void){
    struct timespec t={0};clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0+t.tv_nsec/1e6;
}
static int render(const QRXAccelFrame *frame,const char *output,int repetitions,int warmup){
    if(!frame||frame->count>QRX_ACCEL_MAX_COMMANDS)return -1;
    /* Input validation is separate from Vulkan initialization. */
    size_t vertices=0;
    for(uint32_t i=0;i<frame->count;i++){
        const QRXAccelCmd *c=&frame->commands[i];
        if(c->kind==QRX_AC_CLEAR)continue;
        if(c->kind==QRX_AC_IMAGE){fprintf(stderr,"Vulkan3D: images/textures not yet supported (use Metal or Vulkan 0.8 transfer)\n");return -2;}
        if(c->kind==QRX_AC_RECT){
            if(!c->w||!c->h||c->x>=W||c->y>=H||c->w>W-c->x||c->h>H-c->y)return -1;
            vertices+=6;
        }else if(c->kind==QRX_AC_TRIANGLE){
            if(c->image||c->flags&~7u)return -1;
            for(int v=0;v<3;v++){
                const float *p=&c->vert[v].x;
                for(int n=0;n<9;n++)if(!isfinite(p[n])||fabsf(p[n])>1000000.f)return -1;
            }
            vertices+=3;
        }else return -1;
        if(vertices>MAX_VERTS)return -1;
    }
    GPU g={0};int status=-1;
    if(init_gpu(&g)||create_pipeline(&g))goto cleanup;
    void *mapped=NULL;
    VKCHK(vkMapMemory(g.device,g.vm,0,sizeof(QRXAccelVertex)*MAX_VERTS,0,&mapped));
    QRXAccelVertex *vout=mapped;size_t count=0;
    for(uint32_t i=0;i<frame->count;i++){
        const QRXAccelCmd *c=&frame->commands[i];
        if(c->kind==QRX_AC_TRIANGLE){memcpy(vout+count,c->vert,sizeof(c->vert));count+=3;}
        else if(c->kind==QRX_AC_RECT){quad(vout+count,c);count+=6;}
    }
    vkUnmapMemory(g.device,g.vm);
    VkCommandPoolCreateInfo cp={0};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex=g.family;
    VKCHK(vkCreateCommandPool(g.device,&cp,NULL,&g.pool));
    VkCommandBufferAllocateInfo ba={0};ba.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ba.commandPool=g.pool;ba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ba.commandBufferCount=1;
    VKCHK(vkAllocateCommandBuffers(g.device,&ba,&g.cmds));
    VkCommandBufferBeginInfo bi={0};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags=0; /* recorded command buffer can be re-submitted after queue completion */
    VKCHK(vkBeginCommandBuffer(g.cmds,&bi));
    VkClearValue clear[2]={0};clear[0].color.float32[3]=1;clear[1].depthStencil.depth=1;
    VkRenderPassBeginInfo rp={0};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass=g.pass;rp.framebuffer=g.fb;rp.renderArea=(VkRect2D){{0,0},{W,H}};
    rp.clearValueCount=2;rp.pClearValues=clear;
    vkCmdBeginRenderPass(g.cmds,&rp,VK_SUBPASS_CONTENTS_INLINE);
    VkDeviceSize offset=0;vkCmdBindVertexBuffers(g.cmds,0,1,&g.vertex,&offset);
    uint32_t first=0;
    for(uint32_t i=0;i<frame->count;i++){
        const QRXAccelCmd *c=&frame->commands[i];
        if(c->kind==QRX_AC_CLEAR){
            VkClearAttachment clears[2]={0};
            clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;clears[0].colorAttachment=0;
            clears[0].clearValue.color.float32[0]=((c->color>>16)&255u)/255.f;
            clears[0].clearValue.color.float32[1]=((c->color>>8)&255u)/255.f;
            clears[0].clearValue.color.float32[2]=(c->color&255u)/255.f;
            clears[0].clearValue.color.float32[3]=1;
            clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            clears[1].clearValue.depthStencil.depth=c->depth/65535.f;
            VkClearRect rect={0};rect.rect=(VkRect2D){{0,0},{W,H}};rect.layerCount=1;
            vkCmdClearAttachments(g.cmds,2,clears,1,&rect);
        }else{
            vkCmdBindPipeline(g.cmds,VK_PIPELINE_BIND_POINT_GRAPHICS,
                              g.pipeline[c->kind==QRX_AC_RECT?0:(c->flags&7u)]);
            uint32_t number=c->kind==QRX_AC_RECT?6u:3u;
            vkCmdDraw(g.cmds,number,1,first,0);first+=number;
        }
    }
    vkCmdEndRenderPass(g.cmds);
    VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent=(VkExtent3D){W,H,1};
    vkCmdCopyImageToBuffer(g.cmds,g.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.readback,1,&copy);
    VKCHK(vkEndCommandBuffer(g.cmds));
    VkSubmitInfo submit={0};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount=1;submit.pCommandBuffers=&g.cmds;
    double start_ms=0.0,elapsed_ms=0.0;
    for(int i=-warmup;i<repetitions;i++){
        if(i==0)start_ms=qrx_bench_now();
        VKCHK(vkQueueSubmit(g.queue,1,&submit,VK_NULL_HANDLE));
        VKCHK(vkQueueWaitIdle(g.queue));
    }
    elapsed_ms=qrx_bench_now()-start_ms;
    void *pixels=NULL;
    VKCHK(vkMapMemory(g.device,g.rm,0,(VkDeviceSize)W*H*4,0,&pixels));
    status=save_ppm(output,pixels);vkUnmapMemory(g.device,g.rm);
    if(status==0&&repetitions>1){
        VkPhysicalDeviceProperties prop={0};
        vkGetPhysicalDeviceProperties(g.physical,&prop);
        printf("{\"api\":\"vulkan\",\"kind\":\"gpu_submission_and_wait\",\"gpu\":\"%s\","
               "\"frames\":%d,\"warmup\":%d,\"width\":800,\"height\":450,"
               "\"triangles_per_frame\":%u,\"elapsed_ms\":%.3f,\"fps\":%.3f,\"status\":\"PASS\"}\n",
               prop.deviceName,repetitions,warmup,frame->count-1,elapsed_ms,
               repetitions*1000.0/(elapsed_ms>0?elapsed_ms:1.0));
    }
done:
cleanup:
    release_gpu(&g);return status;
}
int main(int argc,char **argv){
    const char *file=argc>1?argv[1]:"qrx-vulkan-3d.ppm";
    if(argc>1&&!strcmp(argv[1],"--bench")){
        const char *out=argc>2?argv[2]:"qrx-vulkan-bench.ppm";
        int frames=argc>3?atoi(argv[3]):60,warmup=argc>4?atoi(argv[4]):10;
        if(frames<2||frames>2000||warmup<0||warmup>500)return 2;
        QRXAccelFrame *scene=qrx_bench_scene();if(!scene)return 2;
        int rc=render(scene,out,frames,warmup);
        qrx_accel_frame_release(scene);
        return rc?2:0;
    }
    QRXAccelStream s={0};if(!qrx_accel_init(&s))return 2;
    QRXGLContext gl;qrx_gl_init(&gl,&s);
    qrx_gl_clear_color(&gl,0.075f,0.14f,0.23f,1);
    qrx_gl_clear(&gl,QRX_GL_COLOR_BUFFER_BIT|QRX_GL_DEPTH_BUFFER_BIT);
    qrx_gl_enable(&gl,QRX_GL_DEPTH_TEST,true);
    qrx_gl_begin(&gl,QRX_GL_TRIANGLES);
    qrx_gl_color(&gl,1,0,0,1);qrx_gl_vertex(&gl,-.7f,-.7f,-.3f);
    qrx_gl_color(&gl,0,1,0,1);qrx_gl_vertex(&gl,.7f,-.7f,-.3f);
    qrx_gl_color(&gl,0,0,1,1);qrx_gl_vertex(&gl,0,.7f,-.3f);
    qrx_gl_end(&gl);
    if(qrx_gl_error(&gl)||!qrx_gl_flush(&gl)){qrx_accel_destroy(&s);return 2;}
    QRXAccelFrame *frame=qrx_accel_take_latest(&s);
    int rc=render(frame,file,1,0);
    qrx_accel_frame_release(frame);qrx_accel_destroy(&s);
    if(!rc)printf("Vulkan3D GPU graphics pipeline: %s (solid triangle, per-vertex color, depth and blending states)\n",file);
    return rc?2:0;
}
