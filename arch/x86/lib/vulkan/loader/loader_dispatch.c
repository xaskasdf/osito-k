#include "loader.h"
#include <vulkan/vulkan_ositok.h>

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance, uint32_t *, VkPhysicalDevice *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);

/* W3a — device + queue + physical-device-properties forwards. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo *,
    const VkAllocationCallbacks *, VkDevice *);
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice, uint32_t, uint32_t, VkQueue *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice, VkPhysicalDeviceProperties *);

/* W3b.2 — additional phys-dev queries. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice, VkPhysicalDeviceFeatures *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);

/* W3b.3 — memory + buffer lifecycle. */
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice, const VkMemoryAllocateInfo *,
    const VkAllocationCallbacks *, VkDeviceMemory *);
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(
    VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
    VkMemoryMapFlags, void **);
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
    VkDevice, const VkBufferCreateInfo *,
    const VkAllocationCallbacks *, VkBuffer *);
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
    VkDevice, VkBuffer, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
    VkDevice, VkBuffer, VkMemoryRequirements *);
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
    VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);

/* W3b.4 — shader + render pass + image + framebuffer + pipeline + command. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
    VkDevice, const VkShaderModuleCreateInfo *,
    const VkAllocationCallbacks *, VkShaderModule *);
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice, VkShaderModule, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice, const VkRenderPassCreateInfo *,
    const VkAllocationCallbacks *, VkRenderPass *);
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
    VkDevice, VkRenderPass, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice, const VkImageCreateInfo *,
    const VkAllocationCallbacks *, VkImage *);
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice, VkImage, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
    VkDevice, VkImage, VkMemoryRequirements *);
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
    VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice, const VkImageViewCreateInfo *,
    const VkAllocationCallbacks *, VkImageView *);
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice, VkImageView, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice, const VkFramebufferCreateInfo *,
    const VkAllocationCallbacks *, VkFramebuffer *);
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
    VkDevice, VkFramebuffer, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(
    VkDevice, const VkPipelineLayoutCreateInfo *,
    const VkAllocationCallbacks *, VkPipelineLayout *);
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice, VkPipelineLayout, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice, VkPipelineCache, uint32_t,
    const VkGraphicsPipelineCreateInfo *,
    const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
    VkDevice, VkPipeline, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice, const VkCommandPoolCreateInfo *,
    const VkAllocationCallbacks *, VkCommandPool *);
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice, VkCommandPool, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
    VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(
    VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);

/* W3b.6 — vertex input + dynamic state. */
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer, uint32_t, uint32_t,
    const VkBuffer *, const VkDeviceSize *);
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
    VkCommandBuffer, uint32_t, uint32_t, const VkViewport *);
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
    VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *);

/* W3b.5 — WSI + surface + swapchain + queue + sync + present. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateOsitokCompositorSurfaceKHR(
    VkInstance, const VkOsitoCompositorSurfaceCreateInfoOSITOK *,
    const VkAllocationCallbacks *, VkSurfaceKHR *);
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue);
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
    VkDevice, const VkFenceCreateInfo *,
    const VkAllocationCallbacks *, VkFence *);
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(
    VkDevice, VkFence, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice, uint32_t, const VkFence *);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(
    VkDevice, const VkSemaphoreCreateInfo *,
    const VkAllocationCallbacks *, VkSemaphore *);
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
    VkDevice, VkSemaphore, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR *,
    const VkAllocationCallbacks *, VkSwapchainKHR *);
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue, const VkPresentInfoKHR *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);

/* W4.8 — additional instance-level entry points required by Mesa+Zink
 * for screen creation. Most can chain into the W4.7 fallback or just
 * fill plausible defaults. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice, VkFormat, VkFormatProperties *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling,
    VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties *);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *,
    VkImageFormatProperties2 *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDebugUtilsMessengerEXT(
    VkInstance, const VkDebugUtilsMessengerCreateInfoEXT *,
    const VkAllocationCallbacks *, VkDebugUtilsMessengerEXT *);
VKAPI_ATTR void VKAPI_CALL vkDestroyDebugUtilsMessengerEXT(
    VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(
    VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *,
    uint32_t, const VkImageSubresourceRange *);

/* W4.7-fix: stub returning VK_ERROR_FEATURE_NOT_PRESENT for any
 * Vulkan symbol we don't implement. Mesa/Zink/DXVK check returns
 * and bail gracefully instead of dereferencing a NULL function ptr. */
VKAPI_ATTR VkResult VKAPI_CALL osito_vk_unimplemented_stub(void);

PFN_vkVoidFunction
osito_loader_get_instance_proc_addr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;

    /* Global entry points (no instance needed). */
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    /* W4.7-fix: Zink calls these to discover layers/extensions. Stub
     * to "no extensions, no layers" — apps see a vanilla Vulkan stack. */
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
    if (strcmp(pName, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
    if (strcmp(pName, "vkEnumerateDeviceLayerProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties;

    /* Instance-scoped entry points. */
    if (strcmp(pName, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)vkDestroyInstance;
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;

    /* W3a additions — device lifecycle + physical-device properties. */
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)vkCreateDevice;
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (strcmp(pName, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;

    /* W3b.2 additions — three more phys-dev queries. */
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures;
    if (strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties;

    /* W3b.3 additions — device memory + buffer. */
    if (strcmp(pName, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)vkAllocateMemory;
    if (strcmp(pName, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)vkFreeMemory;
    if (strcmp(pName, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)vkMapMemory;
    if (strcmp(pName, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)vkUnmapMemory;
    if (strcmp(pName, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)vkCreateBuffer;
    if (strcmp(pName, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)vkDestroyBuffer;
    if (strcmp(pName, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements;
    if (strcmp(pName, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)vkBindBufferMemory;

    /* W3b.4 additions. */
    if (strcmp(pName, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)vkCreateShaderModule;
    if (strcmp(pName, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)vkDestroyShaderModule;
    if (strcmp(pName, "vkCreateRenderPass") == 0)
        return (PFN_vkVoidFunction)vkCreateRenderPass;
    if (strcmp(pName, "vkDestroyRenderPass") == 0)
        return (PFN_vkVoidFunction)vkDestroyRenderPass;
    if (strcmp(pName, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)vkCreateImage;
    if (strcmp(pName, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)vkDestroyImage;
    if (strcmp(pName, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements;
    if (strcmp(pName, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)vkBindImageMemory;
    if (strcmp(pName, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)vkCreateImageView;
    if (strcmp(pName, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)vkDestroyImageView;
    if (strcmp(pName, "vkCreateFramebuffer") == 0)
        return (PFN_vkVoidFunction)vkCreateFramebuffer;
    if (strcmp(pName, "vkDestroyFramebuffer") == 0)
        return (PFN_vkVoidFunction)vkDestroyFramebuffer;
    if (strcmp(pName, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkCreatePipelineLayout;
    if (strcmp(pName, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipelineLayout;
    if (strcmp(pName, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateGraphicsPipelines;
    if (strcmp(pName, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipeline;
    if (strcmp(pName, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)vkCreateCommandPool;
    if (strcmp(pName, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyCommandPool;
    if (strcmp(pName, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
    if (strcmp(pName, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkFreeCommandBuffers;
    if (strcmp(pName, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkBeginCommandBuffer;
    if (strcmp(pName, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkEndCommandBuffer;
    if (strcmp(pName, "vkCmdBeginRenderPass") == 0)
        return (PFN_vkVoidFunction)vkCmdBeginRenderPass;
    if (strcmp(pName, "vkCmdEndRenderPass") == 0)
        return (PFN_vkVoidFunction)vkCmdEndRenderPass;
    if (strcmp(pName, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)vkCmdBindPipeline;
    if (strcmp(pName, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)vkCmdDraw;

    /* W3b.6 — vertex input + dynamic state. */
    if (strcmp(pName, "vkCmdBindVertexBuffers") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers;
    if (strcmp(pName, "vkCmdSetViewport") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewport;
    if (strcmp(pName, "vkCmdSetScissor") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissor;

    /* W3b.5 additions — WSI + surface + swapchain + queue + sync. */
    if (strcmp(pName, "vkCreateOsitokCompositorSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateOsitokCompositorSurfaceKHR;
    if (strcmp(pName, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroySurfaceKHR;
    if (strcmp(pName, "vkQueueSubmit") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit;
    if (strcmp(pName, "vkQueueWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkQueueWaitIdle;
    if (strcmp(pName, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkDeviceWaitIdle;
    if (strcmp(pName, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)vkCreateFence;
    if (strcmp(pName, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)vkDestroyFence;
    if (strcmp(pName, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)vkResetFences;
    if (strcmp(pName, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)vkWaitForFences;
    if (strcmp(pName, "vkGetFenceStatus") == 0)
        return (PFN_vkVoidFunction)vkGetFenceStatus;
    if (strcmp(pName, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)vkCreateSemaphore;
    if (strcmp(pName, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)vkDestroySemaphore;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)vkQueuePresentKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceFormatsKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfacePresentModesKHR;
    if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceSupportKHR;

    /* W4.8 — instance-level entry points required by Mesa+Zink for
     * screen creation. */
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2;
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2;
    if (strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties2;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2;
    if (strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2;
    if (strcmp(pName, "vkCreateDebugUtilsMessengerEXT") == 0)
        return (PFN_vkVoidFunction)vkCreateDebugUtilsMessengerEXT;
    if (strcmp(pName, "vkDestroyDebugUtilsMessengerEXT") == 0)
        return (PFN_vkVoidFunction)vkDestroyDebugUtilsMessengerEXT;
    if (strcmp(pName, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearColorImage;

    /* Unknown — fall through to the first ICD that resolves it. Matches
     * the spec's language that unknown queries may return NULL when no
     * extension is enabled, but a forward is a friendlier default for
     * debugging. */
    if (instance) {
        struct osito_instance *self = osito_instance_from(instance);
        for (unsigned i = 0; i < self->icd_instance_count; i++) {
            struct osito_icd_inst *ci = &self->icd_instances[i];
            PFN_vkVoidFunction fn = ci->icd->get_proc_addr(ci->handle, pName);
            if (fn) return fn;
        }
    }
    /* W4.7-fix: rather than NULL (which crashes Mesa when it later
     * calls a NULL function pointer), return a stub that just yields
     * VK_ERROR_FEATURE_NOT_PRESENT. Mesa+Zink check returns and bail
     * gracefully on unsupported entries. */
    return (PFN_vkVoidFunction)osito_vk_unimplemented_stub;
}

VKAPI_ATTR VkResult VKAPI_CALL
osito_vk_unimplemented_stub(void) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return osito_loader_get_instance_proc_addr(instance, pName);
}

/* W4.7-fix: Mesa Zink calls vkGetDeviceProcAddr to populate its
 * device-level dispatch table. Forward to the same loader path —
 * device-level resolution falls back to instance-level for everything
 * we currently support. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    (void)device;
    return osito_loader_get_instance_proc_addr((VkInstance)0, pName);
}

/* W4.7-fix: instance-level discovery functions Zink + Mesa rely on. */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                       uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties) {
    (void)pLayerName;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    /* No instance extensions advertised — apps see a vanilla Vulkan 1.4
     * core stack. Add VK_KHR_surface etc. when WSI integration lands. */
    *pPropertyCount = 0;
    (void)pProperties;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                   VkLayerProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    (void)pProperties;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_API_VERSION_1_4;
    return VK_SUCCESS;
}

/* W4.7-fix: Zink probes device-level extensions; we report none, so
 * Zink falls back to core Vulkan paths. */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                     const char *pLayerName,
                                     uint32_t *pPropertyCount,
                                     VkExtensionProperties *pProperties) {
    (void)physicalDevice; (void)pLayerName;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    (void)pProperties;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                 uint32_t *pPropertyCount,
                                 VkLayerProperties *pProperties) {
    (void)physicalDevice;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    (void)pProperties;
    return VK_SUCCESS;
}

/* ---------------- W3b.2 trampolines ---------------------------------------
 *
 * Each VkPhysicalDevice returned by vkEnumeratePhysicalDevices is wrapped
 * in an osito_phys_device (loader.h). Trampolines unwrap to the owning
 * ICD and dispatch against that ICD's real handle — fixing the W3a
 * "first ICD wins" gotcha.
 *
 * VkDevice follows the same wrapping in vkCreateDevice below. */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice,
               const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkDevice *pDevice) {
    if (!physicalDevice || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *pw = osito_phys_from(physicalDevice);
    struct osito_icd_inst    *ci = pw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkCreateDevice fn = (PFN_vkCreateDevice)
        ci->icd->get_proc_addr(ci->handle, "vkCreateDevice");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkDevice icd_dev = VK_NULL_HANDLE;
    VkResult rc = fn(pw->real, pCreateInfo, pAllocator, &icd_dev);
    if (rc != VK_SUCCESS || !icd_dev) return rc;

    struct osito_device *dw = malloc(sizeof(*dw));
    if (!dw) {
        /* Best-effort destroy of the orphan ICD device. */
        PFN_vkDestroyDevice drop = (PFN_vkDestroyDevice)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyDevice");
        if (drop) drop(icd_dev, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(dw, 0, sizeof(*dw));
    set_loader_magic_value(dw);
    dw->owner = ci;
    dw->real  = icd_dev;

    *pDevice = osito_device_to(dw);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    if (!device) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    if (ci) {
        PFN_vkDestroyDevice fn = (PFN_vkDestroyDevice)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyDevice");
        if (fn) fn(dw->real, pAllocator);
    }
    free(dw);
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                 uint32_t queueIndex, VkQueue *pQueue) {
    if (!device || !pQueue) { if (pQueue) *pQueue = VK_NULL_HANDLE; return; }
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    if (!ci) { *pQueue = VK_NULL_HANDLE; return; }
    PFN_vkGetDeviceQueue fn = (PFN_vkGetDeviceQueue)
        ci->icd->get_proc_addr(ci->handle, "vkGetDeviceQueue");
    if (!fn) { *pQueue = VK_NULL_HANDLE; return; }
    VkQueue real = VK_NULL_HANDLE;
    fn(dw->real, queueFamilyIndex, queueIndex, &real);
    if (!real) { *pQueue = VK_NULL_HANDLE; return; }
    /* W3b.5: wrap the ICD-returned queue so QueueSubmit / QueuePresent
     * trampolines can find the owning device via a back-pointer. */
    struct osito_queue *w = malloc(sizeof(*w));
    if (!w) { *pQueue = VK_NULL_HANDLE; return; }
    memset(w, 0, sizeof(*w));
    set_loader_magic_value(w);
    w->owner = dw;
    w->real  = real;
    *pQueue = (VkQueue)w;
}

/* Tiny unwrap helper for the phys-dev trampolines below. */
static inline int osito_unwrap_phys(VkPhysicalDevice h,
                                    struct osito_icd_inst **ci_out,
                                    VkPhysicalDevice *real_out) {
    if (!h) return 0;
    struct osito_phys_device *pw = osito_phys_from(h);
    if (!pw || !pw->owner) return 0;
    *ci_out   = pw->owner;
    *real_out = pw->real;
    return 1;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceProperties *pProperties) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceProperties fn = (PFN_vkGetPhysicalDeviceProperties)
        ci->icd->get_proc_addr(ci->handle, "vkGetPhysicalDeviceProperties");
    if (fn) fn(real, pProperties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                            VkPhysicalDeviceFeatures *pFeatures) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceFeatures fn = (PFN_vkGetPhysicalDeviceFeatures)
        ci->icd->get_proc_addr(ci->handle, "vkGetPhysicalDeviceFeatures");
    if (fn) fn(real, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice physicalDevice,
        uint32_t *pCount, VkQueueFamilyProperties *pFamilies) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (fn) fn(real, pCount, pFamilies);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties *pMem) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceMemoryProperties fn =
        (PFN_vkGetPhysicalDeviceMemoryProperties)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceMemoryProperties");
    if (fn) fn(real, pMem);
}

/* ---------------- W3b.3 trampolines ---------------------------------------
 *
 * Memory + buffer handles are non-dispatchable (plain u64). The loader
 * wraps each returned handle in a heap-allocated struct so trampolines
 * can recover the owning device/ICD and unwrap the ICD-side real handle.
 *
 * See W3b.2 for the matching VkDevice wrapping; these helpers reuse that
 * per-device dispatch machinery. */

static inline struct osito_memory *mem_from(VkDeviceMemory h) {
    return (struct osito_memory *)(uintptr_t)h;
}
static inline VkDeviceMemory mem_to(struct osito_memory *w) {
    return (VkDeviceMemory)(uintptr_t)w;
}
static inline struct osito_buffer *buf_from(VkBuffer h) {
    return (struct osito_buffer *)(uintptr_t)h;
}
static inline VkBuffer buf_to(struct osito_buffer *w) {
    return (VkBuffer)(uintptr_t)w;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAI,
                 const VkAllocationCallbacks *pAllocator,
                 VkDeviceMemory *pMemory) {
    if (!device || !pAI || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkAllocateMemory fn = (PFN_vkAllocateMemory)
        ci->icd->get_proc_addr(ci->handle, "vkAllocateMemory");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceMemory real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pAI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;

    struct osito_memory *mw = malloc(sizeof(*mw));
    if (!mw) {
        PFN_vkFreeMemory drop = (PFN_vkFreeMemory)
            ci->icd->get_proc_addr(ci->handle, "vkFreeMemory");
        if (drop) drop(dw->real, real, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(mw, 0, sizeof(*mw));
    mw->owner = dw;
    mw->real  = real;
    *pMemory = mem_to(mw);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory,
             const VkAllocationCallbacks *pAllocator) {
    if (!device || !memory) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    struct osito_memory    *mw = mem_from(memory);
    if (ci) {
        PFN_vkFreeMemory fn = (PFN_vkFreeMemory)
            ci->icd->get_proc_addr(ci->handle, "vkFreeMemory");
        if (fn) fn(dw->real, mw->real, pAllocator);
    }
    free(mw);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory,
            VkDeviceSize offset, VkDeviceSize size,
            VkMemoryMapFlags flags, void **ppData) {
    if (!device || !memory || !ppData) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    struct osito_memory    *mw = mem_from(memory);
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkMapMemory fn = (PFN_vkMapMemory)
        ci->icd->get_proc_addr(ci->handle, "vkMapMemory");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(dw->real, mw->real, offset, size, flags, ppData);
}

VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    if (!device || !memory) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    struct osito_memory    *mw = mem_from(memory);
    if (!ci) return;
    PFN_vkUnmapMemory fn = (PFN_vkUnmapMemory)
        ci->icd->get_proc_addr(ci->handle, "vkUnmapMemory");
    if (fn) fn(dw->real, mw->real);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCI,
               const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    if (!device || !pCI || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateBuffer fn = (PFN_vkCreateBuffer)
        ci->icd->get_proc_addr(ci->handle, "vkCreateBuffer");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkBuffer real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;

    struct osito_buffer *bw = malloc(sizeof(*bw));
    if (!bw) {
        PFN_vkDestroyBuffer drop = (PFN_vkDestroyBuffer)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyBuffer");
        if (drop) drop(dw->real, real, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(bw, 0, sizeof(*bw));
    bw->owner = dw;
    bw->real  = real;
    *pBuffer = buf_to(bw);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                const VkAllocationCallbacks *pAllocator) {
    if (!device || !buffer) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    struct osito_buffer    *bw = buf_from(buffer);
    if (ci) {
        PFN_vkDestroyBuffer fn = (PFN_vkDestroyBuffer)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyBuffer");
        if (fn) fn(dw->real, bw->real, pAllocator);
    }
    free(bw);
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                              VkMemoryRequirements *pReqs) {
    if (!device || !buffer || !pReqs) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    struct osito_buffer    *bw = buf_from(buffer);
    if (!ci) return;
    PFN_vkGetBufferMemoryRequirements fn =
        (PFN_vkGetBufferMemoryRequirements)ci->icd->get_proc_addr(
            ci->handle, "vkGetBufferMemoryRequirements");
    if (fn) fn(dw->real, bw->real, pReqs);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer,
                   VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    if (!device || !buffer || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    struct osito_buffer    *bw = buf_from(buffer);
    struct osito_memory    *mw = mem_from(memory);
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBindBufferMemory fn = (PFN_vkBindBufferMemory)
        ci->icd->get_proc_addr(ci->handle, "vkBindBufferMemory");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(dw->real, bw->real, mw->real, memoryOffset);
}

/* ---------------- W3b.4 trampolines ---------------------------------------
 *
 * All remaining W3b.4 non-dispatchable objects wrap as {owner, real}.
 * Command buffers are dispatchable — they get VK_LOADER_DATA.
 */

#define NDH_FROM(T, h) ((struct T *)(uintptr_t)(h))
#define NDH_TO(T, w)   ((T)(uintptr_t)(w))

static inline struct osito_shader *sm_from(VkShaderModule h) { return (struct osito_shader *)(uintptr_t)h; }
static inline VkShaderModule      sm_to(struct osito_shader *w) { return (VkShaderModule)(uintptr_t)w; }
static inline struct osito_render_pass *rp_from(VkRenderPass h) { return (struct osito_render_pass *)(uintptr_t)h; }
static inline VkRenderPass             rp_to(struct osito_render_pass *w) { return (VkRenderPass)(uintptr_t)w; }
static inline struct osito_image *img_from(VkImage h) { return (struct osito_image *)(uintptr_t)h; }
static inline VkImage            img_to(struct osito_image *w) { return (VkImage)(uintptr_t)w; }
static inline struct osito_image_view *iv_from(VkImageView h) { return (struct osito_image_view *)(uintptr_t)h; }
static inline VkImageView             iv_to(struct osito_image_view *w) { return (VkImageView)(uintptr_t)w; }
static inline struct osito_framebuffer *fb_from(VkFramebuffer h) { return (struct osito_framebuffer *)(uintptr_t)h; }
static inline VkFramebuffer            fb_to(struct osito_framebuffer *w) { return (VkFramebuffer)(uintptr_t)w; }
static inline struct osito_pipeline_layout *pl_from(VkPipelineLayout h) { return (struct osito_pipeline_layout *)(uintptr_t)h; }
static inline VkPipelineLayout             pl_to(struct osito_pipeline_layout *w) { return (VkPipelineLayout)(uintptr_t)w; }
static inline struct osito_pipeline *pip_from(VkPipeline h) { return (struct osito_pipeline *)(uintptr_t)h; }
static inline VkPipeline            pip_to(struct osito_pipeline *w) { return (VkPipeline)(uintptr_t)w; }
static inline struct osito_cmd_pool *cp_from(VkCommandPool h) { return (struct osito_cmd_pool *)(uintptr_t)h; }
static inline VkCommandPool         cp_to(struct osito_cmd_pool *w) { return (VkCommandPool)(uintptr_t)w; }

/* Shader module */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCI,
                     const VkAllocationCallbacks *pAllocator,
                     VkShaderModule *pShader) {
    if (!device || !pCI || !pShader) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateShaderModule fn = (PFN_vkCreateShaderModule)
        ci->icd->get_proc_addr(ci->handle, "vkCreateShaderModule");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkShaderModule real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_shader *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pShader = sm_to(w);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice device, VkShaderModule shader,
                      const VkAllocationCallbacks *pAllocator) {
    if (!device || !shader) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_shader *w = sm_from(shader);
    if (ci) {
        PFN_vkDestroyShaderModule fn = (PFN_vkDestroyShaderModule)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyShaderModule");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Render pass */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCI,
                   const VkAllocationCallbacks *pAllocator,
                   VkRenderPass *pRP) {
    if (!device || !pCI || !pRP) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateRenderPass fn = (PFN_vkCreateRenderPass)
        ci->icd->get_proc_addr(ci->handle, "vkCreateRenderPass");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkRenderPass real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_render_pass *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pRP = rp_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyRenderPass(VkDevice device, VkRenderPass rpH,
                    const VkAllocationCallbacks *pAllocator) {
    if (!device || !rpH) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_render_pass *w = rp_from(rpH);
    if (ci) {
        PFN_vkDestroyRenderPass fn = (PFN_vkDestroyRenderPass)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyRenderPass");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Image */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo *pCI,
              const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    if (!device || !pCI || !pImage) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateImage fn = (PFN_vkCreateImage)
        ci->icd->get_proc_addr(ci->handle, "vkCreateImage");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkImage real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_image *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pImage = img_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image,
               const VkAllocationCallbacks *pAllocator) {
    if (!device || !image) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_image *w = img_from(image);
    if (ci) {
        PFN_vkDestroyImage fn = (PFN_vkDestroyImage)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyImage");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}
VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                             VkMemoryRequirements *pReqs) {
    if (!device || !image || !pReqs) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_image *w = img_from(image);
    if (!ci) return;
    PFN_vkGetImageMemoryRequirements fn = (PFN_vkGetImageMemoryRequirements)
        ci->icd->get_proc_addr(ci->handle, "vkGetImageMemoryRequirements");
    if (fn) fn(dw->real, w->real, pReqs);
}
VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                  VkDeviceSize memoryOffset) {
    if (!device || !image || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_image *w = img_from(image);
    struct osito_memory *mw = mem_from(memory);
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBindImageMemory fn = (PFN_vkBindImageMemory)
        ci->icd->get_proc_addr(ci->handle, "vkBindImageMemory");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(dw->real, w->real, mw->real, memoryOffset);
}

/* Image view */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCI,
                  const VkAllocationCallbacks *pAllocator, VkImageView *pView) {
    if (!device || !pCI || !pView) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateImageView fn = (PFN_vkCreateImageView)
        ci->icd->get_proc_addr(ci->handle, "vkCreateImageView");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    /* Unwrap image handle in the create info. */
    VkImageViewCreateInfo tmp = *pCI;
    if (pCI->image) {
        struct osito_image *iw = img_from(pCI->image);
        tmp.image = iw->real;
    }
    VkImageView real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, &tmp, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_image_view *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pView = iv_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView view,
                   const VkAllocationCallbacks *pAllocator) {
    if (!device || !view) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_image_view *w = iv_from(view);
    if (ci) {
        PFN_vkDestroyImageView fn = (PFN_vkDestroyImageView)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyImageView");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Framebuffer */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCI,
                    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFB) {
    if (!device || !pCI || !pFB) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateFramebuffer fn = (PFN_vkCreateFramebuffer)
        ci->icd->get_proc_addr(ci->handle, "vkCreateFramebuffer");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    /* Unwrap rp + view handles into a local copy. Limit to 8 atts. */
    #define OSITO_FB_MAX_ATTS 8u
    VkImageView real_views[OSITO_FB_MAX_ATTS] = {0};
    uint32_t n = pCI->attachmentCount;
    if (n > OSITO_FB_MAX_ATTS) n = OSITO_FB_MAX_ATTS;
    for (uint32_t i = 0; i < n; i++) {
        struct osito_image_view *vw = iv_from(pCI->pAttachments[i]);
        real_views[i] = vw ? vw->real : 0;
    }
    struct osito_render_pass *rpw = rp_from(pCI->renderPass);
    VkFramebufferCreateInfo tmp = *pCI;
    tmp.renderPass = rpw ? rpw->real : 0;
    tmp.attachmentCount = n;
    tmp.pAttachments = real_views;

    VkFramebuffer real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, &tmp, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_framebuffer *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pFB = fb_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyFramebuffer(VkDevice device, VkFramebuffer fbH,
                     const VkAllocationCallbacks *pAllocator) {
    if (!device || !fbH) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_framebuffer *w = fb_from(fbH);
    if (ci) {
        PFN_vkDestroyFramebuffer fn = (PFN_vkDestroyFramebuffer)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyFramebuffer");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Pipeline layout */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCI,
                       const VkAllocationCallbacks *pAllocator,
                       VkPipelineLayout *pLayout) {
    if (!device || !pCI || !pLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreatePipelineLayout fn = (PFN_vkCreatePipelineLayout)
        ci->icd->get_proc_addr(ci->handle, "vkCreatePipelineLayout");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineLayout real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_pipeline_layout *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pLayout = pl_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout,
                        const VkAllocationCallbacks *pAllocator) {
    if (!device || !layout) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_pipeline_layout *w = pl_from(layout);
    if (ci) {
        PFN_vkDestroyPipelineLayout fn = (PFN_vkDestroyPipelineLayout)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyPipelineLayout");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Graphics pipelines */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                          uint32_t createInfoCount,
                          const VkGraphicsPipelineCreateInfo *pCreateInfos,
                          const VkAllocationCallbacks *pAllocator,
                          VkPipeline *pPipelines) {
    if (!device || !pCreateInfos || !pPipelines || createInfoCount != 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateGraphicsPipelines fn = (PFN_vkCreateGraphicsPipelines)
        ci->icd->get_proc_addr(ci->handle, "vkCreateGraphicsPipelines");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    /* Unwrap: layout, rp, stage modules. Copy inputs into locals because
     * VkGraphicsPipelineCreateInfo contains pointers we don't own. */
    VkGraphicsPipelineCreateInfo tmp = pCreateInfos[0];
    VkPipelineShaderStageCreateInfo stages[2];
    if (tmp.stageCount > 2) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < tmp.stageCount; i++) {
        stages[i] = tmp.pStages[i];
        if (stages[i].module) {
            struct osito_shader *sw = sm_from(stages[i].module);
            stages[i].module = sw->real;
        }
    }
    tmp.pStages = stages;
    if (tmp.layout) tmp.layout = pl_from(tmp.layout)->real;
    if (tmp.renderPass) tmp.renderPass = rp_from(tmp.renderPass)->real;

    VkPipeline real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pipelineCache, 1, &tmp, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_pipeline *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    pPipelines[0] = pip_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                  const VkAllocationCallbacks *pAllocator) {
    if (!device || !pipeline) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_pipeline *w = pip_from(pipeline);
    if (ci) {
        PFN_vkDestroyPipeline fn = (PFN_vkDestroyPipeline)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyPipeline");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Command pool */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *pCI,
                    const VkAllocationCallbacks *pAllocator,
                    VkCommandPool *pPool) {
    if (!device || !pCI || !pPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateCommandPool fn = (PFN_vkCreateCommandPool)
        ci->icd->get_proc_addr(ci->handle, "vkCreateCommandPool");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandPool real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_cmd_pool *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pPool = cp_to(w);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(VkDevice device, VkCommandPool pool,
                     const VkAllocationCallbacks *pAllocator) {
    if (!device || !pool) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_cmd_pool *w = cp_from(pool);
    if (ci) {
        PFN_vkDestroyCommandPool fn = (PFN_vkDestroyCommandPool)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyCommandPool");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* Command buffers (dispatchable). */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice device,
                         const VkCommandBufferAllocateInfo *pInfo,
                         VkCommandBuffer *pCmdBuffers) {
    if (!device || !pInfo || !pCmdBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkAllocateCommandBuffers fn = (PFN_vkAllocateCommandBuffers)
        ci->icd->get_proc_addr(ci->handle, "vkAllocateCommandBuffers");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t count = pInfo->commandBufferCount;
    if (count == 0) return VK_SUCCESS;
    /* Unwrap pool. */
    VkCommandBufferAllocateInfo tmp = *pInfo;
    if (tmp.commandPool) tmp.commandPool = cp_from(tmp.commandPool)->real;
    #define OSITO_CB_MAX 32u
    VkCommandBuffer reals[OSITO_CB_MAX] = {0};
    if (count > OSITO_CB_MAX) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult rc = fn(dw->real, &tmp, reals);
    if (rc != VK_SUCCESS) return rc;
    for (uint32_t i = 0; i < count; i++) {
        struct osito_cmd_buffer *w = malloc(sizeof(*w));
        if (!w) {
            /* Best-effort rollback: free preceding wrappers. */
            for (uint32_t j = 0; j < i; j++)
                free((void *)pCmdBuffers[j]);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memset(w, 0, sizeof(*w));
        set_loader_magic_value(w);
        w->owner = dw;
        w->real  = reals[i];
        pCmdBuffers[i] = (VkCommandBuffer)w;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers(VkDevice device, VkCommandPool pool,
                     uint32_t count, const VkCommandBuffer *pCmdBuffers) {
    if (!device || !pCmdBuffers || count == 0) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return;
    PFN_vkFreeCommandBuffers fn = (PFN_vkFreeCommandBuffers)
        ci->icd->get_proc_addr(ci->handle, "vkFreeCommandBuffers");
    if (!fn) return;
    VkCommandPool real_pool = pool ? cp_from(pool)->real : 0;
    #define OSITO_CB_FREE_MAX 32u
    VkCommandBuffer reals[OSITO_CB_FREE_MAX] = {0};
    uint32_t n = count < OSITO_CB_FREE_MAX ? count : OSITO_CB_FREE_MAX;
    for (uint32_t i = 0; i < n; i++) {
        struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)pCmdBuffers[i];
        reals[i] = w ? w->real : 0;
    }
    fn(dw->real, real_pool, n, reals);
    for (uint32_t i = 0; i < n; i++)
        free((void *)pCmdBuffers[i]);
}

/* Command recording — cb is dispatchable; get device via back-pointer. */
VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *pBegin) {
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBeginCommandBuffer fn = (PFN_vkBeginCommandBuffer)
        ci->icd->get_proc_addr(ci->handle, "vkBeginCommandBuffer");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(w->real, pBegin);
}
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer cb) {
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEndCommandBuffer fn = (PFN_vkEndCommandBuffer)
        ci->icd->get_proc_addr(ci->handle, "vkEndCommandBuffer");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(w->real);
}
VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderPass(VkCommandBuffer cb,
                     const VkRenderPassBeginInfo *pBegin,
                     VkSubpassContents contents) {
    if (!cb || !pBegin) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdBeginRenderPass fn = (PFN_vkCmdBeginRenderPass)
        ci->icd->get_proc_addr(ci->handle, "vkCmdBeginRenderPass");
    if (!fn) return;
    VkRenderPassBeginInfo tmp = *pBegin;
    if (tmp.renderPass)  tmp.renderPass  = rp_from(tmp.renderPass)->real;
    if (tmp.framebuffer) tmp.framebuffer = fb_from(tmp.framebuffer)->real;
    fn(w->real, &tmp, contents);
}
VKAPI_ATTR void VKAPI_CALL
vkCmdEndRenderPass(VkCommandBuffer cb) {
    if (!cb) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdEndRenderPass fn = (PFN_vkCmdEndRenderPass)
        ci->icd->get_proc_addr(ci->handle, "vkCmdEndRenderPass");
    if (fn) fn(w->real);
}
VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipeline) {
    if (!cb) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdBindPipeline fn = (PFN_vkCmdBindPipeline)
        ci->icd->get_proc_addr(ci->handle, "vkCmdBindPipeline");
    if (!fn) return;
    VkPipeline real = pipeline ? pip_from(pipeline)->real : 0;
    fn(w->real, bp, real);
}
VKAPI_ATTR void VKAPI_CALL
vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount,
          uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    if (!cb) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdDraw fn = (PFN_vkCmdDraw)
        ci->icd->get_proc_addr(ci->handle, "vkCmdDraw");
    if (!fn) return;
    fn(w->real, vertexCount, instanceCount, firstVertex, firstInstance);
}

/* ---------------- W3b.6 trampolines ---------------------------------------
 *
 * Vertex input + dynamic state. No new wrapper types — VkBuffer wrappers
 * already exist (osito_buffer), and VkViewport / VkRect2D are POD.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t firstBinding,
                       uint32_t bindingCount, const VkBuffer *pBuffers,
                       const VkDeviceSize *pOffsets) {
    if (!cb || !pBuffers) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdBindVertexBuffers fn = (PFN_vkCmdBindVertexBuffers)
        ci->icd->get_proc_addr(ci->handle, "vkCmdBindVertexBuffers");
    if (!fn) return;
    /* Translate wrapper VkBuffers -> ICD-side real handles. Use a small
     * stack array; we cap at 8 (the ICD encoder also caps there). */
    VkBuffer real_bufs[8];
    uint32_t n = bindingCount;
    if (n > 8u) n = 8u;
    for (uint32_t i = 0; i < n; i++)
        real_bufs[i] = pBuffers[i] ? buf_from(pBuffers[i])->real : 0;
    fn(w->real, firstBinding, n, real_bufs, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetViewport(VkCommandBuffer cb, uint32_t firstViewport,
                 uint32_t viewportCount, const VkViewport *pViewports) {
    if (!cb) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdSetViewport fn = (PFN_vkCmdSetViewport)
        ci->icd->get_proc_addr(ci->handle, "vkCmdSetViewport");
    if (!fn) return;
    fn(w->real, firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetScissor(VkCommandBuffer cb, uint32_t firstScissor,
                uint32_t scissorCount, const VkRect2D *pScissors) {
    if (!cb) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdSetScissor fn = (PFN_vkCmdSetScissor)
        ci->icd->get_proc_addr(ci->handle, "vkCmdSetScissor");
    if (!fn) return;
    fn(w->real, firstScissor, scissorCount, pScissors);
}

/* ---------------- W3b.5 trampolines ---------------------------------------
 *
 * Surface is instance-scoped (creates a wrapper owned by the VkInstance).
 * Queue is dispatchable — wrapped similarly to VkCommandBuffer.
 * Fence/Semaphore/Swapchain are non-dispatchable, same {owner, real}
 * wrappers as the W3b.3/W3b.4 pattern.
 */

typedef VkResult (VKAPI_PTR *PFN_vkCreateOsitokCompositorSurfaceKHR)(
    VkInstance, const VkOsitoCompositorSurfaceCreateInfoOSITOK *,
    const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef void (VKAPI_PTR *PFN_vkDestroySurfaceKHR)(
    VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
typedef VkResult (VKAPI_PTR *PFN_vkQueueSubmit)(
    VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
typedef VkResult (VKAPI_PTR *PFN_vkQueueWaitIdle)(VkQueue);
typedef VkResult (VKAPI_PTR *PFN_vkDeviceWaitIdle)(VkDevice);
typedef VkResult (VKAPI_PTR *PFN_vkCreateFence)(
    VkDevice, const VkFenceCreateInfo *,
    const VkAllocationCallbacks *, VkFence *);
typedef void (VKAPI_PTR *PFN_vkDestroyFence)(
    VkDevice, VkFence, const VkAllocationCallbacks *);
typedef VkResult (VKAPI_PTR *PFN_vkResetFences)(
    VkDevice, uint32_t, const VkFence *);
typedef VkResult (VKAPI_PTR *PFN_vkWaitForFences)(
    VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
typedef VkResult (VKAPI_PTR *PFN_vkGetFenceStatus)(VkDevice, VkFence);
typedef VkResult (VKAPI_PTR *PFN_vkCreateSemaphore)(
    VkDevice, const VkSemaphoreCreateInfo *,
    const VkAllocationCallbacks *, VkSemaphore *);
typedef void (VKAPI_PTR *PFN_vkDestroySemaphore)(
    VkDevice, VkSemaphore, const VkAllocationCallbacks *);
typedef VkResult (VKAPI_PTR *PFN_vkCreateSwapchainKHR)(
    VkDevice, const VkSwapchainCreateInfoKHR *,
    const VkAllocationCallbacks *, VkSwapchainKHR *);
typedef void (VKAPI_PTR *PFN_vkDestroySwapchainKHR)(
    VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainImagesKHR)(
    VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
typedef VkResult (VKAPI_PTR *PFN_vkAcquireNextImageKHR)(
    VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
typedef VkResult (VKAPI_PTR *PFN_vkQueuePresentKHR)(
    VkQueue, const VkPresentInfoKHR *);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)(
    VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(
    VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);

/* Wrappers for non-dispatchable types. */
static inline struct osito_surface   *surf_from(VkSurfaceKHR h) { return (struct osito_surface *)(uintptr_t)h; }
static inline VkSurfaceKHR            surf_to(struct osito_surface *w) { return (VkSurfaceKHR)(uintptr_t)w; }
static inline struct osito_fence     *fence_from(VkFence h) { return (struct osito_fence *)(uintptr_t)h; }
static inline VkFence                 fence_to(struct osito_fence *w) { return (VkFence)(uintptr_t)w; }
static inline struct osito_semaphore *sem_from(VkSemaphore h) { return (struct osito_semaphore *)(uintptr_t)h; }
static inline VkSemaphore             sem_to(struct osito_semaphore *w) { return (VkSemaphore)(uintptr_t)w; }
static inline struct osito_swapchain *swp_from(VkSwapchainKHR h) { return (struct osito_swapchain *)(uintptr_t)h; }
static inline VkSwapchainKHR          swp_to(struct osito_swapchain *w) { return (VkSwapchainKHR)(uintptr_t)w; }

/* --- Surface --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateOsitokCompositorSurfaceKHR(
        VkInstance instance,
        const VkOsitoCompositorSurfaceCreateInfoOSITOK *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface) {
    if (!instance || !pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_instance *self = osito_instance_from(instance);
    if (self->icd_instance_count == 0) return VK_ERROR_INITIALIZATION_FAILED;
    /* First ICD wins — surface is OsitoK-private so there's exactly one
     * legitimate provider (venus). */
    struct osito_icd_inst *ci = &self->icd_instances[0];
    PFN_vkCreateOsitokCompositorSurfaceKHR fn =
        (PFN_vkCreateOsitokCompositorSurfaceKHR)ci->icd->get_proc_addr(
            ci->handle, "vkCreateOsitokCompositorSurfaceKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceKHR real = VK_NULL_HANDLE;
    VkResult rc = fn(ci->handle, pCreateInfo, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_surface *w = malloc(sizeof(*w));
    if (!w) {
        PFN_vkDestroySurfaceKHR drop = (PFN_vkDestroySurfaceKHR)
            ci->icd->get_proc_addr(ci->handle, "vkDestroySurfaceKHR");
        if (drop) drop(ci->handle, real, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(w, 0, sizeof(*w));
    w->owner_inst = self;
    w->owner_icd  = ci;
    w->real       = real;
    *pSurface = surf_to(w);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks *pAllocator) {
    if (!instance || !surface) return;
    struct osito_surface *w = surf_from(surface);
    if (w->owner_icd) {
        PFN_vkDestroySurfaceKHR fn = (PFN_vkDestroySurfaceKHR)
            w->owner_icd->icd->get_proc_addr(w->owner_icd->handle,
                                             "vkDestroySurfaceKHR");
        if (fn) fn(w->owner_icd->handle, w->real, pAllocator);
    }
    free(w);
}

/* --- Queue (dispatchable — wraps each GetDeviceQueue result). --- */

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue_W3b5(VkDevice device, uint32_t queueFamilyIndex,
                      uint32_t queueIndex, VkQueue *pQueue) {
    /* vkGetDeviceQueue trampoline lives in the W3a section; this is a
     * W3b.5-specific helper for wrapping the real queue. Not called
     * directly from the dispatch table — the W3a vkGetDeviceQueue now
     * must wrap the returned VkQueue. See the updated vkGetDeviceQueue
     * at the top of this file. */
    (void)device; (void)queueFamilyIndex; (void)queueIndex; (void)pQueue;
}

/* vkGetDeviceQueue is already defined above in the W3a section. We
 * need to wrap its result now. The W3a body writes *pQueue directly
 * — patch path: W3b.5 loader-side trampoline re-wraps the returned
 * VkQueue in an osito_queue struct so subsequent QueueSubmit /
 * QueuePresent trampolines can unwrap the owning device. */

static inline VkQueue unwrap_queue(VkQueue q) {
    if (!q) return q;
    struct osito_queue *w = (struct osito_queue *)q;
    return w->real;
}

static inline struct osito_device *queue_owner(VkQueue q) {
    if (!q) return 0;
    struct osito_queue *w = (struct osito_queue *)q;
    return w->owner;
}

/* --- Queue submit + wait-idle. --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, uint32_t submitCount,
              const VkSubmitInfo *pSubmits, VkFence fence) {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = queue_owner(queue);
    if (!dw || !dw->owner) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_icd_inst *ci = dw->owner;
    PFN_vkQueueSubmit fn = (PFN_vkQueueSubmit)
        ci->icd->get_proc_addr(ci->handle, "vkQueueSubmit");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    /* Unwrap the VkSubmitInfo pointers: wait/signal semaphores +
     * command buffers. Allocate scratch on the stack (W3b.5 caps
     * semaphore counts to VENUS_MAX_SEMA_OBJECTS, cmd buffer count
     * to VENUS_MAX_CMD_BUFFER_OBJECTS; enforce 8 per submit for
     * scratch). */
    #define OSITO_QS_MAX_SUBMITS      4u
    #define OSITO_QS_MAX_PER_SUBMIT   8u
    if (submitCount > OSITO_QS_MAX_SUBMITS) return VK_ERROR_INITIALIZATION_FAILED;
    VkSubmitInfo locals[OSITO_QS_MAX_SUBMITS];
    VkSemaphore  waits[OSITO_QS_MAX_SUBMITS][OSITO_QS_MAX_PER_SUBMIT];
    VkSemaphore  sigs [OSITO_QS_MAX_SUBMITS][OSITO_QS_MAX_PER_SUBMIT];
    VkCommandBuffer cbs[OSITO_QS_MAX_SUBMITS][OSITO_QS_MAX_PER_SUBMIT];
    for (uint32_t i = 0; i < submitCount; i++) {
        locals[i] = pSubmits[i];
        uint32_t nw = locals[i].waitSemaphoreCount;
        uint32_t ns = locals[i].signalSemaphoreCount;
        uint32_t nc = locals[i].commandBufferCount;
        if (nw > OSITO_QS_MAX_PER_SUBMIT) nw = OSITO_QS_MAX_PER_SUBMIT;
        if (ns > OSITO_QS_MAX_PER_SUBMIT) ns = OSITO_QS_MAX_PER_SUBMIT;
        if (nc > OSITO_QS_MAX_PER_SUBMIT) nc = OSITO_QS_MAX_PER_SUBMIT;
        for (uint32_t j = 0; j < nw; j++)
            waits[i][j] = pSubmits[i].pWaitSemaphores[j]
                ? sem_from(pSubmits[i].pWaitSemaphores[j])->real : 0;
        for (uint32_t j = 0; j < ns; j++)
            sigs[i][j] = pSubmits[i].pSignalSemaphores[j]
                ? sem_from(pSubmits[i].pSignalSemaphores[j])->real : 0;
        for (uint32_t j = 0; j < nc; j++) {
            struct osito_cmd_buffer *cbw =
                (struct osito_cmd_buffer *)pSubmits[i].pCommandBuffers[j];
            cbs[i][j] = cbw ? cbw->real : 0;
        }
        locals[i].waitSemaphoreCount   = nw;
        locals[i].signalSemaphoreCount = ns;
        locals[i].commandBufferCount   = nc;
        locals[i].pWaitSemaphores      = waits[i];
        locals[i].pSignalSemaphores    = sigs [i];
        locals[i].pCommandBuffers      = cbs  [i];
    }
    VkFence real_fence = fence ? fence_from(fence)->real : VK_NULL_HANDLE;
    return fn(unwrap_queue(queue), submitCount, locals, real_fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle(VkQueue queue) {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = queue_owner(queue);
    if (!dw || !dw->owner) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_icd_inst *ci = dw->owner;
    PFN_vkQueueWaitIdle fn = (PFN_vkQueueWaitIdle)
        ci->icd->get_proc_addr(ci->handle, "vkQueueWaitIdle");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(unwrap_queue(queue));
}

VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(VkDevice device) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkDeviceWaitIdle fn = (PFN_vkDeviceWaitIdle)
        ci->icd->get_proc_addr(ci->handle, "vkDeviceWaitIdle");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(dw->real);
}

/* --- Fence --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCI,
              const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    if (!device || !pCI || !pFence) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateFence fn = (PFN_vkCreateFence)
        ci->icd->get_proc_addr(ci->handle, "vkCreateFence");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkFence real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_fence *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pFence = fence_to(w);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(VkDevice device, VkFence fence,
               const VkAllocationCallbacks *pAllocator) {
    if (!device || !fence) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_fence *w = fence_from(fence);
    if (ci) {
        PFN_vkDestroyFence fn = (PFN_vkDestroyFence)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyFence");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(VkDevice device, uint32_t count, const VkFence *pFences) {
    if (!device || !pFences || count == 0) return VK_SUCCESS;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkResetFences fn = (PFN_vkResetFences)
        ci->icd->get_proc_addr(ci->handle, "vkResetFences");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    #define OSITO_RF_MAX 32u
    VkFence reals[OSITO_RF_MAX];
    uint32_t n = count < OSITO_RF_MAX ? count : OSITO_RF_MAX;
    for (uint32_t i = 0; i < n; i++)
        reals[i] = pFences[i] ? fence_from(pFences[i])->real : 0;
    return fn(dw->real, n, reals);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(VkDevice device, uint32_t count, const VkFence *pFences,
                VkBool32 waitAll, uint64_t timeout) {
    if (!device || !pFences || count == 0) return VK_SUCCESS;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkWaitForFences fn = (PFN_vkWaitForFences)
        ci->icd->get_proc_addr(ci->handle, "vkWaitForFences");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    #define OSITO_WF_MAX 32u
    VkFence reals[OSITO_WF_MAX];
    uint32_t n = count < OSITO_WF_MAX ? count : OSITO_WF_MAX;
    for (uint32_t i = 0; i < n; i++)
        reals[i] = pFences[i] ? fence_from(pFences[i])->real : 0;
    return fn(dw->real, n, reals, waitAll, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetFenceStatus(VkDevice device, VkFence fence) {
    if (!device || !fence) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_fence *w = fence_from(fence);
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetFenceStatus fn = (PFN_vkGetFenceStatus)
        ci->icd->get_proc_addr(ci->handle, "vkGetFenceStatus");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return fn(dw->real, w->real);
}

/* --- Semaphore --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCI,
                  const VkAllocationCallbacks *pAllocator,
                  VkSemaphore *pSem) {
    if (!device || !pCI || !pSem) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateSemaphore fn = (PFN_vkCreateSemaphore)
        ci->icd->get_proc_addr(ci->handle, "vkCreateSemaphore");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSemaphore real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, pCI, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_semaphore *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pSem = sem_to(w);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySemaphore(VkDevice device, VkSemaphore sem,
                   const VkAllocationCallbacks *pAllocator) {
    if (!device || !sem) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_semaphore *w = sem_from(sem);
    if (ci) {
        PFN_vkDestroySemaphore fn = (PFN_vkDestroySemaphore)
            ci->icd->get_proc_addr(ci->handle, "vkDestroySemaphore");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

/* --- Swapchain --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device,
                     const VkSwapchainCreateInfoKHR *pCI,
                     const VkAllocationCallbacks *pAllocator,
                     VkSwapchainKHR *pSwapchain) {
    if (!device || !pCI || !pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateSwapchainKHR fn = (PFN_vkCreateSwapchainKHR)
        ci->icd->get_proc_addr(ci->handle, "vkCreateSwapchainKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR tmp = *pCI;
    if (tmp.surface) tmp.surface = surf_from(tmp.surface)->real;
    if (tmp.oldSwapchain) tmp.oldSwapchain = swp_from(tmp.oldSwapchain)->real;
    VkSwapchainKHR real = VK_NULL_HANDLE;
    VkResult rc = fn(dw->real, &tmp, pAllocator, &real);
    if (rc != VK_SUCCESS || !real) return rc;
    struct osito_swapchain *w = malloc(sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->owner = dw; w->real = real;
    *pSwapchain = swp_to(w);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks *pAllocator) {
    if (!device || !swapchain) return;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_swapchain *w = swp_from(swapchain);
    if (ci) {
        PFN_vkDestroySwapchainKHR fn = (PFN_vkDestroySwapchainKHR)
            ci->icd->get_proc_addr(ci->handle, "vkDestroySwapchainKHR");
        if (fn) fn(dw->real, w->real, pAllocator);
    }
    free(w);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                        uint32_t *pCount, VkImage *pImages) {
    if (!device || !swapchain || !pCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_swapchain *w = swp_from(swapchain);
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetSwapchainImagesKHR fn = (PFN_vkGetSwapchainImagesKHR)
        ci->icd->get_proc_addr(ci->handle, "vkGetSwapchainImagesKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    /* First call (pImages==NULL) just returns the count. */
    if (!pImages) {
        return fn(dw->real, w->real, pCount, NULL);
    }
    /* Second call: collect real VkImages from the ICD, then wrap each
     * in an osito_image {owner, real} so subsequent vkBindImageMemory
     * / vkDestroyImageView trampolines can unwrap correctly. */
    #define OSITO_SW_MAX 8u
    VkImage reals[OSITO_SW_MAX] = {0};
    uint32_t n = (*pCount < OSITO_SW_MAX) ? *pCount : OSITO_SW_MAX;
    uint32_t req = n;
    VkResult rc = fn(dw->real, w->real, &req, reals);
    if (rc != VK_SUCCESS && rc != VK_INCOMPLETE) return rc;
    for (uint32_t i = 0; i < req; i++) {
        struct osito_image *iw = malloc(sizeof(*iw));
        if (!iw) {
            /* Roll back previous wrappers. */
            for (uint32_t j = 0; j < i; j++) free((void *)pImages[j]);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        iw->owner = dw; iw->real = reals[i];
        pImages[i] = (VkImage)(uintptr_t)iw;
    }
    *pCount = req;
    return rc;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                      uint64_t timeout, VkSemaphore sem, VkFence fence,
                      uint32_t *pImageIndex) {
    if (!device || !swapchain || !pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = osito_device_from(device);
    struct osito_icd_inst *ci = dw->owner;
    struct osito_swapchain *w = swp_from(swapchain);
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkAcquireNextImageKHR fn = (PFN_vkAcquireNextImageKHR)
        ci->icd->get_proc_addr(ci->handle, "vkAcquireNextImageKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSemaphore real_sem   = sem   ? sem_from(sem)->real   : VK_NULL_HANDLE;
    VkFence     real_fence = fence ? fence_from(fence)->real : VK_NULL_HANDLE;
    return fn(dw->real, w->real, timeout, real_sem, real_fence, pImageIndex);
}

/* --- Present --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    if (!queue || !pPresentInfo) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *dw = queue_owner(queue);
    if (!dw || !dw->owner) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_icd_inst *ci = dw->owner;
    PFN_vkQueuePresentKHR fn = (PFN_vkQueuePresentKHR)
        ci->icd->get_proc_addr(ci->handle, "vkQueuePresentKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    #define OSITO_PR_MAX 4u
    VkSemaphore    waits [OSITO_PR_MAX];
    VkSwapchainKHR sws   [OSITO_PR_MAX];
    uint32_t nw = pPresentInfo->waitSemaphoreCount;
    uint32_t nsc = pPresentInfo->swapchainCount;
    if (nw  > OSITO_PR_MAX) nw  = OSITO_PR_MAX;
    if (nsc > OSITO_PR_MAX) nsc = OSITO_PR_MAX;
    for (uint32_t i = 0; i < nw; i++)
        waits[i] = pPresentInfo->pWaitSemaphores[i]
            ? sem_from(pPresentInfo->pWaitSemaphores[i])->real : 0;
    for (uint32_t i = 0; i < nsc; i++)
        sws[i] = pPresentInfo->pSwapchains[i]
            ? swp_from(pPresentInfo->pSwapchains[i])->real : 0;
    VkPresentInfoKHR tmp = *pPresentInfo;
    tmp.waitSemaphoreCount = nw;
    tmp.pWaitSemaphores    = waits;
    tmp.swapchainCount     = nsc;
    tmp.pSwapchains        = sws;
    return fn(unwrap_queue(queue), &tmp);
}

/* --- Physical-device surface queries. --- */

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR *pCaps) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real))
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fn =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceKHR real_s = surface ? surf_from(surface)->real : 0;
    return fn(real, real_s, pCaps);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
        uint32_t *pCount, VkSurfaceFormatKHR *pFormats) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real))
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceKHR real_s = surface ? surf_from(surface)->real : 0;
    return fn(real, real_s, pCount, pFormats);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
        uint32_t *pCount, VkPresentModeKHR *pModes) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real))
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceKHR real_s = surface ? surf_from(surface)->real : 0;
    return fn(real, real_s, pCount, pModes);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceSupportKHR(
        VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
        VkSurfaceKHR surface, VkBool32 *pSupported) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real))
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceKHR real_s = surface ? surf_from(surface)->real : 0;
    return fn(real, queueFamilyIndex, real_s, pSupported);
}

/* ---------------- W4.8 — phys-dev "2" trampolines + format queries ---
 *
 * The "2" variants accept a chained pNext list. We forward the base
 * struct to the W3b.2 trampoline and leave the pNext chain alone — the
 * spec allows callees to ignore unknown sType blocks. Where Mesa expects
 * specific sType blocks (e.g. VkPhysicalDeviceVulkan11Properties), we
 * could fill them in a follow-up; for the clear-only path the W3b.2
 * fallback values are sufficient. */

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    /* The spec mandates sType be VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2.
     * We do not enforce — Mesa always fills it correctly. */
    vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
    /* pNext chain: leave unmodified. Callers that requested specific
     * sType blocks see zero-initialized memory (typical Vulkan pattern
     * — caller is expected to memset before calling). */
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                             VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceMemoryProperties2 *pMem) {
    if (!pMem) return;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMem->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                          uint32_t *pCount,
                                          VkQueueFamilyProperties2 *pFamilies) {
    if (!pCount) return;
    /* Two-call pattern: first NULL, then real. */
    if (!pFamilies) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, NULL);
        return;
    }
    /* Forward into W3b.2 fallback then copy each into the .queueFamilyProperties
     * sub-struct of VkQueueFamilyProperties2. Cap at 4 to avoid any stack
     * bloat — the venus_instance fallback only ever returns 1. */
    #define OSITO_QFP2_MAX 4u
    VkQueueFamilyProperties tmp[OSITO_QFP2_MAX];
    uint32_t n = (*pCount < OSITO_QFP2_MAX) ? *pCount : OSITO_QFP2_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, tmp);
    for (uint32_t i = 0; i < n; i++)
        pFamilies[i].queueFamilyProperties = tmp[i];
    *pCount = n;
}

/* W4.8: report COLOR_ATTACHMENT|TRANSFER_DST|SAMPLED_IMAGE for the four
 * BGRA8/RGBA8 variants Mesa+Zink uses for swapchain back-buffers and
 * texture sampling. Everything else is rejected so Zink doesn't try to
 * use a format the underlying SHM compositor cannot present. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                    VkFormat format,
                                    VkFormatProperties *pFormatProperties) {
    (void)physicalDevice;
    if (!pFormatProperties) return;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB: {
        VkFormatFeatureFlags f =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        pFormatProperties->linearTilingFeatures  = f;
        pFormatProperties->optimalTilingFeatures = f;
        pFormatProperties->bufferFeatures        = 0;
        break;
    }
    default:
        /* All zeros — Mesa interprets as "format not supported". */
        break;
    }
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                     VkFormat format,
                                     VkFormatProperties2 *pFormatProperties) {
    if (!pFormatProperties) return;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format,
                                        &pFormatProperties->formatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice physicalDevice, VkFormat format,
        VkImageType type, VkImageTiling tiling,
        VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties *pImageFormatProperties) {
    (void)physicalDevice; (void)tiling; (void)usage; (void)flags;
    if (!pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    /* Only support 2D BGRA8/RGBA8. Mesa probes lots of formats; we say
     * "no" to everything else so it falls back to supported choices. */
    int format_ok = (format == VK_FORMAT_B8G8R8A8_UNORM ||
                     format == VK_FORMAT_R8G8B8A8_UNORM ||
                     format == VK_FORMAT_B8G8R8A8_SRGB  ||
                     format == VK_FORMAT_R8G8B8A8_SRGB);
    int type_ok   = (type == VK_IMAGE_TYPE_2D);
    if (!format_ok || !type_ok)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    pImageFormatProperties->maxExtent.width  = 8192;
    pImageFormatProperties->maxExtent.height = 8192;
    pImageFormatProperties->maxExtent.depth  = 1;
    pImageFormatProperties->maxMipLevels     = 14;   /* log2(8192)+1 */
    pImageFormatProperties->maxArrayLayers   = 256;
    pImageFormatProperties->sampleCounts     = VK_SAMPLE_COUNT_1_BIT;
    pImageFormatProperties->maxResourceSize  = (VkDeviceSize)1 << 31;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2(
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
        VkImageFormatProperties2 *pImageFormatProperties) {
    if (!pImageFormatInfo || !pImageFormatProperties)
        return VK_ERROR_INITIALIZATION_FAILED;
    return vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice,
            pImageFormatInfo->format, pImageFormatInfo->type,
            pImageFormatInfo->tiling, pImageFormatInfo->usage,
            pImageFormatInfo->flags,
            &pImageFormatProperties->imageFormatProperties);
}

/* W4.8: VK_EXT_debug_utils messenger — we don't actually deliver any
 * debug messages, but Mesa often creates one to silence its own
 * "no messenger" complaints. Allocate a sentinel pointer and return
 * success; the destroy is a no-op (we leak the sentinel — only one
 * per process is ever expected). */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkDebugUtilsMessengerEXT *pMessenger) {
    (void)instance; (void)pCreateInfo; (void)pAllocator;
    if (!pMessenger) return VK_ERROR_INITIALIZATION_FAILED;
    /* Use a non-zero sentinel so apps that test "if (messenger != VK_NULL_HANDLE)"
     * see a valid handle. */
    static char osito_dum_sentinel;
    *pMessenger = (VkDebugUtilsMessengerEXT)(uintptr_t)&osito_dum_sentinel;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDebugUtilsMessengerEXT(VkInstance instance,
                                VkDebugUtilsMessengerEXT messenger,
                                const VkAllocationCallbacks *pAllocator) {
    (void)instance; (void)messenger; (void)pAllocator;
    /* No-op — sentinel is static. */
}

/* W4.8 — vkCmdClearColorImage trampoline. The venus ICD (W4.8 follow-up
 * in venus_w3b6_objects.c) records the clear color + image slot on the
 * cmd buffer; QueueSubmit later fills the SHM buffer. This loader
 * trampoline just forwards into the ICD with unwrapped image. */
typedef void (VKAPI_PTR *PFN_vkCmdClearColorImage)(
    VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *,
    uint32_t, const VkImageSubresourceRange *);

VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(VkCommandBuffer cb, VkImage image,
                     VkImageLayout imageLayout,
                     const VkClearColorValue *pColor, uint32_t rangeCount,
                     const VkImageSubresourceRange *pRanges) {
    if (!cb || !image || !pColor) return;
    struct osito_cmd_buffer *w = (struct osito_cmd_buffer *)cb;
    struct osito_icd_inst *ci = w->owner ? w->owner->owner : 0;
    if (!ci) return;
    PFN_vkCmdClearColorImage fn = (PFN_vkCmdClearColorImage)
        ci->icd->get_proc_addr(ci->handle, "vkCmdClearColorImage");
    if (!fn) return;
    VkImage real = img_from(image)->real;
    fn(w->real, real, imageLayout, pColor, rangeCount, pRanges);
}
