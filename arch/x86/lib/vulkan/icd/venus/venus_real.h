#ifndef OSITOK_VENUS_REAL_H
#define OSITOK_VENUS_REAL_H

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ositok.h>
#include <vulkan/vk_icd.h>

#include "venus_wire.h"

#define VENUS_SYS_GPU_CAPS 600L
#define VENUS_SYS_GPU_CTX_CREATE 601L
#define VENUS_SYS_GPU_CTX_DESTROY 602L
#define VENUS_GPU_CAP_READY (1u << 0)
#define VENUS_GPU_CTX 0u

struct venus_physical_device_real {
    VK_LOADER_DATA loader_data;
    uint64_t object_id;
    struct venus_instance_real *instance;
    VkQueueFamilyProperties *queue_family_properties;
    uint32_t queue_family_count;
    uint32_t queue_families_initialized;
    VkExtensionProperties *extension_properties;
    uint32_t extension_count;
    uint32_t extensions_initialized;
};

struct venus_instance_real {
    VK_LOADER_DATA loader_data;
    uint32_t caps;
    int32_t ctx_id;
    uint64_t object_id;
    struct venus_wire *wire;
    struct venus_physical_device_real *physical_devices;
    uint32_t physical_device_count;
    uint32_t physical_devices_initialized;
    uint32_t queue_ring_mask;
};

struct venus_device_real;

struct venus_queue_real {
    VK_LOADER_DATA loader_data;
    uint64_t object_id;
    struct venus_device_real *device;
    uint32_t family_index;
    uint32_t queue_index;
    uint32_t ring_index;
};

struct venus_device_real {
    VK_LOADER_DATA loader_data;
    uint64_t object_id;
    struct venus_physical_device_real *physical_device;
    struct venus_queue_real *queues;
    uint32_t queue_count;
};

struct venus_surface_real {
    struct venus_instance_real *instance;
    uint32_t window_id;
};

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EnumerateInstanceVersion(uint32_t *api_version);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateInstance(const VkInstanceCreateInfo *create_info,
                          const VkAllocationCallbacks *allocator,
                          VkInstance *instance);

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyInstance(VkInstance instance,
                           const VkAllocationCallbacks *allocator);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EnumeratePhysicalDevices(VkInstance instance,
                                    uint32_t *physical_device_count,
                                    VkPhysicalDevice *physical_devices);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceProperties(VkPhysicalDevice physical_device,
                                       VkPhysicalDeviceProperties *properties);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t *property_count,
    VkQueueFamilyProperties *properties);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceFeatures(VkPhysicalDevice physical_device,
                                     VkPhysicalDeviceFeatures *features);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties *properties);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceProperties2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2 *properties);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2 *features);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties2 *properties);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties2 *properties);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *properties);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char *layer_name,
    uint32_t *property_count, VkExtensionProperties *properties);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDevice(VkPhysicalDevice physical_device,
                        const VkDeviceCreateInfo *create_info,
                        const VkAllocationCallbacks *allocator,
                        VkDevice *device);

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDevice(VkDevice device,
                         const VkAllocationCallbacks *allocator);

VKAPI_ATTR void VKAPI_CALL
venus_real_GetDeviceQueue(VkDevice device, uint32_t queue_family_index,
                          uint32_t queue_index, VkQueue *queue);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_DeviceWaitIdle(VkDevice device);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorSetLayout *set_layout);

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout set_layout,
    const VkAllocationCallbacks *allocator);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDescriptorUpdateTemplate(
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo *create_info,
    const VkAllocationCallbacks *allocator,
    VkDescriptorUpdateTemplate *update_template);

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate update_template,
    const VkAllocationCallbacks *allocator);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkPipelineLayout *pipeline_layout);

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipeline_layout,
    const VkAllocationCallbacks *allocator);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateSemaphore(VkDevice device,
                           const VkSemaphoreCreateInfo *create_info,
                           const VkAllocationCallbacks *allocator,
                           VkSemaphore *semaphore);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroySemaphore(VkDevice device, VkSemaphore semaphore,
                            const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateFence(VkDevice device,
                       const VkFenceCreateInfo *create_info,
                       const VkAllocationCallbacks *allocator,
                       VkFence *fence);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyFence(VkDevice device, VkFence fence,
                         const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetFences(VkDevice device, uint32_t fence_count,
                       const VkFence *fences);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_WaitForFences(VkDevice device, uint32_t fence_count,
                         const VkFence *fences, VkBool32 wait_all,
                         uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateCommandPool(VkDevice device,
                             const VkCommandPoolCreateInfo *create_info,
                             const VkAllocationCallbacks *allocator,
                             VkCommandPool *command_pool);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyCommandPool(VkDevice device, VkCommandPool command_pool,
                              const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetCommandPool(VkDevice device, VkCommandPool command_pool,
                            VkCommandPoolResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *allocate_info,
    VkCommandBuffer *command_buffers);
VKAPI_ATTR void VKAPI_CALL
venus_real_FreeCommandBuffers(VkDevice device, VkCommandPool command_pool,
                              uint32_t count,
                              const VkCommandBuffer *command_buffers);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_BeginCommandBuffer(VkCommandBuffer command_buffer,
                              const VkCommandBufferBeginInfo *begin_info);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_EndCommandBuffer(VkCommandBuffer command_buffer);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetCommandBuffer(VkCommandBuffer command_buffer,
                               VkCommandBufferResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_QueueSubmit2(VkQueue queue, uint32_t submit_count,
                        const VkSubmitInfo2 *submits, VkFence fence);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdCopyBuffer2(VkCommandBuffer command_buffer,
                          const VkCopyBufferInfo2 *copy_info);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdPipelineBarrier2(VkCommandBuffer command_buffer,
                               const VkDependencyInfo *dependency_info);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdCopyBufferToImage2(
    VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2 *copy_info);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdCopyImageToBuffer2(
    VkCommandBuffer command_buffer,
    const VkCopyImageToBufferInfo2 *copy_info);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdClearColorImage(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearColorValue *color, uint32_t range_count,
    const VkImageSubresourceRange *ranges);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdClearDepthStencilImage(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearDepthStencilValue *depth_stencil, uint32_t range_count,
    const VkImageSubresourceRange *ranges);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBeginRendering(VkCommandBuffer command_buffer,
                             const VkRenderingInfo *rendering_info);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdEndRendering(VkCommandBuffer command_buffer);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindVertexBuffers2(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer *buffers,
    const VkDeviceSize *offsets, const VkDeviceSize *sizes,
    const VkDeviceSize *strides);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer,
                              VkDeviceSize offset, VkIndexType index_type);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindIndexBuffer2(VkCommandBuffer command_buffer, VkBuffer buffer,
                               VkDeviceSize offset, VkDeviceSize size,
                               VkIndexType index_type);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindPipeline(VkCommandBuffer command_buffer,
                           VkPipelineBindPoint bind_point,
                           VkPipeline pipeline);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetCullMode(VkCommandBuffer command_buffer,
                          VkCullModeFlags cull_mode);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetFrontFace(VkCommandBuffer command_buffer,
                           VkFrontFace front_face);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetPrimitiveTopology(VkCommandBuffer command_buffer,
                                   VkPrimitiveTopology topology);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetBlendConstants(VkCommandBuffer command_buffer,
                                const float blend_constants[4]);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetViewportWithCount(VkCommandBuffer command_buffer,
                                   uint32_t viewport_count,
                                   const VkViewport *viewports);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdSetScissorWithCount(VkCommandBuffer command_buffer,
                                  uint32_t scissor_count,
                                  const VkRect2D *scissors);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdDraw(VkCommandBuffer command_buffer, uint32_t vertex_count,
                   uint32_t instance_count, uint32_t first_vertex,
                   uint32_t first_instance);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdDrawIndexed(VkCommandBuffer command_buffer,
                          uint32_t index_count, uint32_t instance_count,
                          uint32_t first_index, int32_t vertex_offset,
                          uint32_t first_instance);
VKAPI_ATTR void VKAPI_CALL
venus_real_CmdBindDescriptorSets(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t first_set,
    uint32_t descriptor_set_count, const VkDescriptorSet *descriptor_sets,
    uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateBuffer(VkDevice device,
                        const VkBufferCreateInfo *create_info,
                        const VkAllocationCallbacks *allocator,
                        VkBuffer *buffer);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyBuffer(VkDevice device, VkBuffer buffer,
                         const VkAllocationCallbacks *allocator);
VKAPI_ATTR void VKAPI_CALL
venus_real_GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                       VkMemoryRequirements *requirements);
VKAPI_ATTR void VKAPI_CALL
venus_real_GetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_BindBufferMemory(VkDevice device, VkBuffer buffer,
                            VkDeviceMemory memory,
                            VkDeviceSize memory_offset);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_AllocateMemory(VkDevice device,
                          const VkMemoryAllocateInfo *allocate_info,
                          const VkAllocationCallbacks *allocator,
                          VkDeviceMemory *memory);
VKAPI_ATTR void VKAPI_CALL
venus_real_FreeMemory(VkDevice device, VkDeviceMemory memory,
                      const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_MapMemory(VkDevice device, VkDeviceMemory memory,
                     VkDeviceSize offset, VkDeviceSize size,
                     VkMemoryMapFlags flags, void **data);
VKAPI_ATTR void VKAPI_CALL
venus_real_UnmapMemory(VkDevice device, VkDeviceMemory memory);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateImage(VkDevice device,
                       const VkImageCreateInfo *create_info,
                       const VkAllocationCallbacks *allocator,
                       VkImage *image);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyImage(VkDevice device, VkImage image,
                        const VkAllocationCallbacks *allocator);
VKAPI_ATTR void VKAPI_CALL
venus_real_GetImageMemoryRequirements(VkDevice device, VkImage image,
                                      VkMemoryRequirements *requirements);
VKAPI_ATTR void VKAPI_CALL
venus_real_GetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_BindImageMemory(VkDevice device, VkImage image,
                           VkDeviceMemory memory,
                           VkDeviceSize memory_offset);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateImageView(VkDevice device,
                           const VkImageViewCreateInfo *create_info,
                           const VkAllocationCallbacks *allocator,
                           VkImageView *view);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyImageView(VkDevice device, VkImageView view,
                            const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateSampler(VkDevice device,
                         const VkSamplerCreateInfo *create_info,
                         const VkAllocationCallbacks *allocator,
                         VkSampler *sampler);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroySampler(VkDevice device, VkSampler sampler,
                          const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkShaderModule *shader_module);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyShaderModule(VkDevice device,
                               VkShaderModule shader_module,
                               const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateComputePipelines(
    VkDevice device, VkPipelineCache pipeline_cache,
    uint32_t create_info_count,
    const VkComputePipelineCreateInfo *create_infos,
    const VkAllocationCallbacks *allocator, VkPipeline *pipelines);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyPipeline(VkDevice device, VkPipeline pipeline,
                           const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool);
VKAPI_ATTR void VKAPI_CALL
venus_real_DestroyDescriptorPool(
    VkDevice device, VkDescriptorPool pool,
    const VkAllocationCallbacks *allocator);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                               VkDescriptorPoolResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL
venus_real_AllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *allocate_info,
    VkDescriptorSet *sets);
VKAPI_ATTR void VKAPI_CALL
venus_real_UpdateDescriptorSets(
    VkDevice device, uint32_t write_count,
    const VkWriteDescriptorSet *writes, uint32_t copy_count,
    const VkCopyDescriptorSet *copies);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateOsitokCompositorSurfaceKHR(
    VkInstance instance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                             const VkAllocationCallbacks *allocator);

#endif
