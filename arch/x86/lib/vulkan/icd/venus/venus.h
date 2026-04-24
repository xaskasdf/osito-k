/*
 * venus.h — userland venus ICD for OsitoK.
 *
 * Wave 3a: skeleton only. No protocol wire encoding yet. Validates the
 * kernel handshake path (SYS_GPU_CAPS / CTX_CREATE / CTX_DESTROY) and
 * hosts the Vulkan dispatch for instance+device lifecycles.
 */
#ifndef OSITOK_VK_VENUS_H
#define OSITOK_VK_VENUS_H

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* Matches kernel ABI from arch/x86/include/sys/gpu_syscalls.h. We do NOT
 * include that header here because it's kernel-tree; duplicate the minimum
 * constants instead. */
#define VENUS_SYS_GPU_CAPS         600L
#define VENUS_SYS_GPU_CTX_CREATE   601L
#define VENUS_SYS_GPU_CTX_DESTROY  602L
#define VENUS_GPU_CAP_VENUS_READY  (1u << 0)
#define VENUS_GPU_CTX_VENUS        0x00u

/* Forward decl — real definition in venus_wire.c (production) or
 * venus_wire_test_shim.c (test binary). Callers treat it as opaque. */
struct venus_wire;

struct venus_instance {
    VK_LOADER_DATA loader_data;
    uint32_t       caps;          /* SYS_GPU_CAPS snapshot */
    int32_t        ctx_id;        /* kernel GPU ctx, 0 if none */
    struct venus_wire *wire;      /* guest-side ring wrapper (W3b.1) */
    uint64_t       host_handle;   /* host VkInstance handle-id (W3b.1) */
};

/* Guest-local memory tracking (W3b.3). Sized for hello-memory plus a
 * little headroom — not a general-purpose allocator. */
#define VENUS_MAX_MEM_OBJECTS  32u
#define VENUS_MAX_BUF_OBJECTS  32u

/* W3b.4 object tables. */
#define VENUS_MAX_SHADER_OBJECTS       32u
#define VENUS_MAX_RP_OBJECTS           32u
#define VENUS_MAX_IMAGE_OBJECTS        32u
#define VENUS_MAX_IMAGE_VIEW_OBJECTS   32u
#define VENUS_MAX_FB_OBJECTS           32u
#define VENUS_MAX_PL_LAYOUT_OBJECTS    32u
#define VENUS_MAX_PIPELINE_OBJECTS     32u
#define VENUS_MAX_CMD_POOL_OBJECTS     16u
#define VENUS_MAX_CMD_BUFFER_OBJECTS   32u

struct venus_memory {
    uint64_t host_id;       /* host VkDeviceMemory id (0 if guest-local fallback) */
    uint64_t size;
    void    *local_ptr;     /* guest-side backing buffer (malloc'd) */
    uint32_t type_index;
    uint32_t in_use;
    uint32_t mapped;
    uint32_t _pad;
};

struct venus_buffer {
    uint64_t host_id;       /* host VkBuffer id (0 if guest-local fallback) */
    uint64_t size;
    uint32_t usage;
    uint32_t in_use;
    /* Bind-time bookkeeping (guest-local, no host side effect). */
    int32_t  bound_mem_slot;
    uint32_t _pad;
    uint64_t bound_offset;
};

struct venus_shader {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t code_size;
};

struct venus_render_pass {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t attachment_count;
};

struct venus_image {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    int32_t  bound_mem_slot;
    uint32_t _pad;
    uint64_t bound_offset;
};

struct venus_image_view {
    uint64_t host_id;
    uint32_t in_use;
    int32_t  image_slot;
};

struct venus_framebuffer {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t width;
    uint32_t height;
    uint32_t _pad;
};

struct venus_pipeline_layout {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t _pad;
};

struct venus_pipeline {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t _pad;
};

struct venus_cmd_pool {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t queue_family_index;
};

struct venus_cmd_buffer {
    VK_LOADER_DATA loader_data;        /* MUST be first — dispatchable handle */
    struct venus_device *owner;        /* back-pointer for dispatch */
    uint64_t host_id;                  /* host VkCommandBuffer id (0 for guest-local) */
    int32_t  pool_slot;
    uint32_t in_use;
    uint32_t recording;                /* 1 after Begin, 0 after End */
    uint32_t _pad;
};

struct venus_device {
    VK_LOADER_DATA loader_data;
    struct venus_instance *parent;
    /* Single implicit queue for W3a — real queue families land in W3b. */
    VK_LOADER_DATA queue_loader_data;
    uint64_t host_handle;   /* host VkDevice id (0 if guest-local fallback) */
    struct venus_memory memories[VENUS_MAX_MEM_OBJECTS];
    struct venus_buffer buffers[VENUS_MAX_BUF_OBJECTS];
    /* W3b.4 slot tables. */
    struct venus_shader          shaders       [VENUS_MAX_SHADER_OBJECTS];
    struct venus_render_pass     render_passes [VENUS_MAX_RP_OBJECTS];
    struct venus_image           images        [VENUS_MAX_IMAGE_OBJECTS];
    struct venus_image_view      image_views   [VENUS_MAX_IMAGE_VIEW_OBJECTS];
    struct venus_framebuffer     framebuffers  [VENUS_MAX_FB_OBJECTS];
    struct venus_pipeline_layout pl_layouts    [VENUS_MAX_PL_LAYOUT_OBJECTS];
    struct venus_pipeline        pipelines     [VENUS_MAX_PIPELINE_OBJECTS];
    struct venus_cmd_pool        cmd_pools     [VENUS_MAX_CMD_POOL_OBJECTS];
    struct venus_cmd_buffer      cmd_buffers   [VENUS_MAX_CMD_BUFFER_OBJECTS];
};

/* Entry points. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *,
                     VkInstance *);

VKAPI_ATTR void VKAPI_CALL
venus_DestroyInstance(VkInstance, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_EnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *,
                                             VkQueueFamilyProperties *);

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice,
                                        VkPhysicalDeviceMemoryProperties *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                   const VkAllocationCallbacks *, VkDevice *);

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDevice(VkDevice, const VkAllocationCallbacks *);

VKAPI_ATTR void VKAPI_CALL
venus_GetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);

/* W3b.3 — memory + buffer lifecycle. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateMemory(VkDevice, const VkMemoryAllocateInfo *,
                     const VkAllocationCallbacks *, VkDeviceMemory *);

VKAPI_ATTR void VKAPI_CALL
venus_FreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_MapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
                VkMemoryMapFlags, void **);

VKAPI_ATTR void VKAPI_CALL
venus_UnmapMemory(VkDevice, VkDeviceMemory);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateBuffer(VkDevice, const VkBufferCreateInfo *,
                   const VkAllocationCallbacks *, VkBuffer *);

VKAPI_ATTR void VKAPI_CALL
venus_DestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);

VKAPI_ATTR void VKAPI_CALL
venus_GetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_BindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);

/* W3b.4 — shader + render pass + image + framebuffer + pipeline + cmd buffer. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *,
                         const VkAllocationCallbacks *, VkShaderModule *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateRenderPass(VkDevice, const VkRenderPassCreateInfo *,
                       const VkAllocationCallbacks *, VkRenderPass *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateImage(VkDevice, const VkImageCreateInfo *,
                  const VkAllocationCallbacks *, VkImage *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);

VKAPI_ATTR void VKAPI_CALL
venus_GetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_BindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateImageView(VkDevice, const VkImageViewCreateInfo *,
                      const VkAllocationCallbacks *, VkImageView *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *,
                        const VkAllocationCallbacks *, VkFramebuffer *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *,
                           const VkAllocationCallbacks *, VkPipelineLayout *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t,
                              const VkGraphicsPipelineCreateInfo *,
                              const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *,
                        const VkAllocationCallbacks *, VkCommandPool *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *,
                             VkCommandBuffer *);
VKAPI_ATTR void VKAPI_CALL
venus_FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_BeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_EndCommandBuffer(VkCommandBuffer);

VKAPI_ATTR void VKAPI_CALL
venus_CmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL
venus_CmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance, const char *);

#endif
