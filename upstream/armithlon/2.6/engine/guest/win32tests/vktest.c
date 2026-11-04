/* Vulkan under QRX86: a coloured triangle from a vertex buffer, 300 frames through a swapchain.
 * vulkan-1.dll is the host wrapper over MoltenVK. */
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include "vkshaders.h"
static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM a, LPARAM b) { return DefWindowProcA(h, m, a, b); }
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
#define F(n) ((PFN_##n)gipa(inst, #n))
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAILED line %d: %d\n", __LINE__, r_); return 1; } } while (0)
int main(void)
{
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { puts("no vulkan-1.dll"); return 2; }
    gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!gipa) { puts("no vkGetInstanceProcAddr"); return 2; }
    const char *iext[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
    CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
    uint32_t n = 1;
    VkPhysicalDevice phys;
    CK(F(vkEnumeratePhysicalDevices)(inst, &n, &phys));
    VkPhysicalDeviceProperties props;
    F(vkGetPhysicalDeviceProperties)(phys, &props);
    printf("GPU: %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueCount = 1; q.pQueuePriorities = &prio;
    const char *dext[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
    VkDevice dev;
    CK(F(vkCreateDevice)(phys, &dci, NULL, &dev));
    VkQueue queue;
    F(vkGetDeviceQueue)(dev, 0, 0, &queue);
    VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkSurfaceKHR surf;
    CK(F(vkCreateWin32SurfaceKHR)(inst, &si, NULL, &surf));
    VkSwapchainCreateInfoKHR sw = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sw.surface = surf; sw.minImageCount = 2; sw.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; sw.imageExtent.width = 640; sw.imageExtent.height = 480;
    sw.imageArrayLayers = 1; sw.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; sw.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sw.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR chain;
    CK(F(vkCreateSwapchainKHR)(dev, &sw, NULL, &chain));
    VkImage imgs[8];
    uint32_t count = 8;
    CK(F(vkGetSwapchainImagesKHR)(dev, chain, &count, imgs));
    VkAttachmentDescription at = { 0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VkRenderPass rp;
    CK(F(vkCreateRenderPass)(dev, &rpi, NULL, &rp));
    VkFramebuffer fb[8];
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = imgs[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_B8G8R8A8_UNORM;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; iv.subresourceRange.levelCount = 1; iv.subresourceRange.layerCount = 1;
        VkImageView view;
        CK(F(vkCreateImageView)(dev, &iv, NULL, &view));
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &view; fi.width = 640; fi.height = 480; fi.layers = 1;
        CK(F(vkCreateFramebuffer)(dev, &fi, NULL, &fb[i]));
    }
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs, fs;
    smi.codeSize = sizeof vert_spv; smi.pCode = vert_spv;
    CK(F(vkCreateShaderModule)(dev, &smi, NULL, &vs));
    smi.codeSize = sizeof frag_spv; smi.pCode = frag_spv;
    CK(F(vkCreateShaderModule)(dev, &smi, NULL, &fs));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    CK(F(vkCreatePipelineLayout)(dev, &pli, NULL, &layout));
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription bd = { 0, 20, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription ad[2] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bd; vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = ad;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, 640, 480, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 640, 480 } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.layout = layout; gp.renderPass = rp;
    VkPipeline pipe;
    CK(F(vkCreateGraphicsPipelines)(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    /* vertex buffer in host-visible memory */
    static const float verts[] = { 0, -0.8f, 1, 0, 0, 0.8f, 0.8f, 0, 1, 0, -0.8f, 0.8f, 0, 0, 1 };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = sizeof verts; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vb;
    CK(F(vkCreateBuffer)(dev, &bi, NULL, &vb));
    VkMemoryRequirements mr;
    F(vkGetBufferMemoryRequirements)(dev, vb, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    F(vkGetPhysicalDeviceMemoryProperties)(phys, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & 6u) == 6u) { type = i; break; }
    }
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = mr.size; mi.memoryTypeIndex = type;
    VkDeviceMemory mem;
    CK(F(vkAllocateMemory)(dev, &mi, NULL, &mem));
    CK(F(vkBindBufferMemory)(dev, vb, mem, 0));
    void *map;
    CK(F(vkMapMemory)(dev, mem, 0, sizeof verts, 0, &map));
    memcpy(map, verts, sizeof verts);
    F(vkUnmapMemory)(dev, mem);
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CK(F(vkCreateCommandPool)(dev, &pi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CK(F(vkAllocateCommandBuffers)(dev, &cai, &cmd));
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore have, done;
    CK(F(vkCreateSemaphore)(dev, &sci, NULL, &have));
    CK(F(vkCreateSemaphore)(dev, &sci, NULL, &done));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence;
    CK(F(vkCreateFence)(dev, &fci, NULL, &fence));
    DWORD t0 = GetTickCount();
    for (int f = 0; f < 300; f++) {
        uint32_t idx;
        CK(F(vkWaitForFences)(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CK(F(vkResetFences)(dev, 1, &fence));
        CK(F(vkAcquireNextImageKHR)(dev, chain, UINT64_MAX, have, VK_NULL_HANDLE, &idx));
        CK(F(vkResetCommandBuffer)(cmd, 0));
        VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CK(F(vkBeginCommandBuffer)(cmd, &bgi));
        VkClearValue clear;
        clear.color.float32[0] = 0.1f; clear.color.float32[1] = 0.1f; clear.color.float32[2] = 0.3f; clear.color.float32[3] = 1;
        VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = rp; rb.framebuffer = fb[idx]; rb.renderArea = sc; rb.clearValueCount = 1; rb.pClearValues = &clear;
        F(vkCmdBeginRenderPass)(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        F(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize off = 0;
        F(vkCmdBindVertexBuffers)(cmd, 0, 1, &vb, &off);
        F(vkCmdDraw)(cmd, 3, 1, 0, 0);
        F(vkCmdEndRenderPass)(cmd);
        CK(F(vkEndCommandBuffer)(cmd));
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo sub2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        sub2.waitSemaphoreCount = 1; sub2.pWaitSemaphores = &have; sub2.pWaitDstStageMask = &wait;
        sub2.commandBufferCount = 1; sub2.pCommandBuffers = &cmd; sub2.signalSemaphoreCount = 1; sub2.pSignalSemaphores = &done;
        CK(F(vkQueueSubmit)(queue, 1, &sub2, fence));
        VkPresentInfoKHR pr = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &done; pr.swapchainCount = 1; pr.pSwapchains = &chain; pr.pImageIndices = &idx;
        CK(F(vkQueuePresentKHR)(queue, &pr));
    }
    printf("Vulkan: 300 frames in %lu ms\n", (unsigned long)(GetTickCount() - t0));
    (void)wp;
    return 0;
}
