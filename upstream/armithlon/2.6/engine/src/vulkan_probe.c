/* QRXVulkan runtime capability probe, no SDK headers required. Vulkan ABI
 * types/structure prefixes below follow the Vulkan 1.0 C ABI. This binary
 * loads a system Vulkan loader and creates/destroys a graphics device.
 * It DOES NOT yet render guest graphics commands through Vulkan.
 * No guest-supplied native pointers or driver calls are exposed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef int32_t VkResult;
typedef struct{int32_t sType;const void *pNext;const char *pApplicationName;uint32_t applicationVersion;const char *pEngineName;uint32_t engineVersion;uint32_t apiVersion;} VkApplicationInfo;
typedef struct{int32_t sType;const void *pNext;uint32_t flags;const VkApplicationInfo *pApplicationInfo;uint32_t enabledLayerCount;const char*const*ppEnabledLayerNames;uint32_t enabledExtensionCount;const char*const*ppEnabledExtensionNames;} VkInstanceCreateInfo;
typedef struct{char extensionName[256];uint32_t specVersion;} VkExtensionProperties;
typedef struct{uint32_t queueFlags,queueCount,timestampValidBits;uint32_t minImageTransferGranularity[3];} VkQueueFamilyProperties;
typedef struct{int32_t sType;const void *pNext;uint32_t flags;uint32_t queueFamilyIndex;uint32_t queueCount;const float *pQueuePriorities;} VkDeviceQueueCreateInfo;
typedef struct{int32_t sType;const void*pNext;uint32_t flags;uint32_t queueCreateInfoCount;const VkDeviceQueueCreateInfo *pQueueCreateInfos;uint32_t enabledLayerCount;const char*const*ppEnabledLayerNames;uint32_t enabledExtensionCount;const char*const*ppEnabledExtensionNames;const void *pEnabledFeatures;} VkDeviceCreateInfo;
#define VK_SUCCESS 0
#define VK_QUEUE_GRAPHICS_BIT 1u
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 1u
#define VK_API_VERSION_1_0 (1u<<22)

typedef VkResult(*PFN_VkEnumerateInstanceVersion)(uint32_t*);
typedef VkResult(*PFN_VkEnumerateInstanceExtensionProperties)(const char*,uint32_t*,VkExtensionProperties*);
typedef VkResult(*PFN_VkCreateInstance)(const VkInstanceCreateInfo*,const void*,VkInstance*);
typedef void(*PFN_VkDestroyInstance)(VkInstance,const void*);
typedef VkResult(*PFN_VkEnumeratePhysicalDevices)(VkInstance,uint32_t*,VkPhysicalDevice*);
typedef void(*PFN_VkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice,uint32_t*,VkQueueFamilyProperties*);
typedef VkResult(*PFN_VkCreateDevice)(VkPhysicalDevice,const VkDeviceCreateInfo*,const void*,VkDevice*);
typedef void(*PFN_VkDestroyDevice)(VkDevice,const void*);
#define LOAD(func) do{void *sym=dlsym(lib,#func); memcpy(&func,&sym,sizeof(sym));if(!func){fprintf(stderr,"Vulkan loader missing %s\n",#func);goto cleanup;}}while(0)
int main(void){
    void *lib=NULL;VkInstance instance=NULL;VkDevice device=NULL;int exitcode=2;
    PFN_VkEnumerateInstanceVersion vkEnumerateInstanceVersion=NULL;
    PFN_VkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties=NULL;
    PFN_VkCreateInstance vkCreateInstance=NULL;
    PFN_VkDestroyInstance vkDestroyInstance=NULL;
    PFN_VkEnumeratePhysicalDevices vkEnumeratePhysicalDevices=NULL;
    PFN_VkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties=NULL;
    PFN_VkCreateDevice vkCreateDevice=NULL;
    PFN_VkDestroyDevice vkDestroyDevice=NULL;
#ifdef __APPLE__
    const char *libs[]={"libvulkan.1.dylib","libvulkan.dylib",NULL};
#else
    const char *libs[]={"libvulkan.so.1","libvulkan.so",NULL};
#endif
    for(int i=0;libs[i];i++){lib=dlopen(libs[i],RTLD_NOW|RTLD_LOCAL);if(lib)break;}
    if(!lib){fprintf(stderr,"Vulkan loader not found; install Vulkan SDK/loader.\n");return 2;}
    {void *sym=dlsym(lib,"vkEnumerateInstanceVersion");memcpy(&vkEnumerateInstanceVersion,&sym,sizeof(sym));}
    LOAD(vkEnumerateInstanceExtensionProperties);LOAD(vkCreateInstance);LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);LOAD(vkDestroyDevice);
    uint32_t version=VK_API_VERSION_1_0;
    if(vkEnumerateInstanceVersion && vkEnumerateInstanceVersion(&version)!=VK_SUCCESS)goto cleanup;
    printf("Vulkan loader API %u.%u.%u\n",(version>>22)&127u,(version>>12)&1023u,version&4095u);
    uint32_t ext_count=0;int portable=0;
    if(vkEnumerateInstanceExtensionProperties(NULL,&ext_count,NULL)==VK_SUCCESS && ext_count<=1024){
        VkExtensionProperties *exts=calloc(ext_count?ext_count:1,sizeof(*exts));
        if(exts && vkEnumerateInstanceExtensionProperties(NULL,&ext_count,exts)==VK_SUCCESS){
            for(uint32_t i=0;i<ext_count;i++)if(strcmp(exts[i].extensionName,"VK_KHR_portability_enumeration")==0)portable=1;
        }
        free(exts);
    }
    const char *portability_name="VK_KHR_portability_enumeration";
    VkApplicationInfo app={0,NULL,"ARMITHLON Vulkan Host Probe",0,"QRX",0,VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci={1,NULL,portable?VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR:0,
                            &app,0,NULL,portable?1u:0u,portable?&portability_name:NULL};
    VkResult res=vkCreateInstance(&ci,NULL,&instance);
    if(res!=VK_SUCCESS){fprintf(stderr,"vkCreateInstance failed (%d), check Vulkan ICD/drivers\n",res);goto cleanup;}
    uint32_t num=0;
    if(vkEnumeratePhysicalDevices(instance,&num,NULL)!=VK_SUCCESS || num==0 || num>64){fprintf(stderr,"No Vulkan physical device available\n");goto cleanup;}
    VkPhysicalDevice *devices=calloc(num,sizeof(*devices));
    if(!devices)goto cleanup;
    if(vkEnumeratePhysicalDevices(instance,&num,devices)!=VK_SUCCESS){free(devices);goto cleanup;}
    for(uint32_t i=0;i<num;i++){
        uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&nq,NULL);
        if(!nq||nq>128)continue;
        VkQueueFamilyProperties *queues=calloc(nq,sizeof(*queues));if(!queues)continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&nq,queues);
        for(uint32_t k=0;k<nq;k++)if((queues[k].queueFlags&VK_QUEUE_GRAPHICS_BIT)&&queues[k].queueCount){
            float priority=1.0f;
            VkDeviceQueueCreateInfo qi={2,NULL,0,k,1,&priority};
            VkDeviceCreateInfo di={3,NULL,0,1,&qi,0,NULL,0,NULL,NULL};
            res=vkCreateDevice(devices[i],&di,NULL,&device);
            if(res==VK_SUCCESS){printf("Vulkan graphics device READY (device %u, queue family %u, portable=%d)\n",i,k,portable);exitcode=0;break;}
        }
        free(queues);
        if(device)break;
    }
    free(devices);
    if(!device)fprintf(stderr,"No usable Vulkan graphics device: renderer not activated\n");
cleanup:
    if(device && vkDestroyDevice)vkDestroyDevice(device,NULL);
    if(instance && vkDestroyInstance)vkDestroyInstance(instance,NULL);
    if(lib)dlclose(lib);
    if(exitcode==0)puts("NOTE: device probe only. No guest Vulkan rendering or WSI/swapchain yet.");
    return exitcode;
}
