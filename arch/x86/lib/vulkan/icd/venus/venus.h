/*
 * venus.h — userland venus ICD for OsitoK.
 *
 * Wave history:
 *   W3a    — skeleton + kernel handshake (SYS_GPU_CAPS / CTX_CREATE
 *            / CTX_DESTROY), instance+device lifecycle.
 *   W3b.1  — venus wire-protocol ring (guest-host command + reply).
 *   W3b.2  — physical-device queries over the wire.
 *   W3b.3  — VkDeviceMemory + VkBuffer lifecycle + hello-memory.
 *   W3b.4  — shader / render pass / image / image view / framebuffer /
 *            pipeline layout / graphics pipeline / command pool / command
 *            buffer. hello-pipeline smoke. Cmd* recording is guest-local
 *            no-op (real submission lands in W3b.5 with WSI+swapchain).
 *   W3b.5  — WSI: surface / swapchain / queue / sync primitives /
 *            present via SYS_GUI_FLIP (OsitoK compositor). All entries
 *            have guest-local fallbacks. hello-swapchain smoke. Memory
 *            slots can be upgraded to SHM-backed when bound to a
 *            swapchain-owned image (VkBindImageMemory upgrades path).
 *
 * W3b.5 handle markers (keep in lockstep with venus_w3b5_objects.c):
 *   SURFACE    0x0
 *   QUEUE      dispatchable  (VK_LOADER_DATA pointer)
 *   FENCE      0x1
 *   SEMAPHORE  0x2
 *   SWAPCHAIN  0x3
 * Markers occupy only bits 60..63. Slot decode is
 * (handle >> 48) & 0x0FFF — see the W3b.3-fix lesson.
 *
 * W3b.5 linker invariant: venus_w3b5_objects.c MUST appear before
 * venus_w3b4_objects.c in the Makefile SRC list. Both TUs define
 * `venus_BindImageMemory`; --allow-multiple-definition picks the
 * first, and w3b5's variant injects the SHM surface upgrade when
 * the image is_swapchain_owned.
 */
#ifndef OSITOK_VK_VENUS_H
#define OSITOK_VK_VENUS_H

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan_ositok.h>

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

/* Guest-local object tracking. Handles encode 12 slot bits, so these
 * tables can grow up to 4096 entries per object class. DXVK creates many
 * transient buffers during device warmup, so keep enough headroom beyond
 * the original hello-memory smoke-test sizes. */
#define VENUS_MAX_MEM_OBJECTS  512u
#define VENUS_MAX_BUF_OBJECTS  2048u

/* W3b.5 object tables. */
#define VENUS_MAX_SURFACE_OBJECTS     32u
#define VENUS_MAX_QUEUE_OBJECTS        8u
#define VENUS_MAX_FENCE_OBJECTS      256u
#define VENUS_MAX_SEMA_OBJECTS       256u
#define VENUS_MAX_SWAPCHAIN_OBJECTS    8u
#define VENUS_MAX_SWAPCHAIN_IMAGES     4u

/* W3b.5 surface (declared early because venus_instance embeds an array
 * of them). */
struct venus_surface {
    uint32_t in_use;
    uint32_t shm_handle;               /* compositor target SHM handle */
    uint32_t width;
    uint32_t height;
    void    *target_ptr;               /* mapped compositor target */
};

struct venus_instance {
    VK_LOADER_DATA loader_data;
    uint32_t       caps;          /* SYS_GPU_CAPS snapshot */
    int32_t        ctx_id;        /* kernel GPU ctx, 0 if none */
    struct venus_wire *wire;      /* guest-side ring wrapper (W3b.1) */
    uint64_t       host_handle;   /* host VkInstance handle-id (W3b.1) */
    /* W3b.5 — surface slot table (guest-local). */
    struct venus_surface surfaces[VENUS_MAX_SURFACE_OBJECTS];
};

/* W3b.4 object tables. */
#define VENUS_MAX_SHADER_OBJECTS      512u
#define VENUS_MAX_RP_OBJECTS          256u
#define VENUS_MAX_IMAGE_OBJECTS       512u
#define VENUS_MAX_IMAGE_VIEW_OBJECTS  512u
#define VENUS_MAX_BUFFER_VIEW_OBJECTS 1024u
#define VENUS_MAX_SAMPLER_OBJECTS     256u
#define VENUS_MAX_FB_OBJECTS          256u
#define VENUS_MAX_DESC_LAYOUT_OBJECTS 512u
#define VENUS_MAX_DESC_POOL_OBJECTS   256u
#define VENUS_MAX_DESC_SET_OBJECTS    1024u
#define VENUS_MAX_DESC_TPL_OBJECTS    512u
#define VENUS_MAX_PL_LAYOUT_OBJECTS   512u
#define VENUS_MAX_PIPELINE_OBJECTS    1024u
#define VENUS_MAX_CMD_POOL_OBJECTS     64u
#define VENUS_MAX_CMD_BUFFER_OBJECTS  256u

struct venus_memory {
    uint64_t host_id;       /* host VkDeviceMemory id (0 if guest-local fallback) */
    uint64_t size;
    void    *local_ptr;     /* guest-side backing buffer (malloc'd OR SHM-mapped) */
    uint32_t type_index;
    uint32_t in_use;
    uint32_t mapped;
    uint32_t is_shm_backed; /* W3b.5: 1 when local_ptr points to mapped SHM */
    uint32_t shm_handle;    /* W3b.5: SHM handle (0 if none) */
    uint32_t shm_width;     /* W3b.5: width of the SHM image */
    uint32_t shm_height;    /* W3b.5: height of the SHM image */
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
    uint32_t usage;                /* W3b.5: cached VkImageUsageFlags */
    uint32_t is_swapchain_owned;   /* W3b.5: 1 if owned by a VkSwapchain */
    uint64_t bound_offset;
};

struct venus_image_view {
    uint64_t host_id;
    uint32_t in_use;
    int32_t  image_slot;
};

struct venus_buffer_view {
    uint64_t host_id;
    uint32_t in_use;
    int32_t  buffer_slot;
    uint32_t format;
    uint32_t _pad;
    uint64_t offset;
    uint64_t range;
};

struct venus_sampler {
    uint64_t host_id;
    uint32_t in_use;
};

struct venus_framebuffer {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t width;
    uint32_t height;
    /* W3b.6 — first color attachment image slot, used by the CPU-fallback
     * rasterizer to find which framebuffer was last drawn to. -1 if none. */
    int32_t  first_color_image_slot;
};

struct venus_descriptor_set_layout {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t binding_count;
};

struct venus_descriptor_pool {
    uint32_t in_use;
    uint32_t max_sets;
    uint32_t alloc_count;
    uint32_t _pad;
};

struct venus_descriptor_set {
    uint32_t in_use;
    int32_t  pool_slot;
    int32_t  layout_slot;
    int32_t  image_slot;      /* first sampled/storage/input image written */
};

struct venus_descriptor_update_template {
    uint32_t in_use;
    uint32_t entry_count;
    uint32_t template_type;
    uint32_t _pad;
};

struct venus_pipeline_layout {
    uint64_t host_id;
    uint32_t in_use;
    uint32_t set_layout_count;
    uint32_t push_constant_range_count;
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
    /* W3b.6 — guest-local recording state for the CPU-fallback rasterizer.
     * Reset by venus_BeginCommandBuffer; -1 sentinels mean "unbound". */
    int32_t  recorded_vb_slot;         /* venus_buffer slot at binding 0; -1 = none */
    uint64_t recorded_vb_offset;       /* offset within the bound buffer */
    uint32_t recorded_vb_stride;       /* bytes per vertex (defaults to 12 for vec3) */
    uint32_t recorded_vertex_count;    /* last CmdDraw vertexCount */
    uint32_t recorded_first_vertex;    /* last CmdDraw firstVertex */
    int32_t  last_drawn_image_slot;    /* venus_image slot last bound via render pass; -1 */
    uint32_t drew_flag;                /* 1 once CmdDraw recorded inside the active RP */
    /* W4.8 — clear-only fast path. CmdBeginRenderPass with LOAD_OP_CLEAR
     * records the clear value here; CmdClearColorImage also writes here.
     * QueueSubmit fills SHM-backed memory bound to last_drawn_image_slot
     * (or recorded_clear_image_slot if set). */
    uint32_t recorded_clear_color;     /* BGRA8 packed (B=lo,G,R,A=hi); 0 = none */
    uint32_t recorded_has_clear;       /* 1 if recorded_clear_color is valid */
    int32_t  recorded_clear_image_slot;/* venus_image slot for vkCmdClearColorImage; -1 */
    uint32_t recorded_has_copy_image;  /* 1 if vkCmdCopyImage was recorded */
    int32_t  recorded_copy_src_image_slot;
    int32_t  recorded_copy_dst_image_slot;
    uint32_t recorded_has_copy_buffer_to_image;
    int32_t  recorded_copy_src_buffer_slot;
    int32_t  recorded_copy_buffer_dst_image_slot;
    int32_t  recorded_sampled_image_slot;
    uint32_t _pad_sampled;
    uint64_t recorded_copy_buffer_offset;
    uint32_t recorded_copy_buffer_width;
    uint32_t recorded_copy_buffer_height;
    uint32_t recorded_copy_buffer_row_length;
    uint32_t _pad6;
};

/* W3b.5 — WSI + sync primitives (queue/fence/sema/swapchain; the
 * surface struct is declared earlier for the venus_instance table). */
struct venus_queue {
    VK_LOADER_DATA       loader_data;  /* dispatchable — VkQueue is dispatchable */
    struct venus_device *owner;
    uint32_t             in_use;
    uint32_t             queue_family_index;
    uint32_t             queue_index;
    uint32_t             _pad;
};

struct venus_fence {
    uint32_t in_use;
    uint32_t signaled;
};

struct venus_semaphore {
    uint32_t in_use;
    uint32_t signaled;
};

struct venus_swapchain {
    uint32_t in_use;
    int32_t  surface_slot;
    uint32_t image_count;
    uint32_t current_index;
    int32_t  image_slots[VENUS_MAX_SWAPCHAIN_IMAGES];   /* slot of venus_image */
    int32_t  memory_slots[VENUS_MAX_SWAPCHAIN_IMAGES];  /* slot of venus_memory backing */
    uint32_t width;
    uint32_t height;
    uint32_t format;
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
    struct venus_buffer_view     buffer_views  [VENUS_MAX_BUFFER_VIEW_OBJECTS];
    struct venus_sampler         samplers      [VENUS_MAX_SAMPLER_OBJECTS];
    struct venus_framebuffer     framebuffers  [VENUS_MAX_FB_OBJECTS];
    struct venus_descriptor_set_layout desc_layouts [VENUS_MAX_DESC_LAYOUT_OBJECTS];
    struct venus_descriptor_pool       desc_pools   [VENUS_MAX_DESC_POOL_OBJECTS];
    struct venus_descriptor_set        desc_sets    [VENUS_MAX_DESC_SET_OBJECTS];
    struct venus_descriptor_update_template desc_templates [VENUS_MAX_DESC_TPL_OBJECTS];
    struct venus_pipeline_layout pl_layouts    [VENUS_MAX_PL_LAYOUT_OBJECTS];
    struct venus_pipeline        pipelines     [VENUS_MAX_PIPELINE_OBJECTS];
    struct venus_cmd_pool        cmd_pools     [VENUS_MAX_CMD_POOL_OBJECTS];
    struct venus_cmd_buffer      cmd_buffers   [VENUS_MAX_CMD_BUFFER_OBJECTS];
    /* W3b.5 slot tables. */
    struct venus_queue           queues        [VENUS_MAX_QUEUE_OBJECTS];
    struct venus_fence           fences        [VENUS_MAX_FENCE_OBJECTS];
    struct venus_semaphore       semaphores    [VENUS_MAX_SEMA_OBJECTS];
    struct venus_swapchain       swapchains    [VENUS_MAX_SWAPCHAIN_OBJECTS];
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
venus_CreateBufferView(VkDevice, const VkBufferViewCreateInfo *,
                       const VkAllocationCallbacks *, VkBufferView *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyBufferView(VkDevice, VkBufferView, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateSampler(VkDevice, const VkSamplerCreateInfo *,
                    const VkAllocationCallbacks *, VkSampler *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *,
                        const VkAllocationCallbacks *, VkFramebuffer *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *,
                                const VkAllocationCallbacks *, VkDescriptorSetLayout *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout,
                                 const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *,
                           const VkAllocationCallbacks *, VkDescriptorPool *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *,
                             VkDescriptorSet *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_FreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t,
                         const VkDescriptorSet *);
VKAPI_ATTR void VKAPI_CALL
venus_UpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *,
                           uint32_t, const VkCopyDescriptorSet *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint,
                            VkPipelineLayout, uint32_t, uint32_t,
                            const VkDescriptorSet *, uint32_t,
                            const uint32_t *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDescriptorUpdateTemplate(VkDevice,
        const VkDescriptorUpdateTemplateCreateInfo *,
        const VkAllocationCallbacks *, VkDescriptorUpdateTemplate *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyDescriptorUpdateTemplate(VkDevice, VkDescriptorUpdateTemplate,
                                      const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL
venus_UpdateDescriptorSetWithTemplate(VkDevice, VkDescriptorSet,
                                      VkDescriptorUpdateTemplate, const void *);

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
venus_ResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);

VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *,
                             VkCommandBuffer *);
VKAPI_ATTR void VKAPI_CALL
venus_FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_BeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_EndCommandBuffer(VkCommandBuffer);
VKAPI_ATTR VkResult VKAPI_CALL
venus_ResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);

VKAPI_ATTR void VKAPI_CALL
venus_CmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL
venus_CmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBeginRendering(VkCommandBuffer, const VkRenderingInfo *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdEndRendering(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t,
                     int32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndexedIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize,
                             uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize,
                      uint32_t, uint32_t);

/* W3b.6 — vertex input + dynamic state. */
VKAPI_ATTR void VKAPI_CALL
venus_CmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t,
                           const VkBuffer *, const VkDeviceSize *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBindVertexBuffers2(VkCommandBuffer, uint32_t, uint32_t,
                            const VkBuffer *, const VkDeviceSize *,
                            const VkDeviceSize *, const VkDeviceSize *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdSetViewport(VkCommandBuffer, uint32_t, uint32_t,
                     const VkViewport *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdSetViewportWithCount(VkCommandBuffer, uint32_t,
                              const VkViewport *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdSetScissor(VkCommandBuffer, uint32_t, uint32_t,
                    const VkRect2D *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdSetScissorWithCount(VkCommandBuffer, uint32_t,
                             const VkRect2D *);

/* W4.8 — clear-only fast path. */
VKAPI_ATTR void VKAPI_CALL
venus_CmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout,
                         const VkClearColorValue *, uint32_t,
                         const VkImageSubresourceRange *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout,
                                const VkClearDepthStencilValue *, uint32_t,
                                const VkImageSubresourceRange *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdClearAttachments(VkCommandBuffer, uint32_t,
                          const VkClearAttachment *, uint32_t,
                          const VkClearRect *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t,
                    const VkBufferCopy *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBuffer2(VkCommandBuffer, const VkCopyBufferInfo2 *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage,
                           VkImageLayout, uint32_t,
                           const VkBufferImageCopy *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyBufferToImage2(VkCommandBuffer,
                            const VkCopyBufferToImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                   VkImageLayout, uint32_t, const VkImageCopy *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                   VkImageLayout, uint32_t, const VkImageBlit *, VkFilter);
VKAPI_ATTR void VKAPI_CALL
venus_CmdResolveImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                      VkImageLayout, uint32_t, const VkImageResolve *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImage2(VkCommandBuffer, const VkCopyImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdBlitImage2(VkCommandBuffer, const VkBlitImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdResolveImage2(VkCommandBuffer, const VkResolveImageInfo2 *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer,
                           uint32_t, const VkBufferImageCopy *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdCopyImageToBuffer2(VkCommandBuffer,
                            const VkCopyImageToBufferInfo2 *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags,
                         VkPipelineStageFlags, VkDependencyFlags,
                         uint32_t, const VkMemoryBarrier *,
                         uint32_t, const VkBufferMemoryBarrier *,
                         uint32_t, const VkImageMemoryBarrier *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdPushConstants(VkCommandBuffer, VkPipelineLayout,
                       VkShaderStageFlags, uint32_t, uint32_t,
                       const void *);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize,
                           VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndexedIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize,
                                  VkBuffer, VkDeviceSize, uint32_t,
                                  uint32_t);

/* W3b.5 — WSI + surface + swapchain + queue + sync + present. */

/* Surface (instance-scoped, OsitoK-specific). */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateOsitokCompositorSurfaceKHR(VkInstance,
        const VkOsitoCompositorSurfaceCreateInfoOSITOK *,
        const VkAllocationCallbacks *, VkSurfaceKHR *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroySurfaceKHR(VkInstance, VkSurfaceKHR,
                        const VkAllocationCallbacks *);

/* Queue + device-wait. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_QueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL
venus_QueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2 *, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL
venus_QueueSubmit2KHR(VkQueue, uint32_t, const VkSubmitInfo2 *, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL
venus_QueueWaitIdle(VkQueue);
VKAPI_ATTR VkResult VKAPI_CALL
venus_DeviceWaitIdle(VkDevice);

/* Fence. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateFence(VkDevice, const VkFenceCreateInfo *,
                  const VkAllocationCallbacks *, VkFence *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_ResetFences(VkDevice, uint32_t, const VkFence *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_WaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL
venus_GetFenceStatus(VkDevice, VkFence);

/* Semaphore. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *,
                      const VkAllocationCallbacks *, VkSemaphore *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *);

/* Swapchain. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *,
                         const VkAllocationCallbacks *, VkSwapchainKHR *);
VKAPI_ATTR void VKAPI_CALL
venus_DestroySwapchainKHR(VkDevice, VkSwapchainKHR,
                          const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_AcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore,
                          VkFence, uint32_t *);

/* Present. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_QueuePresentKHR(VkQueue, const VkPresentInfoKHR *);

/* Physical-device surface queries. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfaceFormatsKHR(
        VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfacePresentModesKHR(
        VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *);
VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfaceSupportKHR(
        VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance, const char *);

#endif
