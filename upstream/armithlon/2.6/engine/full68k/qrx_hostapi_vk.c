/* vulkan-1 for x86 programs, on MoltenVK when it is on the Mac. The structures differ between
 * 32-bit Windows and the 64-bit host, so each call converts what it passes. A subset: what a
 * program needs to draw with shaders and vertex buffers, without descriptor sets or depth
 * images yet. The swapchain is offscreen; a present reads the frame back and hands it to the
 * Metal renderer, so it works in the window and headless alike.
 * Without MoltenVK every entry point fails the documented way, so programs can fall back. */
#include "qrx_hostapi.h"
#include "qrx_hostgpu.h"
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define DF(n) static uint32_t n(x86cpu *c,const uint32_t *a)
#define NOARGS (void)c;(void)a
#define R32(p) x86_rd32(c,(p))
#define W32(p,v) x86_wr32(c,(p),(v))
static inline uint64_t R64(x86cpu *c,uint32_t p){return (uint64_t)x86_rd32(c,p)|((uint64_t)x86_rd32(c,p+4u)<<32);}
static inline void W64(x86cpu *c,uint32_t p,uint64_t v){x86_wr32(c,p,(uint32_t)v);x86_wr32(c,p+4u,(uint32_t)(v>>32));}
#define R64P(p) R64(c,(p))
#define W64P(p,v) W64(c,(p),(v))
#define ARG64(i) ((uint64_t)a[i]|((uint64_t)a[(i)+1]<<32))
static void rd_mem(x86cpu *c,uint32_t p,void *d,size_t n){uint8_t *o=d;for(size_t i=0;i<n;i++)o[i]=x86_rd8(c,p+(uint32_t)i);}
static void wr_mem(x86cpu *c,uint32_t p,const void *s,size_t n){const uint8_t *o=s;for(size_t i=0;i<n;i++)x86_wr8(c,p+(uint32_t)i,o[i]);}
static char *rd_str(x86cpu *c,uint32_t p){
    static char buf[8][256];static unsigned next;
    char *b=buf[next++%8];unsigned i=0;
    for(;p&&i<255;i++){b[i]=(char)x86_rd8(c,p+i);if(!b[i])break;}
    b[i]=0;return b;
}

/* ---- MoltenVK */
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance host_instance;
static VkDevice host_device;
static bool load_tried,load_ok;
static bool load_moltenvk(void){
    if(load_tried)return load_ok;
    load_tried=true;
    static const char *const paths[]={
        "@executable_path/../Frameworks/libMoltenVK.dylib","/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib",
        "/usr/local/opt/molten-vk/lib/libMoltenVK.dylib","libMoltenVK.dylib",NULL};
    for(unsigned i=0;paths[i];i++){
        void *h=dlopen(paths[i],RTLD_NOW|RTLD_LOCAL);
        if(h){gipa=(PFN_vkGetInstanceProcAddr)dlsym(h,"vkGetInstanceProcAddr");if(gipa){load_ok=true;break;}}
    }
    fprintf(stderr,"[VK] MoltenVK %s\n",load_ok?"loaded":"not found");
    return load_ok;
}
#define INST(name) ((PFN_##name)gipa(host_instance,#name))
#define VKF(name) ((PFN_##name)gipa(host_instance,#name))

/* ---- dispatchable handles are pointers on the host, 32-bit values in the guest */
#define HMAX 4096u
#define HBASE 0x51000000u
static void *htab[HMAX];
static uint32_t hnext=1;
static uint32_t h_put(void *p){
    for(uint32_t i=1;i<hnext;i++)if(htab[i]==p)return HBASE+i;
    if(hnext>=HMAX)return 0;
    htab[hnext]=p;
    return HBASE+hnext++;
}
static void *h_get(uint32_t g){uint32_t i=g-HBASE;return i<hnext?htab[i]:NULL;}

/* ---- host thunks for vkGetInstanceProcAddr */
uint32_t qrx_hostapi_thunk_for(const QRXHostApi *f);

/* ---- mapped memory: the guest sees a copy, synchronised when it submits work */
typedef struct {VkDeviceMemory mem;void *host;uint32_t guest;VkDeviceSize size;bool used;} Mapped;
#define MAPMAX 256u
static Mapped maps[MAPMAX];
extern uint32_t qrx_hostapi_alloc_request;
static uint32_t alloc_shadow(uint32_t size);
static void sync_maps(x86cpu *c){
    for(unsigned i=0;i<MAPMAX;i++)if(maps[i].used&&maps[i].host)rd_mem(c,maps[i].guest,maps[i].host,(size_t)maps[i].size);
}

/* ---- swapchain: images the renderer shows after reading them back */
typedef struct {VkImage image;VkDeviceMemory mem;} SwapImage;
typedef struct {
    bool used;unsigned n,next,w,h;VkFormat format;SwapImage img[8];
    VkBuffer rb;VkDeviceMemory rbmem;void *rbmap;
    VkCommandPool pool;VkCommandBuffer cmd[8];
    VkQueue queue;
} Swap;
#define SWAPMAX 4u
static Swap swaps[SWAPMAX];
static uint32_t hg_tex;
static VkPhysicalDevice host_phys;
static int mem_type(uint32_t bits,VkMemoryPropertyFlags want){
    VkPhysicalDeviceMemoryProperties p;
    VKF(vkGetPhysicalDeviceMemoryProperties)(host_phys,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&want)==want)return (int)i;
    return -1;
}

/* ---- instance level */
DF(vkEnumerateInstanceVersion){NOARGS;if(!load_moltenvk())return (uint32_t)VK_ERROR_INCOMPATIBLE_DRIVER;W32(a[0],VK_API_VERSION_1_1);return 0;}
DF(vkEnumerateInstanceExtensionProperties){
    /* The surface extensions are ours, on top of what the program can use. */
    if(a[2]&&R32(a[1])>=2){
        VkExtensionProperties e[2];memset(e,0,sizeof e);
        strcpy(e[0].extensionName,VK_KHR_SURFACE_EXTENSION_NAME);e[0].specVersion=25;
        strcpy(e[1].extensionName,"VK_KHR_win32_surface");e[1].specVersion=6;
        wr_mem(c,a[2],e,sizeof e);
    }
    W32(a[1],2);
    return 0;
}
DF(vkEnumerateInstanceLayerProperties){NOARGS;W32(a[0],0);return 0;}
DF(vkCreateInstance){
    if(!load_moltenvk())return (uint32_t)VK_ERROR_INCOMPATIBLE_DRIVER;
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion=VK_API_VERSION_1_1;app.pApplicationName="QRX86 guest";
    VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo=&app;
    VkInstance inst=VK_NULL_HANDLE;
    VkResult r=((PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance"))(&ci,NULL,&inst);
    if(r!=VK_SUCCESS)return (uint32_t)r;
    host_instance=inst;
    W32(a[2],h_put(inst));
    return 0;
}
DF(vkDestroyInstance){NOARGS;return 0;}
DF(vkEnumeratePhysicalDevices){
    uint32_t n=0;
    VKF(vkEnumeratePhysicalDevices)(host_instance,&n,NULL);
    if(n>4)n=4;
    if(a[2]){
        uint32_t cap=R32(a[1]);
        VkPhysicalDevice d[4];
        if(n>cap)n=cap;
        VKF(vkEnumeratePhysicalDevices)(host_instance,&n,d);
        for(uint32_t i=0;i<n;i++)W32(a[2]+i*4u,h_put(d[i]));
    }
    W32(a[1],n);
    return 0;
}
/* The only size_t in here is 4 bytes in the guest, and everything after it moves. */
DF(vkGetPhysicalDeviceProperties){
    VkPhysicalDeviceProperties p;
    VKF(vkGetPhysicalDeviceProperties)(h_get(a[0]),&p);
    size_t cut=offsetof(VkPhysicalDeviceProperties,limits)+offsetof(VkPhysicalDeviceLimits,minMemoryMapAlignment);
    size_t next=offsetof(VkPhysicalDeviceProperties,limits)+offsetof(VkPhysicalDeviceLimits,minTexelBufferOffsetAlignment);
    /* On the host 4 bytes of padding precede it; the guest's next field follows it directly. */
    wr_mem(c,a[1],&p,cut-4u);
    W32(a[1]+(uint32_t)cut-4u,(uint32_t)p.limits.minMemoryMapAlignment);
    wr_mem(c,a[1]+(uint32_t)cut,(const uint8_t *)&p+next,sizeof p-next);
    return 0;
}
DF(vkGetPhysicalDeviceFeatures){VkPhysicalDeviceFeatures p;VKF(vkGetPhysicalDeviceFeatures)(h_get(a[0]),&p);wr_mem(c,a[1],&p,sizeof p);return 0;}
DF(vkGetPhysicalDeviceMemoryProperties){VkPhysicalDeviceMemoryProperties p;VKF(vkGetPhysicalDeviceMemoryProperties)(h_get(a[0]),&p);wr_mem(c,a[1],&p,sizeof p);return 0;}
DF(vkGetPhysicalDeviceQueueFamilyProperties){
    uint32_t n=0;
    VkPhysicalDevice d=h_get(a[0]);
    VKF(vkGetPhysicalDeviceQueueFamilyProperties)(d,&n,NULL);
    if(n>8)n=8;
    if(a[2]){
        VkQueueFamilyProperties q[8];
        uint32_t cap=R32(a[1]);
        if(n>cap)n=cap;
        VKF(vkGetPhysicalDeviceQueueFamilyProperties)(d,&n,q);
        wr_mem(c,a[2],q,sizeof(q[0])*n);
    }
    W32(a[1],n);
    return 0;
}
DF(vkEnumerateDeviceExtensionProperties){
    if(a[3]&&R32(a[2])>=1){
        VkExtensionProperties e;memset(&e,0,sizeof e);
        strcpy(e.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME);e.specVersion=70;
        wr_mem(c,a[3],&e,sizeof e);
    }
    W32(a[2],1);
    return 0;
}
DF(vkCreateDevice){
    uint32_t ci=a[1];
    uint32_t qn=R32(ci+12),qp=R32(ci+16);
    VkDeviceQueueCreateInfo q[8];
    static float prio[8][16];
    if(qn>8)qn=8;
    for(uint32_t i=0;i<qn;i++){
        uint32_t g=qp+i*24u;
        memset(&q[i],0,sizeof q[i]);
        q[i].sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q[i].queueFamilyIndex=R32(g+12);q[i].queueCount=R32(g+16);
        if(q[i].queueCount>16)q[i].queueCount=16;
        rd_mem(c,R32(g+20),prio[i],q[i].queueCount*4u);
        q[i].pQueuePriorities=prio[i];
    }
    VkPhysicalDeviceFeatures feat;memset(&feat,0,sizeof feat);
    if(R32(ci+36))rd_mem(c,R32(ci+36),&feat,sizeof feat);
    /* MoltenVK's device is a "portability subset" one and requires saying so. */
    static const char *const ext[]={"VK_KHR_portability_subset"};
    VkDeviceCreateInfo d={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    d.queueCreateInfoCount=qn;d.pQueueCreateInfos=q;d.pEnabledFeatures=&feat;
    d.enabledExtensionCount=1;d.ppEnabledExtensionNames=ext;
    host_phys=h_get(a[0]);
    VkDevice dev=VK_NULL_HANDLE;
    VkResult r=VKF(vkCreateDevice)(host_phys,&d,NULL,&dev);
    if(r!=VK_SUCCESS)return (uint32_t)r;
    host_device=dev;
    W32(a[3],h_put(dev));
    return 0;
}
#define DEVF(name) ((PFN_##name)VKF(vkGetDeviceProcAddr)(host_device,#name))
DF(vkDestroyDevice){NOARGS;return 0;}
DF(vkDeviceWaitIdle){NOARGS;return (uint32_t)DEVF(vkDeviceWaitIdle)(h_get(a[0]));}
DF(vkGetDeviceQueue){
    VkQueue q=VK_NULL_HANDLE;
    DEVF(vkGetDeviceQueue)(h_get(a[0]),a[1],a[2],&q);
    W32(a[3],h_put(q));
    return 0;
}
/* ---- surface: nothing behind it */
static uint64_t surface_next=0x5300000000ULL;
DF(vkCreateWin32SurfaceKHR){W64P(a[3],++surface_next);return 0;}
DF(vkDestroySurfaceKHR){NOARGS;return 0;}
DF(vkGetPhysicalDeviceSurfaceSupportKHR){W32(a[4],1);return 0;}
DF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR){
    VkSurfaceCapabilitiesKHR s;memset(&s,0,sizeof s);
    s.minImageCount=2;s.maxImageCount=8;
    s.currentExtent.width=640;s.currentExtent.height=480;
    s.minImageExtent=s.currentExtent;s.maxImageExtent.width=4096;s.maxImageExtent.height=4096;
    s.maxImageArrayLayers=1;
    s.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;s.currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    s.supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    s.supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    wr_mem(c,a[3],&s,sizeof s);
    return 0;
}
DF(vkGetPhysicalDeviceSurfaceFormatsKHR){
    if(a[4]&&R32(a[3])>=1){
        VkSurfaceFormatKHR f={VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        wr_mem(c,a[4],&f,sizeof f);
    }
    W32(a[3],1);
    return 0;
}
DF(vkGetPhysicalDeviceSurfacePresentModesKHR){
    if(a[4]&&R32(a[3])>=1)W32(a[4],VK_PRESENT_MODE_FIFO_KHR);
    W32(a[3],1);
    return 0;
}
DF(vkCreateSwapchainKHR){
    uint32_t ci=a[1];
    uint32_t n=R32(ci+24),w=R32(ci+36),h=R32(ci+40);
    if(n<2)n=2;
    if(n>8)n=8;
    unsigned idx=0;
    while(idx<SWAPMAX&&swaps[idx].used)idx++;
    if(idx==SWAPMAX)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    Swap *s=&swaps[idx];
    memset(s,0,sizeof *s);
    s->used=true;s->n=n;s->w=w;s->h=h;s->format=(VkFormat)R32(ci+28);
    VkDevice d=host_device;
    for(unsigned i=0;i<n;i++){
        VkImageCreateInfo ic={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType=VK_IMAGE_TYPE_2D;ic.format=s->format;ic.extent.width=w;ic.extent.height=h;ic.extent.depth=1;
        ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkResult r=DEVF(vkCreateImage)(d,&ic,NULL,&s->img[i].image);
        if(r)return (uint32_t)r;
        VkMemoryRequirements mr;
        DEVF(vkGetImageMemoryRequirements)(d,s->img[i].image,&mr);
        VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=mr.size;ai.memoryTypeIndex=(uint32_t)mem_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        r=DEVF(vkAllocateMemory)(d,&ai,NULL,&s->img[i].mem);
        if(r)return (uint32_t)r;
        DEVF(vkBindImageMemory)(d,s->img[i].image,s->img[i].mem,0);
    }
    /* Read-back buffer for the frame. */
    VkBufferCreateInfo bc={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size=(VkDeviceSize)w*h*4u;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    DEVF(vkCreateBuffer)(d,&bc,NULL,&s->rb);
    VkMemoryRequirements mr;
    DEVF(vkGetBufferMemoryRequirements)(d,s->rb,&mr);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=mr.size;ai.memoryTypeIndex=(uint32_t)mem_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    DEVF(vkAllocateMemory)(d,&ai,NULL,&s->rbmem);
    DEVF(vkBindBufferMemory)(d,s->rb,s->rbmem,0);
    DEVF(vkMapMemory)(d,s->rbmem,0,VK_WHOLE_SIZE,0,&s->rbmap);
    VkCommandPoolCreateInfo pc={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=0;
    DEVF(vkCreateCommandPool)(d,&pc,NULL,&s->pool);
    VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool=s->pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=n;
    DEVF(vkAllocateCommandBuffers)(d,&ca,s->cmd);
    qrx_hg_open(w,h);
    W64P(a[3],(uint64_t)idx+1u);
    return 0;
}
DF(vkDestroySwapchainKHR){NOARGS;return 0;}
DF(vkGetSwapchainImagesKHR){
    /* device, swapchain(2), pCount, pImages */
    Swap *s=&swaps[ARG64(1)-1u];
    uint32_t n=s->n;
    if(a[4]){
        uint32_t cap=R32(a[3]);
        if(n>cap)n=cap;
        for(uint32_t i=0;i<n;i++)W64P(a[4]+i*8u,(uint64_t)s->img[i].image);
    }
    W32(a[3],n);
    return 0;
}
DF(vkAcquireNextImageKHR){
    /* device, swapchain(2), timeout(2), semaphore(2), fence(2), pIndex */
    Swap *s=&swaps[ARG64(1)-1u];
    uint32_t i=s->next;
    s->next=(s->next+1u)%s->n;
    W32(a[9],i);
    /* Signal what the program waits on, with an empty submit. */
    VkSemaphore sem=(VkSemaphore)ARG64(5);
    VkFence fence=(VkFence)ARG64(7);
    if(!s->queue)DEVF(vkGetDeviceQueue)(host_device,0,0,&s->queue);
    VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(sem){si.signalSemaphoreCount=1;si.pSignalSemaphores=&sem;}
    if(sem||fence)DEVF(vkQueueSubmit)(s->queue,sem?1:0,sem?&si:NULL,fence);
    return 0;
}
DF(vkQueuePresentKHR){
    uint32_t pi=a[1];
    uint32_t sn=R32(pi+16),sp=R32(pi+20),ip=R32(pi+24);
    VkQueue q=h_get(a[0]);
    DEVF(vkQueueWaitIdle)(q);
    if(sn<1)return 0;
    Swap *s=&swaps[R64P(sp)-1u];
    uint32_t idx=R32(ip);
    VkCommandBuffer cb=s->cmd[idx];
    VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    DEVF(vkResetCommandBuffer)(cb,0);
    DEVF(vkBeginCommandBuffer)(cb,&bi);
    VkImageMemoryBarrier b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=s->img[idx].image;b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;b.subresourceRange.levelCount=1;b.subresourceRange.layerCount=1;
    DEVF(vkCmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkBufferImageCopy rg;memset(&rg,0,sizeof rg);
    rg.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;rg.imageSubresource.layerCount=1;
    rg.imageExtent.width=s->w;rg.imageExtent.height=s->h;rg.imageExtent.depth=1;
    DEVF(vkCmdCopyImageToBuffer)(cb,s->img[idx].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s->rb,1,&rg);
    DEVF(vkEndCommandBuffer)(cb);
    VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount=1;si.pCommandBuffers=&cb;
    DEVF(vkQueueSubmit)(q,1,&si,VK_NULL_HANDLE);
    DEVF(vkQueueWaitIdle)(q);
    /* Show it: BGRA to the renderer's RGBA, and a quad over the whole target. */
    static uint8_t *px;static size_t pxn;
    size_t bytes=(size_t)s->w*s->h*4u;
    if(bytes>pxn){free(px);px=malloc(bytes);pxn=bytes;}
    const uint8_t *src=s->rbmap;
    bool bgra=s->format==VK_FORMAT_B8G8R8A8_UNORM||s->format==VK_FORMAT_B8G8R8A8_SRGB;
    for(size_t i=0;i<bytes;i+=4){px[i]=src[i+(bgra?2:0)];px[i+1]=src[i+1];px[i+2]=src[i+(bgra?0:2)];px[i+3]=255;}
    hg_tex=qrx_hg_texture(hg_tex,s->w,s->h,px,false,false,false);
    QRXHGState st;qrx_hg_state_default(&st);
    st.texture=hg_tex;st.depth_test=false;st.depth_write=false;st.cull=HG_CULL_NONE;st.env=HG_ENV_REPLACE;
    QRXHGVertex v[6];
    static const float pos[6][4]={{-1,1,0,0},{1,1,1,0},{1,-1,1,1},{-1,1,0,0},{1,-1,1,1},{-1,-1,0,1}};
    for(int i=0;i<6;i++){v[i].x=pos[i][0];v[i].y=pos[i][1];v[i].z=0;v[i].w=1;v[i].u=pos[i][2];v[i].v=pos[i][3];v[i].r=v[i].g=v[i].b=v[i].a=1;}
    qrx_hg_triangles(&st,v,6);
    qrx_hg_present();
    return 0;
}
/* ---- resources */
static uint32_t ret_handle(x86cpu *c,uint32_t out,uint64_t h,VkResult r){if(r==VK_SUCCESS)W64P(out,h);return (uint32_t)r;}
DF(vkCreateBuffer){
    uint32_t ci=a[1];
    VkBufferCreateInfo b={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.flags=R32(ci+8);b.size=R64P(ci+16);b.usage=R32(ci+24);b.sharingMode=(VkSharingMode)R32(ci+28);
    VkBuffer o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateBuffer)(h_get(a[0]),&b,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkDestroyNoop){NOARGS;return 0;}
DF(vkGetBufferMemoryRequirements){
    VkMemoryRequirements m;
    DEVF(vkGetBufferMemoryRequirements)(h_get(a[0]),(VkBuffer)ARG64(1),&m);
    wr_mem(c,a[3],&m,sizeof m);
    return 0;
}
DF(vkAllocateMemory){
    uint32_t ci=a[1];
    VkMemoryAllocateInfo m={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    m.allocationSize=R64P(ci+8);m.memoryTypeIndex=R32(ci+16);
    VkDeviceMemory o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkAllocateMemory)(h_get(a[0]),&m,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkFreeMemory){NOARGS;return 0;}
DF(vkBindBufferMemory){return (uint32_t)DEVF(vkBindBufferMemory)(h_get(a[0]),(VkBuffer)ARG64(1),(VkDeviceMemory)ARG64(3),ARG64(5));}
/* device, memory(2), offset(2), size(2), flags, ppData */
DF(vkMapMemory){
    VkDeviceMemory m=(VkDeviceMemory)ARG64(1);
    VkDeviceSize off=ARG64(3),size=ARG64(5);
    if(size==VK_WHOLE_SIZE||size>(1u<<26))return (uint32_t)VK_ERROR_MEMORY_MAP_FAILED;
    /* The door re-runs this call once the guest has allocated the copy. */
    extern uint32_t qrx_hostapi_host_alloc(uint32_t size);
    uint32_t g=qrx_hostapi_host_alloc((uint32_t)size);
    if(!g)return 0;
    void *hostp=NULL;
    VkResult r=DEVF(vkMapMemory)(h_get(a[0]),m,off,size,0,&hostp);
    if(r)return (uint32_t)r;
    wr_mem(c,g,hostp,(size_t)size);
    for(unsigned i=0;i<MAPMAX;i++)if(!maps[i].used){maps[i]=(Mapped){m,hostp,g,size,true};break;}
    W32(a[8],g);
    return 0;
}
DF(vkUnmapMemory){
    VkDeviceMemory m=(VkDeviceMemory)ARG64(1);
    for(unsigned i=0;i<MAPMAX;i++)if(maps[i].used&&maps[i].mem==m){
        rd_mem(c,maps[i].guest,maps[i].host,(size_t)maps[i].size);
        maps[i].used=false;
    }
    DEVF(vkUnmapMemory)(h_get(a[0]),m);
    return 0;
}
DF(vkFlushMappedMemoryRanges){NOARGS;sync_maps(c);return 0;}
DF(vkCreateImageView){
    uint32_t ci=a[1];
    VkImageViewCreateInfo v={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.flags=R32(ci+8);v.image=(VkImage)R64P(ci+16);v.viewType=(VkImageViewType)R32(ci+24);v.format=(VkFormat)R32(ci+28);
    rd_mem(c,ci+32,&v.components,16);rd_mem(c,ci+48,&v.subresourceRange,20);
    VkImageView o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateImageView)(h_get(a[0]),&v,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkCreateRenderPass){
    uint32_t ci=a[1];
    VkAttachmentDescription at[8];VkSubpassDescription sub[4];VkAttachmentReference refs[4][8],dref[4];VkSubpassDependency dep[8];
    uint32_t an=R32(ci+12),sn=R32(ci+20),dn=R32(ci+28);
    if(an>8||sn>4||dn>8)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    rd_mem(c,R32(ci+16),at,sizeof(at[0])*an);
    rd_mem(c,R32(ci+32),dep,sizeof(dep[0])*dn);
    for(uint32_t i=0;i<sn;i++){
        uint32_t g=R32(ci+24)+i*40u;
        memset(&sub[i],0,sizeof sub[i]);
        sub[i].pipelineBindPoint=(VkPipelineBindPoint)R32(g+4);
        sub[i].colorAttachmentCount=R32(g+16);
        if(sub[i].colorAttachmentCount>8)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
        rd_mem(c,R32(g+20),refs[i],8u*sub[i].colorAttachmentCount);
        sub[i].pColorAttachments=refs[i];
        if(R32(g+28)){rd_mem(c,R32(g+28),&dref[i],8);sub[i].pDepthStencilAttachment=&dref[i];}
    }
    VkRenderPassCreateInfo rp={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount=an;rp.pAttachments=at;rp.subpassCount=sn;rp.pSubpasses=sub;rp.dependencyCount=dn;rp.pDependencies=dep;
    VkRenderPass o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateRenderPass)(h_get(a[0]),&rp,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkCreateFramebuffer){
    uint32_t ci=a[1];
    VkImageView views[8];
    uint32_t n=R32(ci+24);
    if(n>8)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    for(uint32_t i=0;i<n;i++)views[i]=(VkImageView)R64P(R32(ci+28)+i*8u);
    VkFramebufferCreateInfo f={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    f.renderPass=(VkRenderPass)R64P(ci+16);f.attachmentCount=n;f.pAttachments=views;
    f.width=R32(ci+32);f.height=R32(ci+36);f.layers=R32(ci+40);
    VkFramebuffer o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateFramebuffer)(h_get(a[0]),&f,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkCreateShaderModule){
    uint32_t ci=a[1],size=R32(ci+12);
    uint32_t *code=malloc(size);
    if(!code)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    rd_mem(c,R32(ci+16),code,size);
    VkShaderModuleCreateInfo s={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    s.codeSize=size;s.pCode=code;
    VkShaderModule o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateShaderModule)(h_get(a[0]),&s,NULL,&o);
    free(code);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkCreatePipelineLayout){
    uint32_t ci=a[1];
    VkPushConstantRange pc[4];
    uint32_t pn=R32(ci+20);
    if(pn>4||R32(ci+12))return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;  /* no descriptor sets yet */
    if(pn)rd_mem(c,R32(ci+24),pc,12u*pn);
    VkPipelineLayoutCreateInfo l={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    l.pushConstantRangeCount=pn;l.pPushConstantRanges=pc;
    VkPipelineLayout o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreatePipelineLayout)(h_get(a[0]),&l,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
/* device, cache(2), count, infos, alloc, pipelines */
DF(vkCreateGraphicsPipelines){
    uint32_t count=a[3];
    for(uint32_t pi=0;pi<count;pi++){
        uint32_t g=a[4]+pi*96u;
        uint32_t sn=R32(g+12);
        if(sn>4)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
        VkPipelineShaderStageCreateInfo st[4];char names[4][64];
        for(uint32_t i=0;i<sn;i++){
            uint32_t s=R32(g+16)+i*32u;
            memset(&st[i],0,sizeof st[i]);
            st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[i].stage=(VkShaderStageFlagBits)R32(s+12);
            st[i].module=(VkShaderModule)R64P(s+16);
            strncpy(names[i],rd_str(c,R32(s+24)),63);names[i][63]=0;
            st[i].pName=names[i];
        }
        VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkVertexInputBindingDescription vb[8];VkVertexInputAttributeDescription va[16];
        if(R32(g+20)){
            uint32_t p=R32(g+20);
            vi.vertexBindingDescriptionCount=R32(p+12);vi.vertexAttributeDescriptionCount=R32(p+20);
            if(vi.vertexBindingDescriptionCount>8||vi.vertexAttributeDescriptionCount>16)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
            rd_mem(c,R32(p+16),vb,12u*vi.vertexBindingDescriptionCount);
            rd_mem(c,R32(p+24),va,16u*vi.vertexAttributeDescriptionCount);
            vi.pVertexBindingDescriptions=vb;vi.pVertexAttributeDescriptions=va;
        }
        VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology=(VkPrimitiveTopology)R32(R32(g+24)+12);ia.primitiveRestartEnable=R32(R32(g+24)+16);
        VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        VkViewport vps[1];VkRect2D scs[1];
        {
            uint32_t p=R32(g+32);
            vp.viewportCount=R32(p+12);vp.scissorCount=R32(p+20);
            if(vp.viewportCount>1||vp.scissorCount>1)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
            if(R32(p+16)){rd_mem(c,R32(p+16),vps,24);vp.pViewports=vps;}
            if(R32(p+24)){rd_mem(c,R32(p+24),scs,16);vp.pScissors=scs;}
        }
        VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        {
            uint32_t p=R32(g+36);
            rs.depthClampEnable=R32(p+12);rs.rasterizerDiscardEnable=R32(p+16);rs.polygonMode=(VkPolygonMode)R32(p+20);
            rs.cullMode=R32(p+24);rs.frontFace=(VkFrontFace)R32(p+28);rs.depthBiasEnable=R32(p+32);
            rs.lineWidth=api_float(R32(p+48));
        }
        VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples=R32(g+40)?(VkSampleCountFlagBits)R32(R32(g+40)+12):VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba[8];
        if(R32(g+48)){
            uint32_t p=R32(g+48);
            cb.logicOpEnable=R32(p+12);cb.logicOp=(VkLogicOp)R32(p+16);cb.attachmentCount=R32(p+20);
            if(cb.attachmentCount>8)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
            rd_mem(c,R32(p+24),cba,32u*cb.attachmentCount);
            cb.pAttachments=cba;
            rd_mem(c,p+28,cb.blendConstants,16);
        }
        VkPipelineDynamicStateCreateInfo dy={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        VkDynamicState ds[16];
        if(R32(g+52)){
            uint32_t p=R32(g+52);
            dy.dynamicStateCount=R32(p+12);
            if(dy.dynamicStateCount>16)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
            rd_mem(c,R32(p+16),ds,4u*dy.dynamicStateCount);
            dy.pDynamicStates=ds;
        }
        VkGraphicsPipelineCreateInfo gp={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount=sn;gp.pStages=st;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;
        gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pColorBlendState=R32(g+48)?&cb:NULL;gp.pDynamicState=R32(g+52)?&dy:NULL;
        gp.layout=(VkPipelineLayout)R64P(g+56);gp.renderPass=(VkRenderPass)R64P(g+64);gp.subpass=R32(g+72);
        VkPipeline o=VK_NULL_HANDLE;
        VkResult r=DEVF(vkCreateGraphicsPipelines)(h_get(a[0]),VK_NULL_HANDLE,1,&gp,NULL,&o);
        if(r)return (uint32_t)r;
        W64P(a[6]+pi*8u,(uint64_t)o);
    }
    return 0;
}
DF(vkCreateCommandPool){
    uint32_t ci=a[1];
    VkCommandPoolCreateInfo p={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    p.flags=R32(ci+8);p.queueFamilyIndex=R32(ci+12);
    VkCommandPool o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateCommandPool)(h_get(a[0]),&p,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkAllocateCommandBuffers){
    uint32_t ci=a[1],n=R32(ci+20);
    if(n>64)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    VkCommandBuffer cb[64];
    VkCommandBufferAllocateInfo i={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    i.commandPool=(VkCommandPool)R64P(ci+8);i.level=(VkCommandBufferLevel)R32(ci+16);i.commandBufferCount=n;
    VkResult r=DEVF(vkAllocateCommandBuffers)(h_get(a[0]),&i,cb);
    if(r)return (uint32_t)r;
    for(uint32_t k=0;k<n;k++)W32(a[2]+k*4u,h_put(cb[k]));
    return 0;
}
DF(vkBeginCommandBuffer){
    VkCommandBufferBeginInfo b={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags=R32(a[1]+8);
    return (uint32_t)DEVF(vkBeginCommandBuffer)(h_get(a[0]),&b);
}
DF(vkEndCommandBuffer){NOARGS;return (uint32_t)DEVF(vkEndCommandBuffer)(h_get(a[0]));}
DF(vkResetCommandBuffer){NOARGS;return (uint32_t)DEVF(vkResetCommandBuffer)(h_get(a[0]),a[1]);}
DF(vkCmdBeginRenderPass){
    uint32_t g=a[1];
    VkClearValue cv[8];
    VkRenderPassBeginInfo b={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    b.renderPass=(VkRenderPass)R64P(g+8);b.framebuffer=(VkFramebuffer)R64P(g+16);
    rd_mem(c,g+24,&b.renderArea,16);
    b.clearValueCount=R32(g+40);
    if(b.clearValueCount>8)b.clearValueCount=8;
    rd_mem(c,R32(g+44),cv,16u*b.clearValueCount);
    b.pClearValues=cv;
    DEVF(vkCmdBeginRenderPass)(h_get(a[0]),&b,(VkSubpassContents)a[2]);
    return 0;
}
DF(vkCmdEndRenderPass){NOARGS;DEVF(vkCmdEndRenderPass)(h_get(a[0]));return 0;}
DF(vkCmdBindPipeline){NOARGS;DEVF(vkCmdBindPipeline)(h_get(a[0]),(VkPipelineBindPoint)a[1],(VkPipeline)ARG64(2));return 0;}
DF(vkCmdDraw){NOARGS;DEVF(vkCmdDraw)(h_get(a[0]),a[1],a[2],a[3],a[4]);return 0;}
DF(vkCmdDrawIndexed){NOARGS;DEVF(vkCmdDrawIndexed)(h_get(a[0]),a[1],a[2],a[3],(int32_t)a[4],a[5]);return 0;}
DF(vkCmdSetViewport){
    VkViewport v[8];uint32_t n=a[2]>8?8:a[2];
    rd_mem(c,a[3],v,24u*n);
    DEVF(vkCmdSetViewport)(h_get(a[0]),a[1],n,v);
    return 0;
}
DF(vkCmdSetScissor){
    VkRect2D v[8];uint32_t n=a[2]>8?8:a[2];
    rd_mem(c,a[3],v,16u*n);
    DEVF(vkCmdSetScissor)(h_get(a[0]),a[1],n,v);
    return 0;
}
DF(vkCmdBindVertexBuffers){
    VkBuffer b[8];VkDeviceSize o[8];uint32_t n=a[2]>8?8:a[2];
    for(uint32_t i=0;i<n;i++){b[i]=(VkBuffer)R64P(a[3]+i*8u);o[i]=R64P(a[4]+i*8u);}
    DEVF(vkCmdBindVertexBuffers)(h_get(a[0]),a[1],n,b,o);
    return 0;
}
DF(vkCmdBindIndexBuffer){NOARGS;DEVF(vkCmdBindIndexBuffer)(h_get(a[0]),(VkBuffer)ARG64(1),ARG64(3),(VkIndexType)a[5]);return 0;}
DF(vkCmdPushConstants){
    /* cb, layout(2), stages, offset, size, values */
    uint8_t d[256];uint32_t n=a[5]>256?256:a[5];
    rd_mem(c,a[6],d,n);
    DEVF(vkCmdPushConstants)(h_get(a[0]),(VkPipelineLayout)ARG64(1),a[3],a[4],n,d);
    return 0;
}
DF(vkCreateSemaphore){
    VkSemaphoreCreateInfo s={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateSemaphore)(h_get(a[0]),&s,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
DF(vkCreateFence){
    VkFenceCreateInfo s={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    s.flags=R32(a[1]+8);
    VkFence o=VK_NULL_HANDLE;
    VkResult r=DEVF(vkCreateFence)(h_get(a[0]),&s,NULL,&o);
    return ret_handle(c,a[3],(uint64_t)o,r);
}
/* device, count, fences, waitAll, timeout(2) */
DF(vkWaitForFences){
    VkFence f[16];uint32_t n=a[1]>16?16:a[1];
    for(uint32_t i=0;i<n;i++)f[i]=(VkFence)R64P(a[2]+i*8u);
    return (uint32_t)DEVF(vkWaitForFences)(h_get(a[0]),n,f,a[3],ARG64(4));
}
DF(vkResetFences){
    VkFence f[16];uint32_t n=a[1]>16?16:a[1];
    for(uint32_t i=0;i<n;i++)f[i]=(VkFence)R64P(a[2]+i*8u);
    return (uint32_t)DEVF(vkResetFences)(h_get(a[0]),n,f);
}
/* queue, count, infos, fence(2) */
DF(vkQueueSubmit){
    uint32_t n=a[1];
    if(n>4)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    sync_maps(c);
    VkSubmitInfo si[4];
    VkSemaphore wait[4][8],sig[4][8];VkPipelineStageFlags st[4][8];VkCommandBuffer cb[4][16];
    for(uint32_t i=0;i<n;i++){
        uint32_t g=a[2]+i*36u;
        memset(&si[i],0,sizeof si[i]);
        si[i].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        uint32_t wn=R32(g+8),cn=R32(g+20),sn=R32(g+28);
        if(wn>8||cn>16||sn>8)return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
        for(uint32_t k=0;k<wn;k++){wait[i][k]=(VkSemaphore)R64P(R32(g+12)+k*8u);st[i][k]=R32(R32(g+16)+k*4u);}
        for(uint32_t k=0;k<cn;k++)cb[i][k]=h_get(R32(R32(g+24)+k*4u));
        for(uint32_t k=0;k<sn;k++)sig[i][k]=(VkSemaphore)R64P(R32(g+32)+k*8u);
        si[i].waitSemaphoreCount=wn;si[i].pWaitSemaphores=wait[i];si[i].pWaitDstStageMask=st[i];
        si[i].commandBufferCount=cn;si[i].pCommandBuffers=cb[i];
        si[i].signalSemaphoreCount=sn;si[i].pSignalSemaphores=sig[i];
    }
    return (uint32_t)DEVF(vkQueueSubmit)(h_get(a[0]),n,si,(VkFence)ARG64(3));
}
DF(vkQueueWaitIdle){NOARGS;return (uint32_t)DEVF(vkQueueWaitIdle)(h_get(a[0]));}

/* ---- table: dword counts include the two halves of each 64-bit argument */
#define E(n,args) {#n,args,n}
#define NOP(n,args) {#n,args,vkDestroyNoop}
const QRXHostApi qrx_api_vk[]={
    E(vkEnumerateInstanceVersion,1),E(vkEnumerateInstanceExtensionProperties,3),E(vkEnumerateInstanceLayerProperties,2),
    E(vkCreateInstance,3),E(vkDestroyInstance,2),E(vkEnumeratePhysicalDevices,3),E(vkGetPhysicalDeviceProperties,2),
    E(vkGetPhysicalDeviceFeatures,2),E(vkGetPhysicalDeviceMemoryProperties,2),E(vkGetPhysicalDeviceQueueFamilyProperties,3),
    E(vkEnumerateDeviceExtensionProperties,4),E(vkCreateDevice,4),E(vkDestroyDevice,2),E(vkDeviceWaitIdle,1),E(vkGetDeviceQueue,4),
    E(vkCreateWin32SurfaceKHR,4),{"vkDestroySurfaceKHR",4,vkDestroySurfaceKHR},E(vkGetPhysicalDeviceSurfaceSupportKHR,5),
    E(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,4),E(vkGetPhysicalDeviceSurfaceFormatsKHR,5),E(vkGetPhysicalDeviceSurfacePresentModesKHR,5),
    E(vkCreateSwapchainKHR,4),E(vkDestroySwapchainKHR,4),E(vkGetSwapchainImagesKHR,5),E(vkAcquireNextImageKHR,10),E(vkQueuePresentKHR,2),
    E(vkCreateBuffer,4),NOP(vkDestroyBuffer,4),E(vkGetBufferMemoryRequirements,4),E(vkAllocateMemory,4),E(vkFreeMemory,4),
    E(vkBindBufferMemory,7),E(vkMapMemory,9),E(vkUnmapMemory,3),E(vkFlushMappedMemoryRanges,3),
    E(vkCreateImageView,4),NOP(vkDestroyImageView,4),E(vkCreateRenderPass,4),NOP(vkDestroyRenderPass,4),E(vkCreateFramebuffer,4),NOP(vkDestroyFramebuffer,4),
    E(vkCreateShaderModule,4),NOP(vkDestroyShaderModule,4),E(vkCreatePipelineLayout,4),NOP(vkDestroyPipelineLayout,4),E(vkCreateGraphicsPipelines,7),NOP(vkDestroyPipeline,4),
    E(vkCreateCommandPool,4),NOP(vkDestroyCommandPool,4),E(vkAllocateCommandBuffers,3),NOP(vkFreeCommandBuffers,5),
    E(vkBeginCommandBuffer,2),E(vkEndCommandBuffer,1),E(vkResetCommandBuffer,2),
    E(vkCmdBeginRenderPass,3),E(vkCmdEndRenderPass,1),E(vkCmdBindPipeline,4),E(vkCmdDraw,5),E(vkCmdDrawIndexed,6),E(vkCmdSetViewport,4),E(vkCmdSetScissor,4),
    E(vkCmdBindVertexBuffers,5),E(vkCmdBindIndexBuffer,6),E(vkCmdPushConstants,7),
    E(vkCreateSemaphore,4),NOP(vkDestroySemaphore,4),E(vkCreateFence,4),NOP(vkDestroyFence,4),E(vkWaitForFences,6),E(vkResetFences,3),
    E(vkQueueSubmit,5),E(vkQueueWaitIdle,1),
    {NULL,0,NULL}
};
static uint32_t proc_addr(x86cpu *c,uint32_t name){
    const char *n=rd_str(c,name);
    for(const QRXHostApi *f=qrx_api_vk;f->name;f++)
        if(!strcmp(f->name,n))return qrx_hostapi_thunk_for(f);
    if(!strcmp(n,"vkGetInstanceProcAddr")||!strcmp(n,"vkGetDeviceProcAddr")){
        extern const QRXHostApi qrx_api_vk_procs[];
        for(const QRXHostApi *f=qrx_api_vk_procs;f->name;f++)if(!strcmp(f->name,n))return qrx_hostapi_thunk_for(f);
    }
    return 0;
}
DF(vkGetInstanceProcAddr){return proc_addr(c,a[1]);}
DF(vkGetDeviceProcAddr){return proc_addr(c,a[1]);}
const QRXHostApi qrx_api_vk_procs[]={E(vkGetInstanceProcAddr,2),E(vkGetDeviceProcAddr,2),{NULL,0,NULL}};
