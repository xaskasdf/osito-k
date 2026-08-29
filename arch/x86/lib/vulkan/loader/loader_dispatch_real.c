#include "loader.h"
#include <vulkan/vulkan_ositok.h>

extern int strcmp(const char *, const char *);
extern int printf(const char *, ...);

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t *api_version)
{
    if (!api_version)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (unsigned i = 0; i < osito_icd_count; i++) {
        PFN_vkEnumerateInstanceVersion enumerate =
            (PFN_vkEnumerateInstanceVersion)osito_icd_table[i].get_proc_addr(
                VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
        if (!enumerate)
            continue;
        VkResult result = enumerate(api_version);
        if (result == VK_SUCCESS)
            return result;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *layer_name,
                                       uint32_t *property_count,
                                       VkExtensionProperties *properties)
{
    if (!property_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (layer_name)
        return VK_ERROR_LAYER_NOT_PRESENT;
    static const VkExtensionProperties extensions[] = {
        { VK_KHR_SURFACE_EXTENSION_NAME, 25 },
        { VK_OSITOK_COMPOSITOR_SURFACE_EXTENSION_NAME,
          VK_OSITOK_COMPOSITOR_SURFACE_SPEC_VERSION },
    };
    uint32_t total = sizeof(extensions) / sizeof(extensions[0]);
    if (!properties) {
        *property_count = total;
        return VK_SUCCESS;
    }
    uint32_t written = *property_count < total ? *property_count : total;
    memcpy(properties, extensions, sizeof(extensions[0]) * written);
    *property_count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!device || !name)
        return 0;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkDeviceWaitIdle;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorSetLayout;
    if (strcmp(name, "vkDestroyDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorSetLayout;
    if (strcmp(name, "vkCreateDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplate;
    if (strcmp(name, "vkDestroyDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorUpdateTemplate;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkCreatePipelineLayout;
    if (strcmp(name, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipelineLayout;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)vkCreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)vkDestroySemaphore;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)vkCreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)vkDestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)vkResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)vkWaitForFences;
    if (strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)vkCreateCommandPool;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyCommandPool;
    if (strcmp(name, "vkResetCommandPool") == 0)
        return (PFN_vkVoidFunction)vkResetCommandPool;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkFreeCommandBuffers;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkBeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkEndCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkResetCommandBuffer;
    if (strcmp(name, "vkQueueSubmit2") == 0 ||
        strcmp(name, "vkQueueSubmit2KHR") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit2;
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)vkCreateBuffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)vkDestroyBuffer;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements;
    if (strcmp(name, "vkGetBufferMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2;
    if (strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)vkBindBufferMemory;
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)vkAllocateMemory;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)vkFreeMemory;
    if (strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)vkMapMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)vkUnmapMemory;
    if (strcmp(name, "vkCmdCopyBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBuffer2;
    if (strcmp(name, "vkCmdPipelineBarrier2") == 0 ||
        strcmp(name, "vkCmdPipelineBarrier2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPipelineBarrier2;
    if (strcmp(name, "vkCmdCopyBufferToImage2") == 0 ||
        strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBufferToImage2;
    if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyImageToBuffer2;
    if (strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearColorImage;
    if (strcmp(name, "vkCmdClearDepthStencilImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearDepthStencilImage;
    if (strcmp(name, "vkCmdBeginRendering") == 0 ||
        strcmp(name, "vkCmdBeginRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBeginRendering;
    if (strcmp(name, "vkCmdEndRendering") == 0 ||
        strcmp(name, "vkCmdEndRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdEndRendering;
    if (strcmp(name, "vkCmdBindVertexBuffers2") == 0 ||
        strcmp(name, "vkCmdBindVertexBuffers2EXT") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers2;
    if (strcmp(name, "vkCmdBindIndexBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer;
    if (strcmp(name, "vkCmdBindIndexBuffer2") == 0 ||
        strcmp(name, "vkCmdBindIndexBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer2KHR;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)vkCmdBindPipeline;
    if (strcmp(name, "vkCmdSetCullMode") == 0 ||
        strcmp(name, "vkCmdSetCullModeEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetCullMode;
    if (strcmp(name, "vkCmdSetFrontFace") == 0 ||
        strcmp(name, "vkCmdSetFrontFaceEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetFrontFace;
    if (strcmp(name, "vkCmdSetPrimitiveTopology") == 0 ||
        strcmp(name, "vkCmdSetPrimitiveTopologyEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetPrimitiveTopology;
    if (strcmp(name, "vkCmdSetBlendConstants") == 0)
        return (PFN_vkVoidFunction)vkCmdSetBlendConstants;
    if (strcmp(name, "vkCmdSetViewportWithCount") == 0 ||
        strcmp(name, "vkCmdSetViewportWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewportWithCount;
    if (strcmp(name, "vkCmdSetScissorWithCount") == 0 ||
        strcmp(name, "vkCmdSetScissorWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissorWithCount;
    if (strcmp(name, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)vkCmdDraw;
    if (strcmp(name, "vkCmdDrawIndexed") == 0)
        return (PFN_vkVoidFunction)vkCmdDrawIndexed;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkCmdBindDescriptorSets;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)vkCreateImage;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)vkDestroyImage;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements;
    if (strcmp(name, "vkGetImageMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements2;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)vkBindImageMemory;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)vkCreateImageView;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)vkDestroyImageView;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)vkCreateSampler;
    if (strcmp(name, "vkDestroySampler") == 0)
        return (PFN_vkVoidFunction)vkDestroySampler;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)vkCreateShaderModule;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)vkDestroyShaderModule;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateGraphicsPipelines;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipeline;
    if (strcmp(name, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorPool;
    if (strcmp(name, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorPool;
    if (strcmp(name, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkResetDescriptorPool;
    if (strcmp(name, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkAllocateDescriptorSets;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkUpdateDescriptorSets;
    return 0;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physical_device,
                              VkPhysicalDeviceProperties *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceProperties");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t *property_count,
    VkQueueFamilyProperties *properties)
{
    if (!physical_device || !property_count)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceQueueFamilyProperties");
    if (get_properties)
        get_properties(self->real, property_count, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical_device,
                            VkPhysicalDeviceFeatures *features)
{
    if (!physical_device || !features)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFeatures get_features =
        (PFN_vkGetPhysicalDeviceFeatures)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceFeatures");
    if (get_features)
        get_features(self->real, features);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceMemoryProperties get_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceMemoryProperties");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical_device,
                               VkPhysicalDeviceProperties2 *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceProperties2)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceProperties2");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical_device,
                             VkPhysicalDeviceFeatures2 *features)
{
    if (!physical_device || !features)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFeatures2 get_features =
        (PFN_vkGetPhysicalDeviceFeatures2)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetPhysicalDeviceFeatures2");
    if (get_features)
        get_features(self->real, features);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties2 *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceMemoryProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceMemoryProperties2");
    if (get_properties)
        get_properties(self->real, properties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties2 *properties)
{
    if (!physical_device || !properties)
        return;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceFormatProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceFormatProperties2");
    if (get_properties)
        get_properties(self->real, format, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *properties)
{
    if (!physical_device || !info || !properties)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_properties =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkGetPhysicalDeviceImageFormatProperties2");
    if (!get_properties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return get_properties(self->real, info, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char *layer_name,
    uint32_t *property_count, VkExtensionProperties *properties)
{
    if (!physical_device || !property_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *self = osito_phys_from(physical_device);
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
            self->owner->icd->get_proc_addr(
                self->owner->handle,
                "vkEnumerateDeviceExtensionProperties");
    if (!enumerate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return enumerate(self->real, layer_name, property_count, properties);
}

typedef VkResult (VKAPI_PTR *PFN_ositoCreateSurface)(
    VkInstance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *,
    const VkAllocationCallbacks *, VkSurfaceKHR *);

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateOsitokCompositorSurfaceKHR(
    VkInstance instance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    if (!instance || !create_info || !surface)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_instance *self = osito_instance_from(instance);
    if (!self->icd_instance_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_icd_inst *owner = &self->icd_instances[0];
    PFN_ositoCreateSurface create =
        (PFN_ositoCreateSurface)owner->icd->get_proc_addr(
            owner->handle, "vkCreateOsitokCompositorSurfaceKHR");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfaceKHR real = VK_NULL_HANDLE;
    VkResult result = create(owner->handle, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_surface *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroySurfaceKHR destroy =
            (PFN_vkDestroySurfaceKHR)owner->icd->get_proc_addr(
                owner->handle, "vkDestroySurfaceKHR");
        if (destroy)
            destroy(owner->handle, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(wrapper, 0, sizeof(*wrapper));
    wrapper->owner_inst = self;
    wrapper->owner_icd = owner;
    wrapper->real = real;
    *surface = (VkSurfaceKHR)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks *allocator)
{
    if (!instance || !surface)
        return;
    struct osito_surface *wrapper =
        (struct osito_surface *)(uintptr_t)surface;
    PFN_vkDestroySurfaceKHR destroy =
        (PFN_vkDestroySurfaceKHR)wrapper->owner_icd->icd->get_proc_addr(
            wrapper->owner_icd->handle, "vkDestroySurfaceKHR");
    if (destroy)
        destroy(wrapper->owner_icd->handle, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physical_device,
               const VkDeviceCreateInfo *create_info,
               const VkAllocationCallbacks *allocator,
               VkDevice *device)
{
    if (!physical_device || !create_info || !device)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *physical = osito_phys_from(physical_device);
    PFN_vkCreateDevice create =
        (PFN_vkCreateDevice)physical->owner->icd->get_proc_addr(
            physical->owner->handle, "vkCreateDevice");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice real = VK_NULL_HANDLE;
    VkResult result = create(physical->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;

    struct osito_device *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyDevice destroy =
            (PFN_vkDestroyDevice)physical->owner->icd->get_proc_addr(
                physical->owner->handle, "vkDestroyDevice");
        if (destroy)
            destroy(real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(wrapper, 0, sizeof(*wrapper));
    set_loader_magic_value(wrapper);
    wrapper->owner = physical->owner;
    wrapper->real = real;
    *device = osito_device_to(wrapper);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    if (!device)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_queue *queue = self->queues;
    while (queue) {
        struct osito_queue *next = queue->next;
        free(queue);
        queue = next;
    }
    PFN_vkDestroyDevice destroy =
        (PFN_vkDestroyDevice)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyDevice");
    if (destroy)
        destroy(self->real, allocator);
    free(self);
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queue_family_index,
                 uint32_t queue_index, VkQueue *queue)
{
    if (!device || !queue)
        return;
    *queue = VK_NULL_HANDLE;
    struct osito_device *self = osito_device_from(device);
    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkGetDeviceQueue");
    if (!get_queue)
        return;
    VkQueue real = VK_NULL_HANDLE;
    get_queue(self->real, queue_family_index, queue_index, &real);
    if (!real)
        return;
    for (struct osito_queue *existing = self->queues; existing;
         existing = existing->next) {
        if (existing->real == real) {
            *queue = (VkQueue)existing;
            return;
        }
    }
    struct osito_queue *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper)
        return;
    memset(wrapper, 0, sizeof(*wrapper));
    set_loader_magic_value(wrapper);
    wrapper->owner = self;
    wrapper->real = real;
    wrapper->next = self->queues;
    self->queues = wrapper;
    *queue = (VkQueue)wrapper;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(VkDevice device)
{
    if (!device)
        return VK_ERROR_DEVICE_LOST;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDeviceWaitIdle wait_idle =
        (PFN_vkDeviceWaitIdle)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDeviceWaitIdle");
    return wait_idle ? wait_idle(self->real) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *set_layout)
{
    if (!device || !create_info || !set_layout)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateDescriptorSetLayout create =
        (PFN_vkCreateDescriptorSetLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateDescriptorSetLayout");
    return create ? create(self->real, create_info, allocator, set_layout)
                  : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout set_layout,
    const VkAllocationCallbacks *allocator)
{
    if (!device || !set_layout)
        return;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDestroyDescriptorSetLayout destroy =
        (PFN_vkDestroyDescriptorSetLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyDescriptorSetLayout");
    if (destroy)
        destroy(self->real, set_layout, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorUpdateTemplate *update_template)
{
    if (!device || !create_info || !update_template)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateDescriptorUpdateTemplate create =
        (PFN_vkCreateDescriptorUpdateTemplate)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkCreateDescriptorUpdateTemplate");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDescriptorUpdateTemplateCreateInfo real_info = *create_info;
    if (create_info->pipelineLayout) {
        struct osito_pipeline_layout *layout =
            (struct osito_pipeline_layout *)(uintptr_t)
                create_info->pipelineLayout;
        if (layout->owner != self)
            return VK_ERROR_INITIALIZATION_FAILED;
        real_info.pipelineLayout = layout->real;
    }
    return create(self->real, &real_info, allocator, update_template);
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate update_template,
    const VkAllocationCallbacks *allocator)
{
    if (!device || !update_template)
        return;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDestroyDescriptorUpdateTemplate destroy =
        (PFN_vkDestroyDescriptorUpdateTemplate)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyDescriptorUpdateTemplate");
    if (destroy)
        destroy(self->real, update_template, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkPipelineLayout *pipeline_layout)
{
    if (!device || !create_info || !pipeline_layout)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreatePipelineLayout create =
        (PFN_vkCreatePipelineLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreatePipelineLayout");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkPipelineLayout real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_pipeline_layout *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyPipelineLayout destroy =
            (PFN_vkDestroyPipelineLayout)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyPipelineLayout");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *pipeline_layout = (VkPipelineLayout)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipeline_layout,
    const VkAllocationCallbacks *allocator)
{
    if (!device || !pipeline_layout)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_pipeline_layout *wrapper =
        (struct osito_pipeline_layout *)(uintptr_t)pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy =
        (PFN_vkDestroyPipelineLayout)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyPipelineLayout");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSemaphore(VkDevice device,
                  const VkSemaphoreCreateInfo *create_info,
                  const VkAllocationCallbacks *allocator,
                  VkSemaphore *semaphore)
{
    if (!device || !create_info || !semaphore)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateSemaphore create =
        (PFN_vkCreateSemaphore)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateSemaphore");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSemaphore real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_semaphore *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroySemaphore destroy =
            (PFN_vkDestroySemaphore)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroySemaphore");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *semaphore = (VkSemaphore)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                   const VkAllocationCallbacks *allocator)
{
    if (!device || !semaphore)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_semaphore *wrapper =
        (struct osito_semaphore *)(uintptr_t)semaphore;
    PFN_vkDestroySemaphore destroy =
        (PFN_vkDestroySemaphore)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroySemaphore");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice device, const VkFenceCreateInfo *create_info,
              const VkAllocationCallbacks *allocator, VkFence *fence)
{
    if (!device || !create_info || !fence)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateFence create =
        (PFN_vkCreateFence)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateFence");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkFence real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_fence *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyFence destroy =
            (PFN_vkDestroyFence)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyFence");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *fence = (VkFence)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(VkDevice device, VkFence fence,
               const VkAllocationCallbacks *allocator)
{
    if (!device || !fence)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_fence *wrapper = (struct osito_fence *)(uintptr_t)fence;
    PFN_vkDestroyFence destroy =
        (PFN_vkDestroyFence)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyFence");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(VkDevice device, uint32_t fence_count, const VkFence *fences)
{
    if (!device || !fence_count || !fences)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    VkFence *real = malloc((size_t)fence_count * sizeof(*real));
    if (!real)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < fence_count; i++) {
        struct osito_fence *wrapper =
            (struct osito_fence *)(uintptr_t)fences[i];
        if (!wrapper || wrapper->owner != self) {
            free(real);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real[i] = wrapper->real;
    }
    PFN_vkResetFences reset =
        (PFN_vkResetFences)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkResetFences");
    VkResult result = reset
        ? reset(self->real, fence_count, real)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
    free(real);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(VkDevice device, uint32_t fence_count,
                const VkFence *fences, VkBool32 wait_all, uint64_t timeout)
{
    if (!device || !fence_count || !fences)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    VkFence *real = malloc((size_t)fence_count * sizeof(*real));
    if (!real)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < fence_count; i++) {
        struct osito_fence *wrapper =
            (struct osito_fence *)(uintptr_t)fences[i];
        if (!wrapper || wrapper->owner != self) {
            free(real);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real[i] = wrapper->real;
    }
    PFN_vkWaitForFences wait =
        (PFN_vkWaitForFences)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkWaitForFences");
    VkResult result = wait
        ? wait(self->real, fence_count, real, wait_all, timeout)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
    free(real);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *allocate_info,
                 const VkAllocationCallbacks *allocator,
                 VkDeviceMemory *memory)
{
    if (!device || !allocate_info || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkAllocateMemory allocate =
        (PFN_vkAllocateMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkAllocateMemory");
    if (!allocate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDeviceMemory real = VK_NULL_HANDLE;
    VkResult result = allocate(
        self->real, allocate_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_memory *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkFreeMemory release =
            (PFN_vkFreeMemory)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkFreeMemory");
        if (release)
            release(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *memory = (VkDeviceMemory)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory,
             const VkAllocationCallbacks *allocator)
{
    if (!device || !memory)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_memory *wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    PFN_vkFreeMemory release =
        (PFN_vkFreeMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkFreeMemory");
    if (release)
        release(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
            VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    if (!device || !memory || !data)
        return VK_ERROR_MEMORY_MAP_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_memory *wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (wrapper->owner != self)
        return VK_ERROR_MEMORY_MAP_FAILED;
    PFN_vkMapMemory map =
        (PFN_vkMapMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkMapMemory");
    return map ? map(self->real, wrapper->real, offset, size, flags, data)
               : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    if (!device || !memory)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_memory *wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (wrapper->owner != self)
        return;
    PFN_vkUnmapMemory unmap =
        (PFN_vkUnmapMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkUnmapMemory");
    if (unmap)
        unmap(self->real, wrapper->real);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBuffer2(VkCommandBuffer command_buffer,
                 const VkCopyBufferInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        !copy_info->srcBuffer || !copy_info->dstBuffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *source =
        (struct osito_buffer *)(uintptr_t)copy_info->srcBuffer;
    struct osito_buffer *destination =
        (struct osito_buffer *)(uintptr_t)copy_info->dstBuffer;
    if (source->owner != device || destination->owner != device)
        return;
    VkCopyBufferInfo2 real_info = *copy_info;
    real_info.srcBuffer = source->real;
    real_info.dstBuffer = destination->real;
    PFN_vkCmdCopyBuffer2 copy =
        (PFN_vkCmdCopyBuffer2)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdCopyBuffer2");
    if (copy)
        copy(command_wrapper->real, &real_info);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2(VkCommandBuffer command_buffer,
                      const VkDependencyInfo *dependency_info)
{
    if (!command_buffer || !dependency_info)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkDependencyInfo real_info = *dependency_info;
    VkBufferMemoryBarrier2 *buffer_barriers = 0;
    VkImageMemoryBarrier2 *image_barriers = 0;

    if (dependency_info->bufferMemoryBarrierCount) {
        if (!dependency_info->pBufferMemoryBarriers)
            return;
        buffer_barriers = malloc(
            (size_t)dependency_info->bufferMemoryBarrierCount *
            sizeof(*buffer_barriers));
        if (!buffer_barriers)
            return;
        memcpy(buffer_barriers, dependency_info->pBufferMemoryBarriers,
               (size_t)dependency_info->bufferMemoryBarrierCount *
               sizeof(*buffer_barriers));
        real_info.pBufferMemoryBarriers = buffer_barriers;
        for (uint32_t i = 0; i < dependency_info->bufferMemoryBarrierCount;
             i++) {
            struct osito_buffer *wrapper =
                (struct osito_buffer *)(uintptr_t)buffer_barriers[i].buffer;
            if (!wrapper || wrapper->owner != device) {
                free(buffer_barriers);
                return;
            }
            buffer_barriers[i].buffer = wrapper->real;
        }
    }

    if (dependency_info->imageMemoryBarrierCount) {
        if (!dependency_info->pImageMemoryBarriers) {
            free(buffer_barriers);
            return;
        }
        image_barriers = malloc(
            (size_t)dependency_info->imageMemoryBarrierCount *
            sizeof(*image_barriers));
        if (!image_barriers) {
            free(buffer_barriers);
            return;
        }
        memcpy(image_barriers, dependency_info->pImageMemoryBarriers,
               (size_t)dependency_info->imageMemoryBarrierCount *
               sizeof(*image_barriers));
        real_info.pImageMemoryBarriers = image_barriers;
        for (uint32_t i = 0; i < dependency_info->imageMemoryBarrierCount;
             i++) {
            struct osito_image *wrapper =
                (struct osito_image *)(uintptr_t)image_barriers[i].image;
            if (!wrapper || wrapper->owner != device) {
                free(image_barriers);
                free(buffer_barriers);
                return;
            }
            image_barriers[i].image = wrapper->real;
        }
    }

    PFN_vkCmdPipelineBarrier2 barrier =
        (PFN_vkCmdPipelineBarrier2)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdPipelineBarrier2");
    if (barrier)
        barrier(command_wrapper->real, &real_info);
    free(image_barriers);
    free(buffer_barriers);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBufferToImage2(VkCommandBuffer command_buffer,
                        const VkCopyBufferToImageInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        !copy_info->srcBuffer || !copy_info->dstImage)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *buffer =
        (struct osito_buffer *)(uintptr_t)copy_info->srcBuffer;
    struct osito_image *image =
        (struct osito_image *)(uintptr_t)copy_info->dstImage;
    if (buffer->owner != device || image->owner != device)
        return;
    VkCopyBufferToImageInfo2 real_info = *copy_info;
    real_info.srcBuffer = buffer->real;
    real_info.dstImage = image->real;
    PFN_vkCmdCopyBufferToImage2 copy =
        (PFN_vkCmdCopyBufferToImage2)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdCopyBufferToImage2");
    if (copy)
        copy(command_wrapper->real, &real_info);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer2(VkCommandBuffer command_buffer,
                        const VkCopyImageToBufferInfo2 *copy_info)
{
    if (!command_buffer || !copy_info ||
        !copy_info->srcImage || !copy_info->dstBuffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_image *image =
        (struct osito_image *)(uintptr_t)copy_info->srcImage;
    struct osito_buffer *buffer =
        (struct osito_buffer *)(uintptr_t)copy_info->dstBuffer;
    if (buffer->owner != device || image->owner != device)
        return;
    VkCopyImageToBufferInfo2 real_info = *copy_info;
    real_info.srcImage = image->real;
    real_info.dstBuffer = buffer->real;
    PFN_vkCmdCopyImageToBuffer2 copy =
        (PFN_vkCmdCopyImageToBuffer2)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdCopyImageToBuffer2");
    if (copy)
        copy(command_wrapper->real, &real_info);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(VkCommandBuffer command_buffer, VkImage image,
                     VkImageLayout image_layout,
                     const VkClearColorValue *color,
                     uint32_t range_count,
                     const VkImageSubresourceRange *ranges)
{
    if (!command_buffer || !image || !color || !range_count || !ranges)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_image *image_wrapper =
        (struct osito_image *)(uintptr_t)image;
    if (image_wrapper->owner != device)
        return;
    PFN_vkCmdClearColorImage clear =
        (PFN_vkCmdClearColorImage)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdClearColorImage");
    if (clear)
        clear(command_wrapper->real, image_wrapper->real, image_layout,
              color, range_count, ranges);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdClearDepthStencilImage(
    VkCommandBuffer command_buffer, VkImage image,
    VkImageLayout image_layout,
    const VkClearDepthStencilValue *depth_stencil,
    uint32_t range_count, const VkImageSubresourceRange *ranges)
{
    if (!command_buffer || !image || !depth_stencil ||
        !range_count || !ranges)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_image *image_wrapper =
        (struct osito_image *)(uintptr_t)image;
    if (image_wrapper->owner != device)
        return;
    PFN_vkCmdClearDepthStencilImage clear =
        (PFN_vkCmdClearDepthStencilImage)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdClearDepthStencilImage");
    if (clear)
        clear(command_wrapper->real, image_wrapper->real, image_layout,
              depth_stencil, range_count, ranges);
}

static int unwrap_rendering_attachment(
    struct osito_device *device, VkRenderingAttachmentInfo *destination,
    const VkRenderingAttachmentInfo *source)
{
    *destination = *source;
    if (source->imageView) {
        struct osito_image_view *view =
            (struct osito_image_view *)(uintptr_t)source->imageView;
        if (view->owner != device)
            return -1;
        destination->imageView = view->real;
    }
    if (source->resolveImageView) {
        struct osito_image_view *view =
            (struct osito_image_view *)(uintptr_t)source->resolveImageView;
        if (view->owner != device)
            return -1;
        destination->resolveImageView = view->real;
    }
    return 0;
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRendering(VkCommandBuffer command_buffer,
                    const VkRenderingInfo *rendering_info)
{
    if (!command_buffer || !rendering_info ||
        (rendering_info->colorAttachmentCount &&
         !rendering_info->pColorAttachments))
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkRenderingAttachmentInfo *colors = 0;
    if (rendering_info->colorAttachmentCount) {
        colors = malloc(sizeof(*colors) *
                        rendering_info->colorAttachmentCount);
        if (!colors)
            return;
        for (uint32_t i = 0; i < rendering_info->colorAttachmentCount; i++) {
            if (unwrap_rendering_attachment(
                    device, &colors[i],
                    &rendering_info->pColorAttachments[i]) < 0) {
                free(colors);
                return;
            }
        }
    }
    VkRenderingAttachmentInfo depth;
    VkRenderingAttachmentInfo stencil;
    VkRenderingInfo real_info = *rendering_info;
    real_info.pColorAttachments = colors;
    if (rendering_info->pDepthAttachment) {
        if (unwrap_rendering_attachment(
                device, &depth, rendering_info->pDepthAttachment) < 0) {
            free(colors);
            return;
        }
        real_info.pDepthAttachment = &depth;
    }
    if (rendering_info->pStencilAttachment) {
        if (unwrap_rendering_attachment(
                device, &stencil, rendering_info->pStencilAttachment) < 0) {
            free(colors);
            return;
        }
        real_info.pStencilAttachment = &stencil;
    }
    PFN_vkCmdBeginRendering begin =
        (PFN_vkCmdBeginRendering)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBeginRendering");
    if (begin)
        begin(command_wrapper->real, &real_info);
    free(colors);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdEndRendering(VkCommandBuffer command_buffer)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    PFN_vkCmdEndRendering end =
        (PFN_vkCmdEndRendering)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdEndRendering");
    if (end)
        end(command_wrapper->real);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers2(VkCommandBuffer command_buffer,
                        uint32_t first_binding, uint32_t binding_count,
                        const VkBuffer *buffers,
                        const VkDeviceSize *offsets,
                        const VkDeviceSize *sizes,
                        const VkDeviceSize *strides)
{
    if (!command_buffer || !binding_count || !buffers || !offsets)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    VkBuffer *real_buffers = malloc(sizeof(*real_buffers) * binding_count);
    if (!real_buffers)
        return;
    for (uint32_t i = 0; i < binding_count; i++) {
        if (!buffers[i]) {
            real_buffers[i] = VK_NULL_HANDLE;
            continue;
        }
        struct osito_buffer *buffer =
            (struct osito_buffer *)(uintptr_t)buffers[i];
        if (buffer->owner != device) {
            free(real_buffers);
            return;
        }
        real_buffers[i] = buffer->real;
    }
    PFN_vkCmdBindVertexBuffers2 bind =
        (PFN_vkCmdBindVertexBuffers2)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindVertexBuffers2");
    if (bind)
        bind(command_wrapper->real, first_binding, binding_count,
             real_buffers, offsets, sizes, strides);
    free(real_buffers);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer,
                     VkDeviceSize offset, VkIndexType index_type)
{
    if (!command_buffer || !buffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *buffer_wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    if (buffer_wrapper->owner != device)
        return;
    PFN_vkCmdBindIndexBuffer bind =
        (PFN_vkCmdBindIndexBuffer)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBindIndexBuffer");
    if (bind)
        bind(command_wrapper->real, buffer_wrapper->real, offset, index_type);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer2KHR(VkCommandBuffer command_buffer, VkBuffer buffer,
                         VkDeviceSize offset, VkDeviceSize size,
                         VkIndexType index_type)
{
    if (!command_buffer || !buffer)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_buffer *buffer_wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    if (buffer_wrapper->owner != device)
        return;
    PFN_vkCmdBindIndexBuffer2KHR bind =
        (PFN_vkCmdBindIndexBuffer2KHR)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBindIndexBuffer2KHR");
    if (!bind)
        bind = (PFN_vkCmdBindIndexBuffer2KHR)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindIndexBuffer2");
    if (bind)
        bind(command_wrapper->real, buffer_wrapper->real,
             offset, size, index_type);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer2(VkCommandBuffer command_buffer, VkBuffer buffer,
                      VkDeviceSize offset, VkDeviceSize size,
                      VkIndexType index_type)
{
    vkCmdBindIndexBuffer2KHR(
        command_buffer, buffer, offset, size, index_type);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer command_buffer,
                  VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
    if (!command_buffer || !pipeline)
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_pipeline *pipeline_wrapper =
        (struct osito_pipeline *)(uintptr_t)pipeline;
    if (pipeline_wrapper->owner != device)
        return;
    PFN_vkCmdBindPipeline bind =
        (PFN_vkCmdBindPipeline)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkCmdBindPipeline");
    if (bind)
        bind(command_wrapper->real, bind_point, pipeline_wrapper->real);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetCullMode(VkCommandBuffer command_buffer, VkCullModeFlags cull_mode)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetCullMode set =
        (PFN_vkCmdSetCullMode)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetCullMode");
    if (set)
        set(wrapper->real, cull_mode);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetFrontFace(VkCommandBuffer command_buffer, VkFrontFace front_face)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetFrontFace set =
        (PFN_vkCmdSetFrontFace)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdSetFrontFace");
    if (set)
        set(wrapper->real, front_face);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetPrimitiveTopology(VkCommandBuffer command_buffer,
                          VkPrimitiveTopology topology)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetPrimitiveTopology set =
        (PFN_vkCmdSetPrimitiveTopology)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle,
                "vkCmdSetPrimitiveTopology");
    if (set)
        set(wrapper->real, topology);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetBlendConstants(VkCommandBuffer command_buffer,
                       const float blend_constants[4])
{
    if (!command_buffer || !blend_constants)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetBlendConstants set =
        (PFN_vkCmdSetBlendConstants)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkCmdSetBlendConstants");
    if (set)
        set(wrapper->real, blend_constants);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetViewportWithCount(VkCommandBuffer command_buffer,
                          uint32_t viewport_count,
                          const VkViewport *viewports)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetViewportWithCount set =
        (PFN_vkCmdSetViewportWithCount)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkCmdSetViewportWithCount");
    if (set)
        set(wrapper->real, viewport_count, viewports);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdSetScissorWithCount(VkCommandBuffer command_buffer,
                         uint32_t scissor_count, const VkRect2D *scissors)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdSetScissorWithCount set =
        (PFN_vkCmdSetScissorWithCount)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkCmdSetScissorWithCount");
    if (set)
        set(wrapper->real, scissor_count, scissors);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdDraw(VkCommandBuffer command_buffer, uint32_t vertex_count,
          uint32_t instance_count, uint32_t first_vertex,
          uint32_t first_instance)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdDraw draw =
        (PFN_vkCmdDraw)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdDraw");
    if (draw)
        draw(wrapper->real, vertex_count, instance_count,
             first_vertex, first_instance);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexed(VkCommandBuffer command_buffer,
                 uint32_t index_count, uint32_t instance_count,
                 uint32_t first_index, int32_t vertex_offset,
                 uint32_t first_instance)
{
    if (!command_buffer)
        return;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkCmdDrawIndexed draw =
        (PFN_vkCmdDrawIndexed)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkCmdDrawIndexed");
    if (draw)
        draw(wrapper->real, index_count, instance_count,
             first_index, vertex_offset, first_instance);
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t first_set,
    uint32_t descriptor_set_count, const VkDescriptorSet *descriptor_sets,
    uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets)
{
    if (!command_buffer || !layout ||
        (descriptor_set_count && !descriptor_sets))
        return;
    struct osito_cmd_buffer *command_wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    struct osito_device *device = command_wrapper->owner;
    struct osito_pipeline_layout *layout_wrapper =
        (struct osito_pipeline_layout *)(uintptr_t)layout;
    if (layout_wrapper->owner != device)
        return;
    VkDescriptorSet *real_sets = descriptor_set_count
        ? malloc(sizeof(*real_sets) * descriptor_set_count) : 0;
    if (descriptor_set_count && !real_sets)
        return;
    for (uint32_t i = 0; i < descriptor_set_count; i++) {
        struct osito_descriptor_set *set =
            (struct osito_descriptor_set *)(uintptr_t)descriptor_sets[i];
        if (!set || set->owner != device) {
            free(real_sets);
            return;
        }
        real_sets[i] = set->real;
    }
    PFN_vkCmdBindDescriptorSets bind =
        (PFN_vkCmdBindDescriptorSets)
            device->owner->icd->get_proc_addr(
                device->owner->handle, "vkCmdBindDescriptorSets");
    if (bind)
        bind(command_wrapper->real, bind_point, layout_wrapper->real,
             first_set, descriptor_set_count, real_sets,
             dynamic_offset_count, dynamic_offsets);
    free(real_sets);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo *create_info,
              const VkAllocationCallbacks *allocator, VkImage *image)
{
    if (!device || !create_info || !image)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateImage create =
        (PFN_vkCreateImage)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateImage");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkImage real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_image *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyImage destroy =
            (PFN_vkDestroyImage)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyImage");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *image = (VkImage)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image,
               const VkAllocationCallbacks *allocator)
{
    if (!device || !image)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *wrapper =
        (struct osito_image *)(uintptr_t)image;
    PFN_vkDestroyImage destroy =
        (PFN_vkDestroyImage)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyImage");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                             VkMemoryRequirements *requirements)
{
    if (!device || !image || !requirements)
        return;
    memset(requirements, 0, sizeof(*requirements));
    struct osito_device *self = osito_device_from(device);
    struct osito_image *wrapper =
        (struct osito_image *)(uintptr_t)image;
    PFN_vkGetImageMemoryRequirements get =
        (PFN_vkGetImageMemoryRequirements)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetImageMemoryRequirements");
    if (get)
        get(self->real, wrapper->real, requirements);
}

VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements)
{
    if (!device || !info || !requirements || !info->image)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *wrapper =
        (struct osito_image *)(uintptr_t)info->image;
    VkImageMemoryRequirementsInfo2 real_info = *info;
    real_info.image = wrapper->real;
    PFN_vkGetImageMemoryRequirements2 get =
        (PFN_vkGetImageMemoryRequirements2)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetImageMemoryRequirements2");
    if (get) {
        get(self->real, &real_info, requirements);
        return;
    }
    vkGetImageMemoryRequirements(device, info->image,
                                 &requirements->memoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                  VkDeviceSize memory_offset)
{
    if (!device || !image || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *image_wrapper =
        (struct osito_image *)(uintptr_t)image;
    struct osito_memory *memory_wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (image_wrapper->owner != self || memory_wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBindImageMemory bind =
        (PFN_vkBindImageMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkBindImageMemory");
    return bind ? bind(self->real, image_wrapper->real,
                       memory_wrapper->real, memory_offset)
                : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device,
                  const VkImageViewCreateInfo *create_info,
                  const VkAllocationCallbacks *allocator,
                  VkImageView *view)
{
    if (!device || !create_info || !create_info->image || !view)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_image *image =
        (struct osito_image *)(uintptr_t)create_info->image;
    if (image->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkImageViewCreateInfo real_info = *create_info;
    real_info.image = image->real;
    PFN_vkCreateImageView create =
        (PFN_vkCreateImageView)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateImageView");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkImageView real = VK_NULL_HANDLE;
    VkResult result = create(self->real, &real_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_image_view *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyImageView destroy =
            (PFN_vkDestroyImageView)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyImageView");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *view = (VkImageView)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView view,
                   const VkAllocationCallbacks *allocator)
{
    if (!device || !view)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_image_view *wrapper =
        (struct osito_image_view *)(uintptr_t)view;
    PFN_vkDestroyImageView destroy =
        (PFN_vkDestroyImageView)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyImageView");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *create_info,
                const VkAllocationCallbacks *allocator, VkSampler *sampler)
{
    if (!device || !create_info || !sampler)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateSampler create =
        (PFN_vkCreateSampler)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateSampler");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSampler real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_sampler *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroySampler destroy =
            (PFN_vkDestroySampler)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroySampler");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *sampler = (VkSampler)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySampler(VkDevice device, VkSampler sampler,
                 const VkAllocationCallbacks *allocator)
{
    if (!device || !sampler)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_sampler *wrapper =
        (struct osito_sampler *)(uintptr_t)sampler;
    PFN_vkDestroySampler destroy =
        (PFN_vkDestroySampler)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroySampler");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device,
                     const VkShaderModuleCreateInfo *create_info,
                     const VkAllocationCallbacks *allocator,
                     VkShaderModule *shader_module)
{
    if (!device || !create_info || !shader_module)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateShaderModule create =
        (PFN_vkCreateShaderModule)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateShaderModule");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkShaderModule real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_shader *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyShaderModule destroy =
            (PFN_vkDestroyShaderModule)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyShaderModule");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *shader_module = (VkShaderModule)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice device, VkShaderModule shader_module,
                      const VkAllocationCallbacks *allocator)
{
    if (!device || !shader_module)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_shader *wrapper =
        (struct osito_shader *)(uintptr_t)shader_module;
    PFN_vkDestroyShaderModule destroy =
        (PFN_vkDestroyShaderModule)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyShaderModule");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    if (!device || pipeline_cache || create_info_count != 1 ||
        !create_infos || !pipelines) {
        printf("[VK loader pipeline] invalid args cache=%llu count=%u infos=%u out=%u\n",
               (unsigned long long)pipeline_cache, create_info_count,
               !!create_infos, !!pipelines);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct osito_device *self = osito_device_from(device);
    const VkGraphicsPipelineCreateInfo *source = &create_infos[0];
    if (!source->layout || (source->stageCount && !source->pStages)) {
        printf("[VK loader pipeline] missing layout/stages layout=%llu count=%u stages=%u\n",
               (unsigned long long)source->layout, source->stageCount,
               !!source->pStages);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPipelineShaderStageCreateInfo *stages = 0;
    if (source->stageCount) {
        stages = malloc(sizeof(*stages) * source->stageCount);
        if (!stages)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(stages, source->pStages,
               sizeof(*stages) * source->stageCount);
        for (uint32_t i = 0; i < source->stageCount; i++) {
            if (!stages[i].module)
                continue;
            struct osito_shader *shader =
                (struct osito_shader *)(uintptr_t)stages[i].module;
            if (shader->owner != self) {
                printf("[VK loader pipeline] shader owner mismatch stage=%u\n", i);
                free(stages);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            stages[i].module = shader->real;
        }
    }
    VkGraphicsPipelineCreateInfo real_info = *source;
    real_info.pStages = stages;
    struct osito_pipeline_layout *layout =
        (struct osito_pipeline_layout *)(uintptr_t)source->layout;
    if (layout->owner != self) {
        printf("[VK loader pipeline] layout owner mismatch\n");
        free(stages);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    real_info.layout = layout->real;
    if (source->renderPass) {
        struct osito_render_pass *render_pass =
            (struct osito_render_pass *)(uintptr_t)source->renderPass;
        if (render_pass->owner != self) {
            free(stages);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real_info.renderPass = render_pass->real;
    }
    if (source->basePipelineHandle) {
        struct osito_pipeline *base =
            (struct osito_pipeline *)(uintptr_t)source->basePipelineHandle;
        if (base->owner != self) {
            free(stages);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        real_info.basePipelineHandle = base->real;
    }
    PFN_vkCreateGraphicsPipelines create =
        (PFN_vkCreateGraphicsPipelines)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateGraphicsPipelines");
    if (!create) {
        free(stages);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkPipeline real = VK_NULL_HANDLE;
    VkResult result = create(self->real, VK_NULL_HANDLE, 1,
                             &real_info, allocator, &real);
    free(stages);
    if (result != VK_SUCCESS)
        return result;
    struct osito_pipeline *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyPipeline destroy =
            (PFN_vkDestroyPipeline)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyPipeline");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    pipelines[0] = (VkPipeline)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkComputePipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    if (!device || pipeline_cache || create_info_count != 1 ||
        !create_infos || !pipelines)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    const VkComputePipelineCreateInfo *source = &create_infos[0];
    if (!source->stage.module || !source->layout)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct osito_shader *shader =
        (struct osito_shader *)(uintptr_t)source->stage.module;
    struct osito_pipeline_layout *layout =
        (struct osito_pipeline_layout *)(uintptr_t)source->layout;
    if (shader->owner != self || layout->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkComputePipelineCreateInfo real_info = *source;
    real_info.stage.module = shader->real;
    real_info.layout = layout->real;
    if (source->basePipelineHandle) {
        struct osito_pipeline *base =
            (struct osito_pipeline *)(uintptr_t)source->basePipelineHandle;
        if (base->owner != self)
            return VK_ERROR_INITIALIZATION_FAILED;
        real_info.basePipelineHandle = base->real;
    }

    PFN_vkCreateComputePipelines create =
        (PFN_vkCreateComputePipelines)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateComputePipelines");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkPipeline real = VK_NULL_HANDLE;
    VkResult result = create(self->real, VK_NULL_HANDLE, 1,
                             &real_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;

    struct osito_pipeline *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyPipeline destroy =
            (PFN_vkDestroyPipeline)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyPipeline");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    pipelines[0] = (VkPipeline)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(VkDevice device, VkPipeline pipeline,
                  const VkAllocationCallbacks *allocator)
{
    if (!device || !pipeline)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_pipeline *wrapper =
        (struct osito_pipeline *)(uintptr_t)pipeline;
    PFN_vkDestroyPipeline destroy =
        (PFN_vkDestroyPipeline)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyPipeline");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool)
{
    if (!device || !create_info || !pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateDescriptorPool create =
        (PFN_vkCreateDescriptorPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateDescriptorPool");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDescriptorPool real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_descriptor_pool *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyDescriptorPool destroy =
            (PFN_vkDestroyDescriptorPool)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyDescriptorPool");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *pool = (VkDescriptorPool)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                        const VkAllocationCallbacks *allocator)
{
    if (!device || !pool)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_pool *wrapper =
        (struct osito_descriptor_pool *)(uintptr_t)pool;
    PFN_vkDestroyDescriptorPool destroy =
        (PFN_vkDestroyDescriptorPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyDescriptorPool");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                      VkDescriptorPoolResetFlags flags)
{
    if (!device || !pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_pool *wrapper =
        (struct osito_descriptor_pool *)(uintptr_t)pool;
    if (wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkResetDescriptorPool reset =
        (PFN_vkResetDescriptorPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkResetDescriptorPool");
    return reset ? reset(self->real, wrapper->real, flags)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *allocate_info,
    VkDescriptorSet *sets)
{
    if (!device || !allocate_info || !allocate_info->descriptorPool ||
        !allocate_info->descriptorSetCount || !sets)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_descriptor_pool *pool =
        (struct osito_descriptor_pool *)(uintptr_t)
            allocate_info->descriptorPool;
    if (pool->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorSetAllocateInfo real_info = *allocate_info;
    real_info.descriptorPool = pool->real;
    VkDescriptorSet *real_sets =
        malloc(sizeof(*real_sets) * allocate_info->descriptorSetCount);
    if (!real_sets)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    PFN_vkAllocateDescriptorSets allocate =
        (PFN_vkAllocateDescriptorSets)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkAllocateDescriptorSets");
    if (!allocate) {
        free(real_sets);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    VkResult result = allocate(self->real, &real_info, real_sets);
    if (result != VK_SUCCESS) {
        free(real_sets);
        return result;
    }
    for (uint32_t i = 0; i < allocate_info->descriptorSetCount; i++) {
        struct osito_descriptor_set *wrapper = malloc(sizeof(*wrapper));
        if (!wrapper) {
            free(real_sets);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        wrapper->owner = self;
        wrapper->real = real_sets[i];
        sets[i] = (VkDescriptorSet)(uintptr_t)wrapper;
    }
    free(real_sets);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSets(VkDevice device, uint32_t write_count,
                       const VkWriteDescriptorSet *writes,
                       uint32_t copy_count,
                       const VkCopyDescriptorSet *copies)
{
    if (!device || (write_count && !writes) || copy_count || copies)
        return;
    struct osito_device *self = osito_device_from(device);
    VkWriteDescriptorSet *real_writes =
        write_count ? malloc(sizeof(*real_writes) * write_count) : 0;
    void **allocated_infos = write_count
        ? malloc(sizeof(*allocated_infos) * write_count) : 0;
    if (write_count && (!real_writes || !allocated_infos)) {
        free(allocated_infos);
        free(real_writes);
        return;
    }
    memset(allocated_infos, 0, sizeof(*allocated_infos) * write_count);
    for (uint32_t i = 0; i < write_count; i++) {
        real_writes[i] = writes[i];
        struct osito_descriptor_set *set =
            (struct osito_descriptor_set *)(uintptr_t)writes[i].dstSet;
        if (!set || set->owner != self)
            goto cleanup;
        real_writes[i].dstSet = set->real;
        real_writes[i].pImageInfo = 0;
        real_writes[i].pBufferInfo = 0;
        real_writes[i].pTexelBufferView = 0;
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            writes[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            writes[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            if (!writes[i].pBufferInfo)
                goto cleanup;
            VkDescriptorBufferInfo *infos =
                malloc(sizeof(*infos) * writes[i].descriptorCount);
            if (!infos)
                goto cleanup;
            allocated_infos[i] = infos;
            memcpy(infos, writes[i].pBufferInfo,
                   sizeof(*infos) * writes[i].descriptorCount);
            for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
                if (!infos[j].buffer)
                    continue;
                struct osito_buffer *buffer =
                    (struct osito_buffer *)(uintptr_t)infos[j].buffer;
                if (buffer->owner != self)
                    goto cleanup;
                infos[j].buffer = buffer->real;
            }
            real_writes[i].pBufferInfo = infos;
        } else if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                   writes[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                   writes[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                   writes[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            if (!writes[i].pImageInfo)
                goto cleanup;
            VkDescriptorImageInfo *infos =
                malloc(sizeof(*infos) * writes[i].descriptorCount);
            if (!infos)
                goto cleanup;
            allocated_infos[i] = infos;
            memcpy(infos, writes[i].pImageInfo,
                   sizeof(*infos) * writes[i].descriptorCount);
            for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
                if (infos[j].sampler) {
                    struct osito_sampler *sampler =
                        (struct osito_sampler *)(uintptr_t)infos[j].sampler;
                    if (sampler->owner != self)
                        goto cleanup;
                    infos[j].sampler = sampler->real;
                }
                if (infos[j].imageView) {
                    struct osito_image_view *view =
                        (struct osito_image_view *)(uintptr_t)
                            infos[j].imageView;
                    if (view->owner != self)
                        goto cleanup;
                    infos[j].imageView = view->real;
                }
            }
            real_writes[i].pImageInfo = infos;
        } else {
            goto cleanup;
        }
    }
    PFN_vkUpdateDescriptorSets update =
        (PFN_vkUpdateDescriptorSets)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkUpdateDescriptorSets");
    if (update)
        update(self->real, write_count, real_writes, 0, 0);

cleanup:
    for (uint32_t i = 0; i < write_count; i++)
        free(allocated_infos[i]);
    free(allocated_infos);
    free(real_writes);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *create_info,
               const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    if (!device || !create_info || !buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateBuffer create =
        (PFN_vkCreateBuffer)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateBuffer");
    if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkBuffer real = VK_NULL_HANDLE;
    VkResult result = create(self->real, create_info, allocator, &real);
    if (result != VK_SUCCESS)
        return result;
    struct osito_buffer *wrapper = malloc(sizeof(*wrapper));
    if (!wrapper) {
        PFN_vkDestroyBuffer destroy =
            (PFN_vkDestroyBuffer)self->owner->icd->get_proc_addr(
                self->owner->handle, "vkDestroyBuffer");
        if (destroy)
            destroy(self->real, real, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    wrapper->owner = self;
    wrapper->real = real;
    *buffer = (VkBuffer)(uintptr_t)wrapper;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                const VkAllocationCallbacks *allocator)
{
    if (!device || !buffer)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    PFN_vkDestroyBuffer destroy =
        (PFN_vkDestroyBuffer)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyBuffer");
    if (destroy)
        destroy(self->real, wrapper->real, allocator);
    free(wrapper);
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                              VkMemoryRequirements *requirements)
{
    if (!device || !buffer || !requirements)
        return;
    memset(requirements, 0, sizeof(*requirements));
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    PFN_vkGetBufferMemoryRequirements get =
        (PFN_vkGetBufferMemoryRequirements)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetBufferMemoryRequirements");
    if (get)
        get(self->real, wrapper->real, requirements);
}

VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements)
{
    if (!device || !info || !requirements || !info->buffer)
        return;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *wrapper =
        (struct osito_buffer *)(uintptr_t)info->buffer;
    VkBufferMemoryRequirementsInfo2 real_info = *info;
    real_info.buffer = wrapper->real;
    PFN_vkGetBufferMemoryRequirements2 get =
        (PFN_vkGetBufferMemoryRequirements2)
            self->owner->icd->get_proc_addr(
                self->owner->handle, "vkGetBufferMemoryRequirements2");
    if (get) {
        get(self->real, &real_info, requirements);
        return;
    }
    vkGetBufferMemoryRequirements(device, info->buffer,
                                  &requirements->memoryRequirements);
    for (VkBaseOutStructure *next = (VkBaseOutStructure *)requirements->pNext;
         next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dedicated =
                (VkMemoryDedicatedRequirements *)next;
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                   VkDeviceSize memory_offset)
{
    if (!device || !buffer || !memory)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    struct osito_buffer *buffer_wrapper =
        (struct osito_buffer *)(uintptr_t)buffer;
    struct osito_memory *memory_wrapper =
        (struct osito_memory *)(uintptr_t)memory;
    if (buffer_wrapper->owner != self || memory_wrapper->owner != self)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBindBufferMemory bind =
        (PFN_vkBindBufferMemory)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkBindBufferMemory");
    return bind ? bind(self->real, buffer_wrapper->real,
                       memory_wrapper->real, memory_offset)
                : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device,
                    const VkCommandPoolCreateInfo *create_info,
                    const VkAllocationCallbacks *allocator,
                    VkCommandPool *command_pool)
{
    if (!device || !create_info || !command_pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkCreateCommandPool create =
        (PFN_vkCreateCommandPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkCreateCommandPool");
    return create ? create(self->real, create_info, allocator, command_pool)
                  : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(VkDevice device, VkCommandPool command_pool,
                     const VkAllocationCallbacks *allocator)
{
    if (!device || !command_pool)
        return;
    struct osito_device *self = osito_device_from(device);
    PFN_vkDestroyCommandPool destroy =
        (PFN_vkDestroyCommandPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkDestroyCommandPool");
    if (destroy)
        destroy(self->real, command_pool, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandPool(VkDevice device, VkCommandPool command_pool,
                   VkCommandPoolResetFlags flags)
{
    if (!device || !command_pool)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkResetCommandPool reset =
        (PFN_vkResetCommandPool)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkResetCommandPool");
    return reset ? reset(self->real, command_pool, flags)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *allocate_info,
    VkCommandBuffer *command_buffers)
{
    if (!device || !allocate_info || !command_buffers ||
        !allocate_info->commandBufferCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_device *self = osito_device_from(device);
    PFN_vkAllocateCommandBuffers allocate =
        (PFN_vkAllocateCommandBuffers)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkAllocateCommandBuffers");
    if (!allocate)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint32_t count = allocate_info->commandBufferCount;
    VkCommandBuffer *real = malloc((size_t)count * sizeof(*real));
    if (!real)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = allocate(self->real, allocate_info, real);
    if (result != VK_SUCCESS) {
        free(real);
        return result;
    }
    for (uint32_t i = 0; i < count; i++) {
        struct osito_cmd_buffer *wrapper = malloc(sizeof(*wrapper));
        if (!wrapper) {
            PFN_vkFreeCommandBuffers release =
                (PFN_vkFreeCommandBuffers)self->owner->icd->get_proc_addr(
                    self->owner->handle, "vkFreeCommandBuffers");
            if (release)
                release(self->real, allocate_info->commandPool, count, real);
            for (uint32_t j = 0; j < i; j++)
                free((struct osito_cmd_buffer *)command_buffers[j]);
            free(real);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memset(wrapper, 0, sizeof(*wrapper));
        set_loader_magic_value(wrapper);
        wrapper->owner = self;
        wrapper->real = real[i];
        command_buffers[i] = (VkCommandBuffer)wrapper;
    }
    free(real);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers(VkDevice device, VkCommandPool command_pool,
                     uint32_t count,
                     const VkCommandBuffer *command_buffers)
{
    if (!device || !count || !command_buffers)
        return;
    struct osito_device *self = osito_device_from(device);
    VkCommandBuffer *real = malloc((size_t)count * sizeof(*real));
    if (!real)
        return;
    for (uint32_t i = 0; i < count; i++) {
        struct osito_cmd_buffer *wrapper =
            (struct osito_cmd_buffer *)command_buffers[i];
        real[i] = wrapper->real;
    }
    PFN_vkFreeCommandBuffers release =
        (PFN_vkFreeCommandBuffers)self->owner->icd->get_proc_addr(
            self->owner->handle, "vkFreeCommandBuffers");
    if (release)
        release(self->real, command_pool, count, real);
    for (uint32_t i = 0; i < count; i++)
        free((struct osito_cmd_buffer *)command_buffers[i]);
    free(real);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer command_buffer,
                     const VkCommandBufferBeginInfo *begin_info)
{
    if (!command_buffer || !begin_info)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkBeginCommandBuffer begin =
        (PFN_vkBeginCommandBuffer)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkBeginCommandBuffer");
    return begin ? begin(wrapper->real, begin_info)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer command_buffer)
{
    if (!command_buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkEndCommandBuffer end =
        (PFN_vkEndCommandBuffer)wrapper->owner->owner->icd->get_proc_addr(
            wrapper->owner->owner->handle, "vkEndCommandBuffer");
    return end ? end(wrapper->real) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandBuffer(VkCommandBuffer command_buffer,
                     VkCommandBufferResetFlags flags)
{
    if (!command_buffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_cmd_buffer *wrapper =
        (struct osito_cmd_buffer *)command_buffer;
    PFN_vkResetCommandBuffer reset =
        (PFN_vkResetCommandBuffer)
            wrapper->owner->owner->icd->get_proc_addr(
                wrapper->owner->owner->handle, "vkResetCommandBuffer");
    return reset ? reset(wrapper->real, flags)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, uint32_t submit_count,
               const VkSubmitInfo2 *submits, VkFence fence)
{
    if (!queue || (submit_count && !submits))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_queue *queue_wrapper = (struct osito_queue *)queue;
    struct osito_device *device = queue_wrapper->owner;
    PFN_vkQueueSubmit2 submit =
        (PFN_vkQueueSubmit2)device->owner->icd->get_proc_addr(
            device->owner->handle, "vkQueueSubmit2");
    if (!submit)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkSubmitInfo2 *real_submits = 0;
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (submit_count) {
        real_submits = malloc((size_t)submit_count * sizeof(*real_submits));
        if (!real_submits)
            return result;
        memset(real_submits, 0,
               (size_t)submit_count * sizeof(*real_submits));
    }

    for (uint32_t i = 0; i < submit_count; i++) {
        const VkSubmitInfo2 *source = &submits[i];
        VkSubmitInfo2 *target = &real_submits[i];
        if (source->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
            (source->waitSemaphoreInfoCount &&
             !source->pWaitSemaphoreInfos) ||
            (source->commandBufferInfoCount &&
             !source->pCommandBufferInfos) ||
            (source->signalSemaphoreInfoCount &&
             !source->pSignalSemaphoreInfos)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }
        *target = *source;
        target->pWaitSemaphoreInfos = 0;
        target->pCommandBufferInfos = 0;
        target->pSignalSemaphoreInfos = 0;

        if (source->waitSemaphoreInfoCount) {
            VkSemaphoreSubmitInfo *infos = malloc(
                (size_t)source->waitSemaphoreInfoCount * sizeof(*infos));
            if (!infos)
                goto cleanup;
            target->pWaitSemaphoreInfos = infos;
            for (uint32_t j = 0; j < source->waitSemaphoreInfoCount; j++) {
                infos[j] = source->pWaitSemaphoreInfos[j];
                struct osito_semaphore *wrapper =
                    (struct osito_semaphore *)(uintptr_t)infos[j].semaphore;
                if (!wrapper || wrapper->owner != device) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    goto cleanup;
                }
                infos[j].semaphore = wrapper->real;
            }
        }

        if (source->commandBufferInfoCount) {
            VkCommandBufferSubmitInfo *infos = malloc(
                (size_t)source->commandBufferInfoCount * sizeof(*infos));
            if (!infos)
                goto cleanup;
            target->pCommandBufferInfos = infos;
            for (uint32_t j = 0; j < source->commandBufferInfoCount; j++) {
                infos[j] = source->pCommandBufferInfos[j];
                struct osito_cmd_buffer *wrapper =
                    (struct osito_cmd_buffer *)infos[j].commandBuffer;
                if (!wrapper || wrapper->owner != device) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    goto cleanup;
                }
                infos[j].commandBuffer = wrapper->real;
            }
        }

        if (source->signalSemaphoreInfoCount) {
            VkSemaphoreSubmitInfo *infos = malloc(
                (size_t)source->signalSemaphoreInfoCount * sizeof(*infos));
            if (!infos)
                goto cleanup;
            target->pSignalSemaphoreInfos = infos;
            for (uint32_t j = 0; j < source->signalSemaphoreInfoCount; j++) {
                infos[j] = source->pSignalSemaphoreInfos[j];
                struct osito_semaphore *wrapper =
                    (struct osito_semaphore *)(uintptr_t)infos[j].semaphore;
                if (!wrapper || wrapper->owner != device) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    goto cleanup;
                }
                infos[j].semaphore = wrapper->real;
            }
        }
    }

    VkFence real_fence = VK_NULL_HANDLE;
    if (fence) {
        struct osito_fence *wrapper =
            (struct osito_fence *)(uintptr_t)fence;
        if (wrapper->owner != device) {
            result = VK_ERROR_INITIALIZATION_FAILED;
            goto cleanup;
        }
        real_fence = wrapper->real;
    }
    result = submit(queue_wrapper->real, submit_count, real_submits,
                    real_fence);

cleanup:
    for (uint32_t i = 0; i < submit_count; i++) {
        free((void *)real_submits[i].pWaitSemaphoreInfos);
        free((void *)real_submits[i].pCommandBufferInfos);
        free((void *)real_submits[i].pSignalSemaphoreInfos);
    }
    free(real_submits);
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!name)
        return 0;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (!instance)
        return 0;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)vkDestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2;
    if (strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2;
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)vkCreateDevice;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (strcmp(name, "vkDeviceWaitIdle") == 0)
        return (PFN_vkVoidFunction)vkDeviceWaitIdle;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorSetLayout;
    if (strcmp(name, "vkDestroyDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorSetLayout;
    if (strcmp(name, "vkCreateDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplate;
    if (strcmp(name, "vkDestroyDescriptorUpdateTemplate") == 0 ||
        strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorUpdateTemplate;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkCreatePipelineLayout;
    if (strcmp(name, "vkDestroyPipelineLayout") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipelineLayout;
    if (strcmp(name, "vkCreateSemaphore") == 0)
        return (PFN_vkVoidFunction)vkCreateSemaphore;
    if (strcmp(name, "vkDestroySemaphore") == 0)
        return (PFN_vkVoidFunction)vkDestroySemaphore;
    if (strcmp(name, "vkCreateFence") == 0)
        return (PFN_vkVoidFunction)vkCreateFence;
    if (strcmp(name, "vkDestroyFence") == 0)
        return (PFN_vkVoidFunction)vkDestroyFence;
    if (strcmp(name, "vkResetFences") == 0)
        return (PFN_vkVoidFunction)vkResetFences;
    if (strcmp(name, "vkWaitForFences") == 0)
        return (PFN_vkVoidFunction)vkWaitForFences;
    if (strcmp(name, "vkCreateCommandPool") == 0)
        return (PFN_vkVoidFunction)vkCreateCommandPool;
    if (strcmp(name, "vkDestroyCommandPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyCommandPool;
    if (strcmp(name, "vkResetCommandPool") == 0)
        return (PFN_vkVoidFunction)vkResetCommandPool;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)vkFreeCommandBuffers;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkBeginCommandBuffer;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkEndCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)vkResetCommandBuffer;
    if (strcmp(name, "vkQueueSubmit2") == 0 ||
        strcmp(name, "vkQueueSubmit2KHR") == 0)
        return (PFN_vkVoidFunction)vkQueueSubmit2;
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)vkCreateBuffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)vkDestroyBuffer;
    if (strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements;
    if (strcmp(name, "vkGetBufferMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2;
    if (strcmp(name, "vkBindBufferMemory") == 0)
        return (PFN_vkVoidFunction)vkBindBufferMemory;
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)vkAllocateMemory;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)vkFreeMemory;
    if (strcmp(name, "vkMapMemory") == 0)
        return (PFN_vkVoidFunction)vkMapMemory;
    if (strcmp(name, "vkUnmapMemory") == 0)
        return (PFN_vkVoidFunction)vkUnmapMemory;
    if (strcmp(name, "vkCmdCopyBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBuffer2;
    if (strcmp(name, "vkCmdPipelineBarrier2") == 0 ||
        strcmp(name, "vkCmdPipelineBarrier2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdPipelineBarrier2;
    if (strcmp(name, "vkCmdCopyBufferToImage2") == 0 ||
        strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyBufferToImage2;
    if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 ||
        strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdCopyImageToBuffer2;
    if (strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearColorImage;
    if (strcmp(name, "vkCmdClearDepthStencilImage") == 0)
        return (PFN_vkVoidFunction)vkCmdClearDepthStencilImage;
    if (strcmp(name, "vkCmdBeginRendering") == 0 ||
        strcmp(name, "vkCmdBeginRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBeginRendering;
    if (strcmp(name, "vkCmdEndRendering") == 0 ||
        strcmp(name, "vkCmdEndRenderingKHR") == 0)
        return (PFN_vkVoidFunction)vkCmdEndRendering;
    if (strcmp(name, "vkCmdBindVertexBuffers2") == 0 ||
        strcmp(name, "vkCmdBindVertexBuffers2EXT") == 0)
        return (PFN_vkVoidFunction)vkCmdBindVertexBuffers2;
    if (strcmp(name, "vkCmdBindIndexBuffer") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer;
    if (strcmp(name, "vkCmdBindIndexBuffer2") == 0 ||
        strcmp(name, "vkCmdBindIndexBuffer2KHR") == 0)
        return (PFN_vkVoidFunction)vkCmdBindIndexBuffer2KHR;
    if (strcmp(name, "vkCmdBindPipeline") == 0)
        return (PFN_vkVoidFunction)vkCmdBindPipeline;
    if (strcmp(name, "vkCmdSetCullMode") == 0 ||
        strcmp(name, "vkCmdSetCullModeEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetCullMode;
    if (strcmp(name, "vkCmdSetFrontFace") == 0 ||
        strcmp(name, "vkCmdSetFrontFaceEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetFrontFace;
    if (strcmp(name, "vkCmdSetPrimitiveTopology") == 0 ||
        strcmp(name, "vkCmdSetPrimitiveTopologyEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetPrimitiveTopology;
    if (strcmp(name, "vkCmdSetBlendConstants") == 0)
        return (PFN_vkVoidFunction)vkCmdSetBlendConstants;
    if (strcmp(name, "vkCmdSetViewportWithCount") == 0 ||
        strcmp(name, "vkCmdSetViewportWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetViewportWithCount;
    if (strcmp(name, "vkCmdSetScissorWithCount") == 0 ||
        strcmp(name, "vkCmdSetScissorWithCountEXT") == 0)
        return (PFN_vkVoidFunction)vkCmdSetScissorWithCount;
    if (strcmp(name, "vkCmdDraw") == 0)
        return (PFN_vkVoidFunction)vkCmdDraw;
    if (strcmp(name, "vkCmdDrawIndexed") == 0)
        return (PFN_vkVoidFunction)vkCmdDrawIndexed;
    if (strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkCmdBindDescriptorSets;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)vkCreateImage;
    if (strcmp(name, "vkDestroyImage") == 0)
        return (PFN_vkVoidFunction)vkDestroyImage;
    if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements;
    if (strcmp(name, "vkGetImageMemoryRequirements2") == 0 ||
        strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0)
        return (PFN_vkVoidFunction)vkGetImageMemoryRequirements2;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)vkBindImageMemory;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)vkCreateImageView;
    if (strcmp(name, "vkDestroyImageView") == 0)
        return (PFN_vkVoidFunction)vkDestroyImageView;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)vkCreateSampler;
    if (strcmp(name, "vkDestroySampler") == 0)
        return (PFN_vkVoidFunction)vkDestroySampler;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)vkCreateShaderModule;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)vkDestroyShaderModule;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateGraphicsPipelines;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)vkCreateComputePipelines;
    if (strcmp(name, "vkDestroyPipeline") == 0)
        return (PFN_vkVoidFunction)vkDestroyPipeline;
    if (strcmp(name, "vkCreateDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkCreateDescriptorPool;
    if (strcmp(name, "vkDestroyDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkDestroyDescriptorPool;
    if (strcmp(name, "vkResetDescriptorPool") == 0)
        return (PFN_vkVoidFunction)vkResetDescriptorPool;
    if (strcmp(name, "vkAllocateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkAllocateDescriptorSets;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)vkUpdateDescriptorSets;
    if (strcmp(name, "vkCreateOsitokCompositorSurfaceKHR") == 0)
        return (PFN_vkVoidFunction)vkCreateOsitokCompositorSurfaceKHR;
    if (strcmp(name, "vkDestroySurfaceKHR") == 0)
        return (PFN_vkVoidFunction)vkDestroySurfaceKHR;
    return 0;
}
