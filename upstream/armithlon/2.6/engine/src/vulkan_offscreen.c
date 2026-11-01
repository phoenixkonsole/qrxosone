/* ARMITHLON 0.8: real Vulkan transfer-only offscreen renderer.
 * Uses VkImage clear and Buffer->Image commands to execute QRXGPU 2D on the
 * Vulkan device (no OpenGL, WSI/swapchain or fake CPU Vulkan backend).
 * QRX_AC_TRIANGLE is explicitly unsupported here; 3D still uses native Metal.
 * This file is optional and compiled ONLY when Vulkan SDK headers are found.
 * Pixel output is read back solely for deterministic validation/screenshots. */
#include <vulkan/vulkan.h>
#include "qrx_accel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define W 800u
#define H 450u
#define UPLOAD_LIMIT (24u*1024u*1024u)
#define CHECK(call) do{VkResult _r=(call);if(_r!=VK_SUCCESS){fprintf(stderr,"Vulkan: %s failed: %d\n",#call,(int)_r);goto cleanup;}}while(0)

typedef struct GPU {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkImage image;
    VkDeviceMemory image_memory;
    VkBuffer staging, readback;
    VkDeviceMemory staging_memory, readback_memory;
    VkCommandPool pool;
    VkCommandBuffer cmds;
} GPU;
static int memtype(VkPhysicalDevice device,uint32_t mask,VkMemoryPropertyFlags flags){
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(device,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((mask&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags)return (int)i;
    return -1;
}
static int make_buffer(GPU *g,VkDeviceSize size,VkBufferUsageFlags usage,VkBuffer *buf,VkDeviceMemory *mem){
    VkBufferCreateInfo bi={0};bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size=size;bi.usage=usage;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(g->device,&bi,NULL,buf)!=VK_SUCCESS)return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->device,*buf,&req);
    int type=memtype(g->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(type<0)return -1;
    VkMemoryAllocateInfo ai={0};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize=req.size;ai.memoryTypeIndex=(uint32_t)type;
    if(vkAllocateMemory(g->device,&ai,NULL,mem)!=VK_SUCCESS)return -1;
    return vkBindBufferMemory(g->device,*buf,*mem,0)==VK_SUCCESS?0:-1;
}
static int write_ppm(const char *path,const uint8_t *rgba){
    FILE *f=fopen(path,"wb");if(!f)return -1;
    if(fprintf(f,"P6\n%u %u\n255\n",W,H)<0){fclose(f);return -1;}
    int bad=0;
    for(uint32_t y=0;y<H;y++)for(uint32_t x=0;x<W;x++){
        const uint8_t *p=rgba+((size_t)y*W+x)*4;
        if(fwrite(p,1,3,f)!=3)bad=1;
    }
    if(fclose(f)!=0)bad=1;
    return bad?-1:0;
}
static void cleanup(GPU *g){
    if(g->device){
        vkDeviceWaitIdle(g->device);
        if(g->pool)vkDestroyCommandPool(g->device,g->pool,NULL);
        if(g->image)vkDestroyImage(g->device,g->image,NULL);
        if(g->staging)vkDestroyBuffer(g->device,g->staging,NULL);
        if(g->readback)vkDestroyBuffer(g->device,g->readback,NULL);
        if(g->image_memory)vkFreeMemory(g->device,g->image_memory,NULL);
        if(g->staging_memory)vkFreeMemory(g->device,g->staging_memory,NULL);
        if(g->readback_memory)vkFreeMemory(g->device,g->readback_memory,NULL);
        vkDestroyDevice(g->device,NULL);
    }
    if(g->instance)vkDestroyInstance(g->instance,NULL);
}
static int render(const QRXAccelFrame *f,const char *path){
    GPU g={0};
    int rc=-1;
    uint8_t *uploads=NULL;
    size_t upload_used=0;
    VkBufferImageCopy copies[QRX_ACCEL_MAX_COMMANDS];
    if(!f || f->count>QRX_ACCEL_MAX_COMMANDS)return -1;
    /* Validate all commands BEFORE submitting anything to the Vulkan driver. */
    for(uint32_t i=0;i<f->count;i++){
        const QRXAccelCmd *c=&f->commands[i];
        if(c->kind==QRX_AC_TRIANGLE){fprintf(stderr,"Vulkan 0.8: TRIANGLE requires graphics pipeline (Metal backend supported).\n");return -2;}
        if(c->kind==QRX_AC_CLEAR)continue;
        if(c->kind!=QRX_AC_RECT && c->kind!=QRX_AC_IMAGE)return -1;
        if(!c->w||!c->h||c->x>=W||c->y>=H||c->w>W-c->x||c->h>H-c->y)return -1;
        size_t sz=(size_t)c->w*c->h*4;
        if(sz>UPLOAD_LIMIT-upload_used)return -1;
        if(c->kind==QRX_AC_IMAGE && (!c->image||c->pitch<c->w*4))return -1;
        copies[i].bufferOffset=upload_used;
        copies[i].bufferRowLength=0;
        copies[i].bufferImageHeight=0;
        copies[i].imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copies[i].imageOffset=(VkOffset3D){(int32_t)c->x,(int32_t)c->y,0};
        copies[i].imageExtent=(VkExtent3D){c->w,c->h,1};
        upload_used+=sz;
    }
    /* Vulkan macOS portability enumeration (MoltenVK/KosmicKrisp). */
    uint32_t num_ext=0;int portability=0;
    if(vkEnumerateInstanceExtensionProperties(NULL,&num_ext,NULL)==VK_SUCCESS && num_ext<=1024){
        VkExtensionProperties *ext=calloc(num_ext?num_ext:1,sizeof(*ext));
        if(ext && vkEnumerateInstanceExtensionProperties(NULL,&num_ext,ext)==VK_SUCCESS){
            for(uint32_t i=0;i<num_ext;i++)if(!strcmp(ext[i].extensionName,"VK_KHR_portability_enumeration"))portability=1;
        }free(ext);
    }
    const char *extension="VK_KHR_portability_enumeration";
    VkApplicationInfo app={0};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="ARMITHLON QRXGPU Vulkan 0.8";app.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo ii={0};ii.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ii.pApplicationInfo=&app;
    if(portability){ii.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;ii.enabledExtensionCount=1;ii.ppEnabledExtensionNames=&extension;}
    CHECK(vkCreateInstance(&ii,NULL,&g.instance));
    uint32_t num=0;
    CHECK(vkEnumeratePhysicalDevices(g.instance,&num,NULL));
    if(!num||num>64){fprintf(stderr,"Vulkan 0.8: no physical devices\n");goto cleanup;}
    VkPhysicalDevice devices[64];CHECK(vkEnumeratePhysicalDevices(g.instance,&num,devices));
    for(uint32_t d=0;d<num && !g.physical;d++){
        uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[d],&nq,NULL);
        if(nq==0||nq>128)continue;
        VkQueueFamilyProperties qs[128];vkGetPhysicalDeviceQueueFamilyProperties(devices[d],&nq,qs);
        for(uint32_t q=0;q<nq;q++)if(qs[q].queueCount && (qs[q].queueFlags&(VK_QUEUE_TRANSFER_BIT|VK_QUEUE_GRAPHICS_BIT))){
            g.physical=devices[d];g.family=q;break;
        }
    }
    if(!g.physical){fprintf(stderr,"Vulkan 0.8: no transfer-capable device\n");goto cleanup;}
    const float priority=1.f;
    VkDeviceQueueCreateInfo qi={0};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex=g.family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di={0};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    uint32_t d_extn=0;
    if(vkEnumerateDeviceExtensionProperties(g.physical,NULL,&d_extn,NULL)==VK_SUCCESS && d_extn<=1024){
        VkExtensionProperties *e=calloc(d_extn?d_extn:1,sizeof(*e));
        if(e && vkEnumerateDeviceExtensionProperties(g.physical,NULL,&d_extn,e)==VK_SUCCESS){
            for(uint32_t i=0;i<d_extn;i++)if(!strcmp(e[i].extensionName,"VK_KHR_portability_subset")){
                static const char *subset="VK_KHR_portability_subset";
                di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&subset;
            }
        }free(e);
    }
    CHECK(vkCreateDevice(g.physical,&di,NULL,&g.device));
    vkGetDeviceQueue(g.device,g.family,0,&g.queue);
    VkImageCreateInfo image={0};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType=VK_IMAGE_TYPE_2D;
    image.format=VK_FORMAT_R8G8B8A8_UNORM;image.extent=(VkExtent3D){W,H,1};
    image.mipLevels=1;image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(g.device,&image,NULL,&g.image));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(g.device,g.image,&req);
    int mt=memtype(g.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(mt<0)mt=memtype(g.physical,req.memoryTypeBits,0);
    if(mt<0)goto cleanup;
    VkMemoryAllocateInfo ma={0};ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize=req.size;ma.memoryTypeIndex=(uint32_t)mt;
    CHECK(vkAllocateMemory(g.device,&ma,NULL,&g.image_memory));
    CHECK(vkBindImageMemory(g.device,g.image,g.image_memory,0));
    if(make_buffer(&g,upload_used?upload_used:4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&g.staging,&g.staging_memory))goto cleanup;
    if(make_buffer(&g,(VkDeviceSize)W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&g.readback,&g.readback_memory))goto cleanup;
    uploads=malloc(upload_used?upload_used:1);if(!uploads)goto cleanup;
    for(uint32_t i=0;i<f->count;i++){
        const QRXAccelCmd *c=&f->commands[i];
        if(c->kind!=QRX_AC_RECT && c->kind!=QRX_AC_IMAGE)continue;
        uint8_t *dest=uploads+copies[i].bufferOffset;
        for(uint32_t y=0;y<c->h;y++)for(uint32_t x=0;x<c->w;x++){
            uint8_t *p=dest+((size_t)y*c->w+x)*4;
            if(c->kind==QRX_AC_RECT){
                p[0]=(uint8_t)(c->color>>16);p[1]=(uint8_t)(c->color>>8);
                p[2]=(uint8_t)c->color;p[3]=255;
            }else memcpy(p,c->image+(size_t)y*c->pitch+(size_t)x*4,4);
        }
    }
    if(upload_used){
        void *map=NULL;CHECK(vkMapMemory(g.device,g.staging_memory,0,upload_used,0,&map));
        memcpy(map,uploads,upload_used);vkUnmapMemory(g.device,g.staging_memory);
    }
    VkCommandPoolCreateInfo pc={0};pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pc.queueFamilyIndex=g.family;CHECK(vkCreateCommandPool(g.device,&pc,NULL,&g.pool));
    VkCommandBufferAllocateInfo ca={0};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool=g.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
    CHECK(vkAllocateCommandBuffers(g.device,&ca,&g.cmds));
    VkCommandBufferBeginInfo cb={0};cb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(g.cmds,&cb));
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier before={0};before.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    before.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    before.image=g.image;before.subresourceRange=range;
    before.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(g.cmds,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&before);
    /* Uninitialized portions must be deterministic. */
    VkClearColorValue clear={.float32={0,0,0,1}};
    vkCmdClearColorImage(g.cmds,g.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    for(uint32_t i=0;i<f->count;i++){
        const QRXAccelCmd *c=&f->commands[i];
        /* Previous transfer writes must finish before overwriting overlapping
         * image regions in stream order. Clear and copies can target same pixel. */
        VkImageMemoryBarrier serial={0};
        serial.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        serial.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        serial.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        serial.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        serial.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        serial.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        serial.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        serial.image=g.image;serial.subresourceRange=range;
        vkCmdPipelineBarrier(g.cmds,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,NULL,0,NULL,1,&serial);
        if(c->kind==QRX_AC_CLEAR){
            clear.float32[0]=((c->color>>16)&255u)/255.f;
            clear.float32[1]=((c->color>>8)&255u)/255.f;
            clear.float32[2]=(c->color&255u)/255.f;
            clear.float32[3]=1.f;
            vkCmdClearColorImage(g.cmds,g.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
        }else{
            vkCmdCopyBufferToImage(g.cmds,g.staging,g.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copies[i]);
        }
    }
    VkImageMemoryBarrier after={0};after.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    after.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;after.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    after.image=g.image;after.subresourceRange=range;
    vkCmdPipelineBarrier(g.cmds,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&after);
    VkBufferImageCopy back={0};back.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    back.imageExtent=(VkExtent3D){W,H,1};
    vkCmdCopyImageToBuffer(g.cmds,g.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.readback,1,&back);
    CHECK(vkEndCommandBuffer(g.cmds));
    VkSubmitInfo submit={0};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount=1;submit.pCommandBuffers=&g.cmds;
    CHECK(vkQueueSubmit(g.queue,1,&submit,VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g.queue));
    void *mapped=NULL;CHECK(vkMapMemory(g.device,g.readback_memory,0,(VkDeviceSize)W*H*4,0,&mapped));
    rc=write_ppm(path,(const uint8_t *)mapped);
    vkUnmapMemory(g.device,g.readback_memory);
cleanup:
    free(uploads);cleanup(&g);return rc;
}
int main(int argc,char **argv){
    const char *dest=argc>1?argv[1]:"qrx-vulkan-0.8.ppm";
    QRXAccelStream stream={0};
    if(!qrx_accel_init(&stream))return 2;
    int fail=0;
    fail|=!qrx_accel_clear(&stream,0x00132349,65535);
    fail|=!qrx_accel_rect(&stream,100,80,180,140,0x00ff8800);
    uint8_t argb[16]={255,255,0,0,255,0,255,0,255,0,0,255,255,255,255,255};
    fail|=!qrx_accel_image(&stream,10,10,2,2,argb,8);
    fail|=!qrx_accel_commit(&stream);
    QRXAccelFrame *f=qrx_accel_take_latest(&stream);
    int rc=fail||!f?2:render(f,dest);
    qrx_accel_frame_release(f);qrx_accel_destroy(&stream);
    if(!rc)printf("Vulkan native GPU transfer pipeline OK: %s (%ux%u). No 3D shaders or swapchain yet.\n",dest,W,H);
    return rc?2:0;
}
