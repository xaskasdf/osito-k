#include "venus_real.h"

extern int strcmp(const char *, const char *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    (void)instance;
    if (!name)
        return 0;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyInstance;
    if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)venus_real_EnumerateInstanceVersion;
    if (instance && strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)venus_real_EnumeratePhysicalDevices;
    if (instance && strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)venus_real_GetPhysicalDeviceProperties;
    if (instance &&
        (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
         strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_GetPhysicalDeviceProperties2;
    if (instance &&
        strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)
            venus_real_GetPhysicalDeviceQueueFamilyProperties;
    if (instance && strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)venus_real_GetPhysicalDeviceFeatures;
    if (instance &&
        (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
         strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_GetPhysicalDeviceFeatures2;
    if (instance &&
        strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)venus_real_GetPhysicalDeviceMemoryProperties;
    if (instance &&
        (strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
         strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0))
        return (PFN_vkVoidFunction)
            venus_real_GetPhysicalDeviceMemoryProperties2;
    if (instance &&
        (strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
         strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0))
        return (PFN_vkVoidFunction)
            venus_real_GetPhysicalDeviceFormatProperties2;
    if (instance &&
        (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
         strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0))
        return (PFN_vkVoidFunction)
            venus_real_GetPhysicalDeviceImageFormatProperties2;
    if (instance &&
        strcmp(name,
               "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT") == 0)
        return (PFN_vkVoidFunction)
            venus_real_GetPhysicalDeviceCalibrateableTimeDomainsEXT;
    if (instance &&
        strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)
            venus_real_EnumerateDeviceExtensionProperties;
    if (instance && strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateDevice;
    if (instance && strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyDevice;
    if (instance && strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)venus_real_GetDeviceQueue;
    if (instance && strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)venus_real_DeviceWaitIdle;
    if (instance && strcmp(name, "vkQueueWaitIdle") == 0)
        return (PFN_vkVoidFunction)venus_real_QueueWaitIdle;
    if (instance && strcmp(name, "vkGetCalibratedTimestampsEXT") == 0)
        return (PFN_vkVoidFunction)venus_real_GetCalibratedTimestampsEXT;
    if (instance && strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateDescriptorSetLayout;
    if (instance && strcmp(name, "vkDestroyDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyDescriptorSetLayout;
    if (instance &&
        (strcmp(name, "vkCreateDescriptorUpdateTemplate") == 0 ||
         strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CreateDescriptorUpdateTemplate;
    if (instance &&
        (strcmp(name, "vkDestroyDescriptorUpdateTemplate") == 0 ||
         strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_DestroyDescriptorUpdateTemplate;
    if (instance && strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)venus_real_CreatePipelineLayout;
    if (instance && strcmp(name, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyPipelineLayout;
    if (instance && strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateSemaphore;
    if (instance && strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroySemaphore;
    if (instance &&
        (strcmp(name, "vkGetSemaphoreCounterValue") == 0 ||
         strcmp(name, "vkGetSemaphoreCounterValueKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_GetSemaphoreCounterValue;
    if (instance &&
        (strcmp(name, "vkWaitSemaphores") == 0 ||
         strcmp(name, "vkWaitSemaphoresKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_WaitSemaphores;
    if (instance &&
        (strcmp(name, "vkSignalSemaphore") == 0 ||
         strcmp(name, "vkSignalSemaphoreKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_SignalSemaphore;
    if (instance && strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateFence;
    if (instance && strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyFence;
    if (instance && strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)venus_real_ResetFences;
    if (instance && strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)venus_real_WaitForFences;
    if (instance && strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateCommandPool;
    if (instance && strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyCommandPool;
    if (instance && strcmp(name, "vkResetCommandPool") == 0)
        return (PFN_vkVoidFunction)venus_real_ResetCommandPool;
    if (instance && strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)venus_real_AllocateCommandBuffers;
    if (instance && strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)venus_real_FreeCommandBuffers;
    if (instance && strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)venus_real_BeginCommandBuffer;
    if (instance && strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)venus_real_EndCommandBuffer;
    if (instance && strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)venus_real_ResetCommandBuffer;
    if (instance &&
        (strcmp(name, "vkQueueSubmit2") == 0 ||
         strcmp(name, "vkQueueSubmit2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_QueueSubmit2;
    if (instance &&
        (strcmp(name, "vkCmdCopyBuffer2") == 0 ||
         strcmp(name, "vkCmdCopyBuffer2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdCopyBuffer2;
    if (instance &&
        (strcmp(name, "vkCmdPipelineBarrier2") == 0 ||
         strcmp(name, "vkCmdPipelineBarrier2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdPipelineBarrier2;
    if (instance &&
        (strcmp(name, "vkCmdCopyBufferToImage2") == 0 ||
         strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdCopyBufferToImage2;
    if (instance &&
        (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 ||
         strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdCopyImageToBuffer2;
    if (instance && strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdClearColorImage;
    if (instance && strcmp(name, "vkCmdClearDepthStencilImage") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdClearDepthStencilImage;
    if (instance &&
        (strcmp(name, "vkCmdBeginRendering") == 0 ||
         strcmp(name, "vkCmdBeginRenderingKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdBeginRendering;
    if (instance &&
        (strcmp(name, "vkCmdEndRendering") == 0 ||
         strcmp(name, "vkCmdEndRenderingKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdEndRendering;
    if (instance &&
        (strcmp(name, "vkCmdBindVertexBuffers2") == 0 ||
         strcmp(name, "vkCmdBindVertexBuffers2EXT") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdBindVertexBuffers2;
    if (instance && strcmp(name, "vkCmdBindVertexBuffers") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdBindVertexBuffers;
    if (instance && strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdBindPipeline;
    if (instance &&
        strcmp(name, "vkCmdSetAttachmentFeedbackLoopEnableEXT") == 0)
        return (PFN_vkVoidFunction)
            venus_real_CmdSetAttachmentFeedbackLoopEnableEXT;
    if (instance && strcmp(name, "vkCmdBindIndexBuffer") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdBindIndexBuffer;
    if (instance &&
        (strcmp(name, "vkCmdBindIndexBuffer2") == 0 ||
         strcmp(name, "vkCmdBindIndexBuffer2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdBindIndexBuffer2;
    if (instance &&
        (strcmp(name, "vkCmdSetCullMode") == 0 ||
         strcmp(name, "vkCmdSetCullModeEXT") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdSetCullMode;
    if (instance &&
        (strcmp(name, "vkCmdSetFrontFace") == 0 ||
         strcmp(name, "vkCmdSetFrontFaceEXT") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdSetFrontFace;
    if (instance &&
        (strcmp(name, "vkCmdSetPrimitiveTopology") == 0 ||
         strcmp(name, "vkCmdSetPrimitiveTopologyEXT") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdSetPrimitiveTopology;
    if (instance && strcmp(name, "vkCmdSetBlendConstants") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetBlendConstants;
    if (instance && strcmp(name, "vkCmdSetViewport") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetViewport;
    if (instance && strcmp(name, "vkCmdSetScissor") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetScissor;
    if (instance && strcmp(name, "vkCmdSetLineWidth") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetLineWidth;
    if (instance && strcmp(name, "vkCmdSetDepthBias") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetDepthBias;
    if (instance && strcmp(name, "vkCmdSetDepthBounds") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetDepthBounds;
    if (instance && strcmp(name, "vkCmdSetStencilCompareMask") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetStencilCompareMask;
    if (instance && strcmp(name, "vkCmdSetStencilWriteMask") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetStencilWriteMask;
    if (instance && strcmp(name, "vkCmdSetStencilReference") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdSetStencilReference;
    if (instance &&
        (strcmp(name, "vkCmdSetViewportWithCount") == 0 ||
         strcmp(name, "vkCmdSetViewportWithCountEXT") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdSetViewportWithCount;
    if (instance &&
        (strcmp(name, "vkCmdSetScissorWithCount") == 0 ||
         strcmp(name, "vkCmdSetScissorWithCountEXT") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdSetScissorWithCount;
    if (instance && strcmp(name, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdDraw;
    if (instance && strcmp(name, "vkCmdDrawIndexed") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdDrawIndexed;
    if (instance && strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdBindDescriptorSets;
    if (instance &&
        (strcmp(name, "vkCmdPushDescriptorSet") == 0 ||
         strcmp(name, "vkCmdPushDescriptorSetKHR") == 0))
        return (PFN_vkVoidFunction)venus_real_CmdPushDescriptorSet;
    if (instance && strcmp(name, "vkCmdPushConstants") == 0)
        return (PFN_vkVoidFunction)venus_real_CmdPushConstants;
    if (instance && strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateBuffer;
    if (instance && strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyBuffer;
    if (instance && strcmp(name, "vkCreateBufferView") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateBufferView;
    if (instance && strcmp(name, "vkDestroyBufferView") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyBufferView;
    if (instance && strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)venus_real_GetBufferMemoryRequirements;
    if (instance &&
        (strcmp(name, "vkGetBufferMemoryRequirements2") == 0 ||
         strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_GetBufferMemoryRequirements2;
    if (instance && strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)venus_real_BindBufferMemory;
    if (instance && strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)venus_real_AllocateMemory;
    if (instance && strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)venus_real_FreeMemory;
    if (instance && strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)venus_real_MapMemory;
    if (instance && strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)venus_real_UnmapMemory;
    if (instance && strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateImage;
    if (instance && strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyImage;
    if (instance && strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)venus_real_GetImageMemoryRequirements;
    if (instance &&
        (strcmp(name, "vkGetImageMemoryRequirements2") == 0 ||
         strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0))
        return (PFN_vkVoidFunction)venus_real_GetImageMemoryRequirements2;
    if (instance && strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)venus_real_BindImageMemory;
    if (instance && strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateImageView;
    if (instance && strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyImageView;
    if (instance && strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateSampler;
    if (instance && strcmp(name, "vkDestroySampler") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroySampler;
    if (instance && strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateShaderModule;
    if (instance && strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyShaderModule;
    if (instance && strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateGraphicsPipelines;
    if (instance && strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateComputePipelines;
    if (instance && strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyPipeline;
    if (instance && strcmp(name, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)venus_real_CreateDescriptorPool;
    if (instance && strcmp(name, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroyDescriptorPool;
    if (instance && strcmp(name, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)venus_real_ResetDescriptorPool;
    if (instance && strcmp(name, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)venus_real_AllocateDescriptorSets;
    if (instance && strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)venus_real_UpdateDescriptorSets;
    if (instance &&
        strcmp(name, "vkCreateOsitokCompositorSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)
            venus_real_CreateOsitokCompositorSurfaceKHR;
    if (instance && strcmp(name, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)venus_real_DestroySurfaceKHR;
    return 0;
}
