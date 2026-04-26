#include "venus.h"

extern int strcmp(const char *, const char *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!name) return NULL;

    /* Instance-level entry points. */
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)venus_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)venus_DestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)venus_EnumeratePhysicalDevices;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceFeatures;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceMemoryProperties;

    /* Device-scoped entry points — loader resolves via GIPA with instance
     * arg in the Khronos convention, so we must still answer here. */
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)venus_CreateDevice;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)venus_DestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)venus_GetDeviceQueue;

    /* W3b.3 — memory + buffer entry points. */
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)venus_AllocateMemory;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)venus_FreeMemory;
    if (strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)venus_MapMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)venus_UnmapMemory;
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)venus_CreateBuffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)venus_DestroyBuffer;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)venus_GetBufferMemoryRequirements;
    if (strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)venus_BindBufferMemory;

    /* W3b.4 — shader + render pass + image + framebuffer + pipeline + cmd. */
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)venus_CreateShaderModule;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)venus_DestroyShaderModule;
    if (strcmp(name, "vkCreateRenderPass") == 0)
        return (PFN_vkVoidFunction)venus_CreateRenderPass;
    if (strcmp(name, "vkDestroyRenderPass") == 0)
        return (PFN_vkVoidFunction)venus_DestroyRenderPass;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)venus_CreateImage;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)venus_DestroyImage;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)venus_GetImageMemoryRequirements;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)venus_BindImageMemory;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)venus_CreateImageView;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)venus_DestroyImageView;
    if (strcmp(name, "vkCreateFramebuffer") == 0)
        return (PFN_vkVoidFunction)venus_CreateFramebuffer;
    if (strcmp(name, "vkDestroyFramebuffer") == 0)
        return (PFN_vkVoidFunction)venus_DestroyFramebuffer;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)venus_CreatePipelineLayout;
    if (strcmp(name, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)venus_DestroyPipelineLayout;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)venus_CreateGraphicsPipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)venus_DestroyPipeline;
    if (strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)venus_CreateCommandPool;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)venus_DestroyCommandPool;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)venus_AllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)venus_FreeCommandBuffers;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)venus_BeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)venus_EndCommandBuffer;
    if (strcmp(name, "vkCmdBeginRenderPass") == 0)
        return (PFN_vkVoidFunction)venus_CmdBeginRenderPass;
    if (strcmp(name, "vkCmdEndRenderPass") == 0)
        return (PFN_vkVoidFunction)venus_CmdEndRenderPass;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)venus_CmdBindPipeline;
    if (strcmp(name, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)venus_CmdDraw;

    /* W3b.5 — WSI + surface + swapchain + queue + sync + present. */
    if (strcmp(name, "vkCreateOsitokCompositorSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)venus_CreateOsitokCompositorSurfaceKHR;
    if (strcmp(name, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)venus_DestroySurfaceKHR;
    if (strcmp(name, "vkQueueSubmit") == 0)
        return (PFN_vkVoidFunction)venus_QueueSubmit;
    if (strcmp(name, "vkQueueWaitIdle") == 0)
        return (PFN_vkVoidFunction)venus_QueueWaitIdle;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)venus_DeviceWaitIdle;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)venus_CreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)venus_DestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)venus_ResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)venus_WaitForFences;
    if (strcmp(name, "vkGetFenceStatus") == 0)
        return (PFN_vkVoidFunction)venus_GetFenceStatus;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)venus_CreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)venus_DestroySemaphore;
    if (strcmp(name, "vkCreateSwapchainKHR") == 0)
        return (PFN_vkVoidFunction)venus_CreateSwapchainKHR;
    if (strcmp(name, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction)venus_DestroySwapchainKHR;
    if (strcmp(name, "vkGetSwapchainImagesKHR") == 0)
        return (PFN_vkVoidFunction)venus_GetSwapchainImagesKHR;
    if (strcmp(name, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction)venus_AcquireNextImageKHR;
    if (strcmp(name, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)venus_QueuePresentKHR;
    if (strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceSurfaceFormatsKHR;
    if (strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceSurfacePresentModesKHR;
    if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceSurfaceSupportKHR;

    /* W3b.6 — vertex input + dynamic state. */
    if (strcmp(name, "vkCmdBindVertexBuffers") == 0)
        return (PFN_vkVoidFunction)venus_CmdBindVertexBuffers;
    if (strcmp(name, "vkCmdSetViewport") == 0)
        return (PFN_vkVoidFunction)venus_CmdSetViewport;
    if (strcmp(name, "vkCmdSetScissor") == 0)
        return (PFN_vkVoidFunction)venus_CmdSetScissor;

    /* W4.8 — clear-only fast path. */
    if (strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)venus_CmdClearColorImage;

    return NULL;
}
