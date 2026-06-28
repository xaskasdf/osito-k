/*
 * venus_w3b4_objects.c — W3b.4 object lifecycle and command recording
 * entry points for the venus ICD.
 *
 * Behavior: same dual-path model as W3b.3. Every entry tries the venus
 * wire round-trip first (when the wire is live and the parent device
 * has a valid host handle); on failure/absence the call falls back to a
 * guest-local implementation so the hello-pipeline smoke succeeds even
 * on --no-gl boots.
 *
 * Handle layout (64-bit non-dispatchable):
 *   bit 63     : marker (always 1 for venus ICD handles)
 *   bits 62:60 : object-type tag (3 bits)
 *   bits 59:48 : slot index (12 bits)
 *   bits 47:0  : owning device pointer (canonical low 48 bits)
 *
 * Tag values (per-object-type markers — extended from W3b.3's top-2-bit
 * scheme to 3 tag bits so each W3b.4 object kind has a distinct marker):
 *   0x8 (100b)  memory        (W3b.3)   — top2=10 (preserved for compat)
 *   0xC (110b)  buffer        (W3b.3)   — top2=11 (preserved for compat)
 *   0xA (101b)  shader module (W3b.4)
 *   0x9 (1001b—using full 4-bit marker 0x9000) render pass (W3b.4)
 *   0xE         image         (W3b.4)
 *   0xD         image view    (W3b.4)
 *   0x4         buffer view   (W4.9 guest-local typed buffer view)
 *   0xB         framebuffer   (W3b.4)
 *   0xF         pipeline layout (W3b.4)
 *   0x7         pipeline      (W3b.4)   (re-uses bit63=0 which is fine
 *                                        for non-dispatchable since nothing
 *                                        else uses top2=01)
 *   0x6         cmd pool      (W3b.4)
 *
 * Command buffers are dispatchable — they don't share this scheme.
 *
 * Slot decode ALWAYS masks with VENUS_H_SLOT_MASK (0x0FFF) so marker bits
 * don't leak into the slot index. This was the W3b.3-fix lesson.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int printf(const char *, ...);

/* --- Encoder forward decls --- */
extern int venus_cmd_encode_CreateShaderModule(struct venus_wire *, uint64_t,
        const VkShaderModuleCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyShaderModule(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateRenderPass(struct venus_wire *, uint64_t,
        const VkRenderPassCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyRenderPass(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateImage(struct venus_wire *, uint64_t,
        const VkImageCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyImage(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateImageView(struct venus_wire *, uint64_t, uint64_t,
        const VkImageViewCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyImageView(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_GetImageMemoryRequirements(struct venus_wire *,
        uint64_t, uint64_t, VkMemoryRequirements *);
extern int venus_cmd_encode_BindImageMemory(struct venus_wire *, uint64_t,
        uint64_t, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateFramebuffer(struct venus_wire *, uint64_t,
        uint64_t, const VkFramebufferCreateInfo *, const uint64_t *, uint32_t,
        uint64_t *);
extern int venus_cmd_encode_DestroyFramebuffer(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateSampler(struct venus_wire *, uint64_t,
        const VkSamplerCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroySampler(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateDescriptorSetLayout(struct venus_wire *, uint64_t,
        const VkDescriptorSetLayoutCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyDescriptorSetLayout(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateDescriptorPool(struct venus_wire *, uint64_t,
        const VkDescriptorPoolCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyDescriptorPool(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_AllocateDescriptorSets(struct venus_wire *, uint64_t,
        uint64_t, uint32_t, const uint64_t *, uint64_t *);
extern int venus_cmd_encode_FreeDescriptorSets(struct venus_wire *, uint64_t,
        uint64_t, uint32_t, const uint64_t *);
extern int venus_cmd_encode_UpdateDescriptorSets(struct venus_wire *,
        struct venus_device *, uint32_t, const VkWriteDescriptorSet *,
        uint32_t, const VkCopyDescriptorSet *);
extern int venus_cmd_encode_CreatePipelineLayout(struct venus_wire *, uint64_t,
        const VkPipelineLayoutCreateInfo *, const uint64_t *, uint32_t, uint64_t *);
extern int venus_cmd_encode_DestroyPipelineLayout(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateGraphicsPipelines(struct venus_wire *,
        uint64_t, uint64_t, uint32_t, const VkGraphicsPipelineCreateInfo *,
        uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);
extern int venus_cmd_encode_DestroyPipeline(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateCommandPool(struct venus_wire *, uint64_t,
        const VkCommandPoolCreateInfo *, uint64_t *);
extern int venus_cmd_encode_DestroyCommandPool(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_AllocateCommandBuffers(struct venus_wire *, uint64_t,
        uint64_t, uint32_t, uint32_t, uint64_t *);
extern int venus_cmd_encode_FreeCommandBuffers(struct venus_wire *, uint64_t,
        uint64_t, uint32_t, const uint64_t *);
extern int venus_cmd_encode_BeginCommandBuffer(struct venus_wire *, uint64_t,
        uint64_t, uint32_t);
extern int venus_cmd_encode_EndCommandBuffer(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CmdBeginRenderPass(struct venus_wire *, uint64_t,
        uint64_t, uint64_t, uint64_t, int32_t, int32_t, uint32_t, uint32_t,
        uint32_t, const VkClearValue *, uint32_t);
extern int venus_cmd_encode_CmdEndRenderPass(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CmdBeginRendering(struct venus_wire *,
        struct venus_device *, uint64_t, const VkRenderingInfo *);
extern int venus_cmd_encode_CmdEndRendering(struct venus_wire *, uint64_t);
extern int venus_cmd_encode_CmdBindPipeline(struct venus_wire *, uint64_t,
        uint64_t, uint32_t, uint64_t);
extern int venus_cmd_encode_CmdBindDescriptorSets(struct venus_wire *,
        struct venus_device *, uint64_t, uint32_t, VkPipelineLayout, uint32_t,
        uint32_t, const VkDescriptorSet *, uint32_t, const uint32_t *);
extern int venus_cmd_encode_CmdDraw(struct venus_wire *, uint64_t, uint64_t,
        uint32_t, uint32_t, uint32_t, uint32_t);

/* --- Handle markers (top 4 bits) --- */
#define VENUS_H_MARKER_SHADER       0xA000000000000000ull
#define VENUS_H_MARKER_RP           0x9000000000000000ull
#define VENUS_H_MARKER_IMAGE        0xE000000000000000ull
#define VENUS_H_MARKER_IMGVIEW      0xD000000000000000ull
#define VENUS_H_MARKER_BUFVIEW      0x4000000000000000ull
#define VENUS_H_MARKER_SAMPLER      0x5000000000000000ull
#define VENUS_H_MARKER_DPOOL        0x1000000000000000ull
#define VENUS_H_MARKER_DSL          0x2000000000000000ull
#define VENUS_H_MARKER_DSET         0x3000000000000000ull
#define VENUS_H_MARKER_DTEMPLATE    0x0000000000000000ull
#define VENUS_H_MARKER_FB           0xB000000000000000ull
#define VENUS_H_MARKER_PLLAYOUT     0xF000000000000000ull
#define VENUS_H_MARKER_PIPELINE     0x7000000000000000ull
#define VENUS_H_MARKER_CMDPOOL      0x6000000000000000ull
#define VENUS_H_SLOT_MASK_W3B4      0x0FFFull
#define VENUS_H_PTR_MASK_W3B4       0x0000FFFFFFFFFFFFull

#define MAKE_SLOT_HANDLE(dev, slot, marker)                                  \
    ((uint64_t)((((uint64_t)(uint32_t)(slot)) & VENUS_H_SLOT_MASK_W3B4) << 48 \
              | ((uint64_t)(uintptr_t)(dev) & VENUS_H_PTR_MASK_W3B4)         \
              | (marker)))

#define HANDLE_TO_SLOT(h)  ((int)(((uint64_t)(h) >> 48) & VENUS_H_SLOT_MASK_W3B4))

static uint32_t venus_w3b4_logged_draw;
static uint32_t venus_w3b4_logged_desc_image;
static uint32_t venus_w3b4_logged_desc_bind;
static uint32_t venus_w3b4_logged_desc_template;
static uint32_t venus_w3b4_logged_rt_begin;
static uint32_t venus_w3b4_log_image_create_diag;
static uint32_t venus_w3b4_logged_pipeline_create;
static uint32_t venus_w3b4_logged_bad_pipeline_bind;
static uint32_t venus_w3b4_logged_shader_create;
static uint32_t venus_w3b4_logged_dsl_create;
static uint32_t venus_w3b4_logged_pl_create;

static int venus_desc_type_has_image(uint32_t type) {
    return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

static int venus_desc_set_image_from_view(struct venus_device *dev,
                                          struct venus_descriptor_set *set,
                                          VkImageView view) {
    if (!dev || !set || !view) return 0;
    int view_slot = HANDLE_TO_SLOT(view);
    if (view_slot < 0 || view_slot >= (int)VENUS_MAX_IMAGE_VIEW_OBJECTS)
        return 0;
    if (!dev->image_views[view_slot].in_use)
        return 0;
    if (dev->image_views[view_slot].image_slot < 0)
        return 0;
    set->image_slot = dev->image_views[view_slot].image_slot;
    return 1;
}

/* --- Slot allocators (uniform shape). --- */
#define DEFINE_SLOT_ALLOC(name, table, cap)                                 \
static int name##_slot_alloc(struct venus_device *dev) {                    \
    for (uint32_t i = 0; i < (cap); i++)                                    \
        if (!dev->table[i].in_use) { dev->table[i].in_use = 1; return (int)i; } \
    return -1;                                                              \
}

DEFINE_SLOT_ALLOC(shader,   shaders,       VENUS_MAX_SHADER_OBJECTS)
DEFINE_SLOT_ALLOC(rp,       render_passes, VENUS_MAX_RP_OBJECTS)
DEFINE_SLOT_ALLOC(image,    images,        VENUS_MAX_IMAGE_OBJECTS)
DEFINE_SLOT_ALLOC(imgview,  image_views,   VENUS_MAX_IMAGE_VIEW_OBJECTS)
DEFINE_SLOT_ALLOC(bufview,  buffer_views,  VENUS_MAX_BUFFER_VIEW_OBJECTS)
DEFINE_SLOT_ALLOC(sampler,  samplers,      VENUS_MAX_SAMPLER_OBJECTS)
DEFINE_SLOT_ALLOC(fb,       framebuffers,  VENUS_MAX_FB_OBJECTS)
DEFINE_SLOT_ALLOC(dsl,      desc_layouts,  VENUS_MAX_DESC_LAYOUT_OBJECTS)
DEFINE_SLOT_ALLOC(dpool,    desc_pools,    VENUS_MAX_DESC_POOL_OBJECTS)
DEFINE_SLOT_ALLOC(dset,     desc_sets,     VENUS_MAX_DESC_SET_OBJECTS)
DEFINE_SLOT_ALLOC(dtpl,     desc_templates, VENUS_MAX_DESC_TPL_OBJECTS)
DEFINE_SLOT_ALLOC(pllayout, pl_layouts,    VENUS_MAX_PL_LAYOUT_OBJECTS)
DEFINE_SLOT_ALLOC(pipeline, pipelines,     VENUS_MAX_PIPELINE_OBJECTS)
DEFINE_SLOT_ALLOC(cmdpool,  cmd_pools,     VENUS_MAX_CMD_POOL_OBJECTS)
DEFINE_SLOT_ALLOC(cmdbuf,   cmd_buffers,   VENUS_MAX_CMD_BUFFER_OBJECTS)

/* Command buffers are dispatchable — they carry VK_LOADER_DATA and the app
 * passes the pointer back verbatim. We still need a slot to key the host
 * id. Allocate per-device and return pointer into the in-place slot. */
static inline VkCommandBuffer cb_slot_to_handle(struct venus_device *dev, int slot) {
    return (VkCommandBuffer)&dev->cmd_buffers[slot];
}
static inline int cb_handle_to_slot(struct venus_device *dev, VkCommandBuffer cb) {
    uintptr_t delta = (uintptr_t)cb - (uintptr_t)&dev->cmd_buffers[0];
    if (delta >= VENUS_MAX_CMD_BUFFER_OBJECTS * sizeof(dev->cmd_buffers[0])) return -1;
    return (int)(delta / sizeof(dev->cmd_buffers[0]));
}

/* --- Shader Module --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateShaderModule(VkDevice device,
                         const VkShaderModuleCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkShaderModule *pShader) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pShader) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = shader_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_shader *s = &dev->shaders[slot];
    s->host_id   = 0;
    s->code_size = (uint32_t)pCreateInfo->codeSize;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateShaderModule(dev->parent->wire,
                                                     dev->host_handle,
                                                     pCreateInfo, &host_id);
        if (venus_w3b4_logged_shader_create < 32u) {
            venus_w3b4_logged_shader_create++;
            printf("[VSHADER] create rc=%d host=%llu code=%u\n",
                   rc, (unsigned long long)host_id, s->code_size);
        }
        if (rc == 0 && host_id != 0) s->host_id = host_id;
        else {
            memset(s, 0, sizeof(*s));
            return rc == 0 ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)rc;
        }
    }
    *pShader = (VkShaderModule)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_SHADER);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyShaderModule(VkDevice device, VkShaderModule shader,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !shader) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(shader);
    if (slot < 0 || slot >= (int)VENUS_MAX_SHADER_OBJECTS) return;
    struct venus_shader *s = &dev->shaders[slot];
    if (!s->in_use) return;
    if (dev->parent && dev->parent->wire && s->host_id != 0)
        (void)venus_cmd_encode_DestroyShaderModule(dev->parent->wire,
                                                   dev->host_handle, s->host_id);
    memset(s, 0, sizeof(*s));
}

/* --- Render Pass --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateRenderPass(VkDevice device,
                       const VkRenderPassCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkRenderPass *pRP) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pRP) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = rp_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_render_pass *r = &dev->render_passes[slot];
    r->host_id          = 0;
    r->attachment_count = pCreateInfo->attachmentCount;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateRenderPass(dev->parent->wire,
                                                   dev->host_handle,
                                                   pCreateInfo, &host_id);
        if (rc == 0 && host_id != 0) r->host_id = host_id;
    }
    *pRP = (VkRenderPass)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_RP);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyRenderPass(VkDevice device, VkRenderPass rp,
                        const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !rp) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(rp);
    if (slot < 0 || slot >= (int)VENUS_MAX_RP_OBJECTS) return;
    struct venus_render_pass *r = &dev->render_passes[slot];
    if (!r->in_use) return;
    if (dev->parent && dev->parent->wire && r->host_id != 0)
        (void)venus_cmd_encode_DestroyRenderPass(dev->parent->wire,
                                                 dev->host_handle, r->host_id);
    memset(r, 0, sizeof(*r));
}

/* --- Image --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pImage) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = image_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_image *img = &dev->images[slot];
    img->host_id        = 0;
    img->width          = pCreateInfo->extent.width;
    img->height         = pCreateInfo->extent.height;
    img->format         = (uint32_t)pCreateInfo->format;
    img->bound_mem_slot = -1;
    img->usage          = pCreateInfo->usage;
    img->is_swapchain_owned = 0;
    img->bound_offset   = 0;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateImage(dev->parent->wire,
                                              dev->host_handle,
                                              pCreateInfo, &host_id);
        if (rc == 0 && host_id != 0) img->host_id = host_id;
    }
    if (venus_w3b4_log_image_create_diag < 32u) {
        venus_w3b4_log_image_create_diag++;
        printf("[VIMGD] create slot=%d %ux%u fmt=%u usage=0x%x host=%llu\n",
               slot, img->width, img->height, img->format, img->usage,
               (unsigned long long)img->host_id);
    }
    *pImage = (VkImage)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_IMAGE);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyImage(VkDevice device, VkImage image,
                   const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !image) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(image);
    if (slot < 0 || slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    struct venus_image *img = &dev->images[slot];
    if (!img->in_use) return;
    if (dev->parent && dev->parent->wire && img->host_id != 0)
        (void)venus_cmd_encode_DestroyImage(dev->parent->wire,
                                            dev->host_handle, img->host_id);
    memset(img, 0, sizeof(*img));
}

VKAPI_ATTR void VKAPI_CALL
venus_GetImageMemoryRequirements(VkDevice device, VkImage image,
                                 VkMemoryRequirements *pReqs) {
    if (!device || !image || !pReqs) return;
    memset(pReqs, 0, sizeof(*pReqs));
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(image);
    if (slot < 0 || slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    struct venus_image *img = &dev->images[slot];
    if (!img->in_use) return;

    if (dev->parent && dev->parent->wire && img->host_id != 0) {
        int rc = venus_cmd_encode_GetImageMemoryRequirements(
                dev->parent->wire, dev->host_handle, img->host_id, pReqs);
        if (rc == 0 && pReqs->size != 0 &&
            pReqs->alignment != 0 &&
            pReqs->memoryTypeBits != 0)
            return;
    }
    /* Guest-local fallback: assume RGBA8 4bpp, 256-byte alignment. */
    uint64_t bpp = 4u;
    pReqs->size           = (uint64_t)img->width * img->height * bpp;
    if (!pReqs->size) pReqs->size = 256u;
    pReqs->alignment      = 256u;
    pReqs->memoryTypeBits = 0xFFFFFFFFu;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                      VkDeviceSize memoryOffset) {
    if (!device || !image || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int islot = HANDLE_TO_SLOT(image);
    int mslot = (int)((((uint64_t)memory) >> 48) & VENUS_H_SLOT_MASK_W3B4);
    if (islot < 0 || islot >= (int)VENUS_MAX_IMAGE_OBJECTS) return VK_ERROR_INITIALIZATION_FAILED;
    if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_image  *img = &dev->images[islot];
    struct venus_memory *m   = &dev->memories[mslot];
    if (!img->in_use || !m->in_use) return VK_ERROR_INITIALIZATION_FAILED;

    img->bound_mem_slot = mslot;
    img->bound_offset   = (uint64_t)memoryOffset;

    if (dev->parent && dev->parent->wire && img->host_id != 0 && m->host_id != 0) {
        int rc = venus_cmd_encode_BindImageMemory(dev->parent->wire,
                                                  dev->host_handle,
                                                  img->host_id, m->host_id,
                                                  (uint64_t)memoryOffset);
        if (rc < 0) return VK_ERROR_DEVICE_LOST;
        if (rc != 0) return (VkResult)rc;
    }
    return VK_SUCCESS;
}

/* --- Image View --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateImageView(VkDevice device,
                      const VkImageViewCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkImageView *pView) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pView) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = imgview_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_image_view *iv = &dev->image_views[slot];
    /* Resolve image slot -> host id (for the encoder). */
    int islot = HANDLE_TO_SLOT(pCreateInfo->image);
    uint64_t image_host_id = 0;
    if (islot >= 0 && islot < (int)VENUS_MAX_IMAGE_OBJECTS &&
        dev->images[islot].in_use) {
        image_host_id = dev->images[islot].host_id;
        iv->image_slot = islot;
    } else {
        iv->image_slot = -1;
    }

    iv->host_id = 0;
    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateImageView(dev->parent->wire,
                                                  dev->host_handle,
                                                  image_host_id,
                                                  pCreateInfo, &host_id);
        if (rc == 0 && host_id != 0) iv->host_id = host_id;
    }
    *pView = (VkImageView)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_IMGVIEW);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyImageView(VkDevice device, VkImageView view,
                       const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !view) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(view);
    if (slot < 0 || slot >= (int)VENUS_MAX_IMAGE_VIEW_OBJECTS) return;
    struct venus_image_view *iv = &dev->image_views[slot];
    if (!iv->in_use) return;
    if (dev->parent && dev->parent->wire && iv->host_id != 0)
        (void)venus_cmd_encode_DestroyImageView(dev->parent->wire,
                                                dev->host_handle, iv->host_id);
    memset(iv, 0, sizeof(*iv));
}

/* --- Buffer View ---
 * Guest-local typed buffer views unblock DXVK's raw/structured buffer SRV/UAV
 * setup while the venus wire encoder only covers image views. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateBufferView(VkDevice device,
                       const VkBufferViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkBufferView *pView) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pCreateInfo->buffer || !pView)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int bslot = HANDLE_TO_SLOT(pCreateInfo->buffer);
    if (bslot < 0 || bslot >= (int)VENUS_MAX_BUF_OBJECTS ||
        !dev->buffers[bslot].in_use)
        return VK_ERROR_INITIALIZATION_FAILED;

    int slot = bufview_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_buffer_view *bv = &dev->buffer_views[slot];
    bv->host_id     = 0;
    bv->buffer_slot = bslot;
    bv->format      = (uint32_t)pCreateInfo->format;
    bv->offset      = (uint64_t)pCreateInfo->offset;
    bv->range       = (uint64_t)pCreateInfo->range;

    *pView = (VkBufferView)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_BUFVIEW);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyBufferView(VkDevice device, VkBufferView view,
                        const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !view) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(view);
    if (slot < 0 || slot >= (int)VENUS_MAX_BUFFER_VIEW_OBJECTS) return;
    struct venus_buffer_view *bv = &dev->buffer_views[slot];
    if (!bv->in_use) return;
    memset(bv, 0, sizeof(*bv));
}

/* --- Sampler ---
 * Guest-local object, same shape as the other W3b objects. DXVK's DxvkSampler
 * (and the meta-blit/present path) needs a valid VkSampler handle to proceed;
 * there is no host-side venus sampler encoder yet, so this is purely a tracked
 * slot handle. When a real host backend is wired up, add an optional
 * venus_cmd_encode_CreateSampler round-trip here exactly like CreateImageView. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateSampler(VkDevice device,
                    const VkSamplerCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkSampler *pSampler) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pSampler) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = sampler_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_sampler *s = &dev->samplers[slot];
    s->host_id = 0;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateSampler(dev->parent->wire,
                                                dev->host_handle,
                                                pCreateInfo, &host_id);
        if (rc == VK_SUCCESS && host_id != 0)
            s->host_id = host_id;
        else {
            memset(s, 0, sizeof(*s));
            return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)rc;
        }
    }

    *pSampler = (VkSampler)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_SAMPLER);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroySampler(VkDevice device, VkSampler sampler,
                     const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !sampler) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(sampler);
    if (slot < 0 || slot >= (int)VENUS_MAX_SAMPLER_OBJECTS) return;
    struct venus_sampler *s = &dev->samplers[slot];
    if (!s->in_use) return;
    if (dev->parent && dev->parent->wire && s->host_id != 0)
        (void)venus_cmd_encode_DestroySampler(dev->parent->wire,
                                              dev->host_handle, s->host_id);
    memset(s, 0, sizeof(*s));
}

/* --- Framebuffer --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateFramebuffer(VkDevice device,
                        const VkFramebufferCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkFramebuffer *pFB) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pFB) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = fb_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_framebuffer *f = &dev->framebuffers[slot];
    f->host_id = 0;
    f->width   = pCreateInfo->width;
    f->height  = pCreateInfo->height;
    f->first_color_image_slot = -1;     /* W3b.6 — set below if any attachment */

    /* Resolve rp + view host ids. */
    int rpslot = HANDLE_TO_SLOT(pCreateInfo->renderPass);
    uint64_t rp_host = 0;
    if (rpslot >= 0 && rpslot < (int)VENUS_MAX_RP_OBJECTS &&
        dev->render_passes[rpslot].in_use)
        rp_host = dev->render_passes[rpslot].host_id;

    uint64_t view_ids[VENUS_MAX_IMAGE_VIEW_OBJECTS];
    uint32_t n = pCreateInfo->attachmentCount;
    if (n > VENUS_MAX_IMAGE_VIEW_OBJECTS) n = VENUS_MAX_IMAGE_VIEW_OBJECTS;
    for (uint32_t i = 0; i < n; i++) {
        int vs = HANDLE_TO_SLOT(pCreateInfo->pAttachments[i]);
        if (vs >= 0 && vs < (int)VENUS_MAX_IMAGE_VIEW_OBJECTS &&
            dev->image_views[vs].in_use) {
            view_ids[i] = dev->image_views[vs].host_id;
            /* W3b.6 — first color attachment: record image slot. */
            if (i == 0)
                f->first_color_image_slot = dev->image_views[vs].image_slot;
        } else {
            view_ids[i] = 0ull;
        }
    }

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateFramebuffer(dev->parent->wire,
                                                    dev->host_handle, rp_host,
                                                    pCreateInfo, view_ids, n,
                                                    &host_id);
        if (rc == 0 && host_id != 0) f->host_id = host_id;
    }
    *pFB = (VkFramebuffer)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_FB);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyFramebuffer(VkDevice device, VkFramebuffer fb,
                         const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !fb) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(fb);
    if (slot < 0 || slot >= (int)VENUS_MAX_FB_OBJECTS) return;
    struct venus_framebuffer *f = &dev->framebuffers[slot];
    if (!f->in_use) return;
    if (dev->parent && dev->parent->wire && f->host_id != 0)
        (void)venus_cmd_encode_DestroyFramebuffer(dev->parent->wire,
                                                  dev->host_handle, f->host_id);
    memset(f, 0, sizeof(*f));
}

/* --- Descriptor state ---
 *
 * Venus does not forward descriptors over the wire yet. DXVK still expects
 * these objects to exist before it can build pipeline layouts and binding
 * sets, so keep guest-local opaque handles and accept descriptor updates as
 * no-ops for the current software/WSI fallback path.
 */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDescriptorSetLayout(VkDevice device,
                                const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkDescriptorSetLayout *pLayout) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = dsl_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_descriptor_set_layout *dsl = &dev->desc_layouts[slot];
    dsl->host_id = 0;
    dsl->binding_count = pCreateInfo->bindingCount;
    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateDescriptorSetLayout(dev->parent->wire,
                                                            dev->host_handle,
                                                            pCreateInfo, &host_id);
        if (venus_w3b4_logged_dsl_create < 64u) {
            venus_w3b4_logged_dsl_create++;
            printf("[VDSL] create rc=%d host=%llu bindings=%u flags=0x%x\n",
                   rc, (unsigned long long)host_id,
                   pCreateInfo->bindingCount, pCreateInfo->flags);
        }
        if (rc == VK_SUCCESS && host_id != 0) dsl->host_id = host_id;
        else {
            memset(dsl, 0, sizeof(*dsl));
            return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)rc;
        }
    }
    *pLayout = (VkDescriptorSetLayout)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_DSL);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
                                 const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !layout) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(layout);
    if (slot < 0 || slot >= (int)VENUS_MAX_DESC_LAYOUT_OBJECTS) return;
    struct venus_descriptor_set_layout *dsl = &dev->desc_layouts[slot];
    if (dev->parent && dev->parent->wire && dsl->host_id != 0)
        (void)venus_cmd_encode_DestroyDescriptorSetLayout(dev->parent->wire,
                                                          dev->host_handle,
                                                          dsl->host_id);
    memset(dsl, 0, sizeof(*dsl));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDescriptorPool(VkDevice device,
                           const VkDescriptorPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkDescriptorPool *pPool) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = dpool_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_descriptor_pool *pool = &dev->desc_pools[slot];
    pool->host_id = 0;
    pool->max_sets = pCreateInfo->maxSets;
    pool->alloc_count = 0;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateDescriptorPool(dev->parent->wire,
                                                       dev->host_handle,
                                                       pCreateInfo, &host_id);
        if (rc == VK_SUCCESS && host_id != 0)
            pool->host_id = host_id;
        else {
            memset(pool, 0, sizeof(*pool));
            return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)rc;
        }
    }

    *pPool = (VkDescriptorPool)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_DPOOL);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                            const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !pool) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(pool);
    if (slot < 0 || slot >= (int)VENUS_MAX_DESC_POOL_OBJECTS) return;
    struct venus_descriptor_pool *dp = &dev->desc_pools[slot];
    if (dev->parent && dev->parent->wire && dp->host_id != 0)
        (void)venus_cmd_encode_DestroyDescriptorPool(dev->parent->wire,
                                                     dev->host_handle,
                                                     dp->host_id);
    memset(&dev->desc_pools[slot], 0, sizeof(dev->desc_pools[slot]));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateDescriptorSets(VkDevice device,
                             const VkDescriptorSetAllocateInfo *pAllocateInfo,
                             VkDescriptorSet *pDescriptorSets) {
    if (!device || !pAllocateInfo || !pDescriptorSets)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int pool_slot = HANDLE_TO_SLOT(pAllocateInfo->descriptorPool);
    if (pool_slot < 0 || pool_slot >= (int)VENUS_MAX_DESC_POOL_OBJECTS)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_descriptor_pool *pool = &dev->desc_pools[pool_slot];
    if (!pool->in_use) return VK_ERROR_INITIALIZATION_FAILED;
    if (pool->alloc_count + pAllocateInfo->descriptorSetCount > pool->max_sets)
        return VK_ERROR_OUT_OF_POOL_MEMORY;

    int allocated[16];
    uint64_t layout_ids[16];
    uint64_t set_ids[16];
    uint32_t count = pAllocateInfo->descriptorSetCount;
    if (count > 16u) return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t i = 0; i < count; i++) {
        int layout_slot = HANDLE_TO_SLOT(pAllocateInfo->pSetLayouts[i]);
        if (layout_slot < 0 || layout_slot >= (int)VENUS_MAX_DESC_LAYOUT_OBJECTS ||
            !dev->desc_layouts[layout_slot].in_use) {
            for (uint32_t j = 0; j < i; j++)
                memset(&dev->desc_sets[allocated[j]], 0, sizeof(dev->desc_sets[allocated[j]]));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        layout_ids[i] = dev->desc_layouts[layout_slot].host_id;
        if (pool->host_id != 0 && layout_ids[i] == 0) {
            for (uint32_t j = 0; j < i; j++)
                memset(&dev->desc_sets[allocated[j]], 0, sizeof(dev->desc_sets[allocated[j]]));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        int set_slot = dset_slot_alloc(dev);
        if (set_slot < 0) {
            for (uint32_t j = 0; j < i; j++)
                memset(&dev->desc_sets[allocated[j]], 0, sizeof(dev->desc_sets[allocated[j]]));
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        allocated[i] = set_slot;
        struct venus_descriptor_set *set = &dev->desc_sets[set_slot];
        set->host_id = 0;
        set->pool_slot = pool_slot;
        set->layout_slot = layout_slot;
        set->image_slot = -1;
        pDescriptorSets[i] =
            (VkDescriptorSet)MAKE_SLOT_HANDLE(dev, set_slot, VENUS_H_MARKER_DSET);
    }

    if (pool->host_id != 0 && dev->parent && dev->parent->wire) {
        int rc = venus_cmd_encode_AllocateDescriptorSets(dev->parent->wire,
                                                         dev->host_handle,
                                                         pool->host_id,
                                                         count, layout_ids,
                                                         set_ids);
        if (rc != VK_SUCCESS) {
            for (uint32_t j = 0; j < count; j++)
                memset(&dev->desc_sets[allocated[j]], 0, sizeof(dev->desc_sets[allocated[j]]));
            return (VkResult)rc;
        }
        for (uint32_t j = 0; j < count; j++)
            dev->desc_sets[allocated[j]].host_id = set_ids[j];
    }

    pool->alloc_count += count;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool,
                         uint32_t descriptorSetCount,
                         const VkDescriptorSet *pDescriptorSets) {
    if (!device || !pDescriptorSets) return VK_SUCCESS;
    struct venus_device *dev = (struct venus_device *)device;
    int pool_slot = pool ? HANDLE_TO_SLOT(pool) : -1;
    struct venus_descriptor_pool *dp =
        (pool_slot >= 0 && pool_slot < (int)VENUS_MAX_DESC_POOL_OBJECTS)
            ? &dev->desc_pools[pool_slot] : NULL;
    uint64_t host_sets[16];
    uint32_t host_count = 0;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        int slot = HANDLE_TO_SLOT(pDescriptorSets[i]);
        if (slot < 0 || slot >= (int)VENUS_MAX_DESC_SET_OBJECTS) continue;
        if (host_count < 16u && dev->desc_sets[slot].host_id != 0)
            host_sets[host_count++] = dev->desc_sets[slot].host_id;
        if (dev->desc_sets[slot].in_use && dp && dp->alloc_count)
            dp->alloc_count--;
        memset(&dev->desc_sets[slot], 0, sizeof(dev->desc_sets[slot]));
    }
    if (dp && dp->host_id != 0 && host_count && dev->parent && dev->parent->wire)
        (void)venus_cmd_encode_FreeDescriptorSets(dev->parent->wire,
                                                  dev->host_handle,
                                                  dp->host_id,
                                                  host_count, host_sets);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_UpdateDescriptorSets(VkDevice device,
                           uint32_t descriptorWriteCount,
                           const VkWriteDescriptorSet *pDescriptorWrites,
                           uint32_t descriptorCopyCount,
                           const VkCopyDescriptorSet *pDescriptorCopies) {
    if (!device) return;
    struct venus_device *dev = (struct venus_device *)device;

    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
        int set_slot = HANDLE_TO_SLOT(w->dstSet);
        if (set_slot < 0 || set_slot >= (int)VENUS_MAX_DESC_SET_OBJECTS)
            continue;
        struct venus_descriptor_set *set = &dev->desc_sets[set_slot];
        if (!set->in_use || !w->pImageInfo || w->descriptorCount == 0)
            continue;
        if (!venus_desc_type_has_image((uint32_t)w->descriptorType))
            continue;

        VkImageView view = w->pImageInfo[0].imageView;
        if (!venus_desc_set_image_from_view(dev, set, view))
            continue;
        if (!venus_w3b4_logged_desc_image) {
            venus_w3b4_logged_desc_image = 1u;
            printf("[VDESC] write set=%d image=%d type=%u\n",
                   set_slot, set->image_slot, (unsigned)w->descriptorType);
        }
    }

    for (uint32_t i = 0; i < descriptorCopyCount; i++) {
        const VkCopyDescriptorSet *c = &pDescriptorCopies[i];
        int src_slot = HANDLE_TO_SLOT(c->srcSet);
        int dst_slot = HANDLE_TO_SLOT(c->dstSet);
        if (src_slot < 0 || src_slot >= (int)VENUS_MAX_DESC_SET_OBJECTS)
            continue;
        if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_DESC_SET_OBJECTS)
            continue;
        if (!dev->desc_sets[src_slot].in_use ||
            !dev->desc_sets[dst_slot].in_use)
            continue;
        dev->desc_sets[dst_slot].image_slot =
            dev->desc_sets[src_slot].image_slot;
    }

    if (dev->parent && dev->parent->wire && dev->host_handle != 0)
        (void)venus_cmd_encode_UpdateDescriptorSets(dev->parent->wire, dev,
                                                    descriptorWriteCount,
                                                    pDescriptorWrites,
                                                    descriptorCopyCount,
                                                    pDescriptorCopies);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDescriptorUpdateTemplate(
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pDescriptorUpdateTemplate)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = dtpl_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_descriptor_update_template *tpl = &dev->desc_templates[slot];
    tpl->entry_count = pCreateInfo->descriptorUpdateEntryCount;
    if (tpl->entry_count > VENUS_MAX_DESC_TPL_ENTRIES)
        tpl->entry_count = VENUS_MAX_DESC_TPL_ENTRIES;
    tpl->template_type = (uint32_t)pCreateInfo->templateType;
    for (uint32_t i = 0; i < tpl->entry_count; i++) {
        const VkDescriptorUpdateTemplateEntry *src =
            &pCreateInfo->pDescriptorUpdateEntries[i];
        tpl->entries[i].dst_binding = src->dstBinding;
        tpl->entries[i].dst_array_element = src->dstArrayElement;
        tpl->entries[i].descriptor_count = src->descriptorCount;
        tpl->entries[i].descriptor_type = (uint32_t)src->descriptorType;
        tpl->entries[i].offset = (uint64_t)src->offset;
        tpl->entries[i].stride = (uint64_t)src->stride;
    }
    *pDescriptorUpdateTemplate =
        (VkDescriptorUpdateTemplate)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_DTEMPLATE);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDescriptorUpdateTemplate(
        VkDevice device,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !descriptorUpdateTemplate) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(descriptorUpdateTemplate);
    if (slot < 0 || slot >= (int)VENUS_MAX_DESC_TPL_OBJECTS) return;
    memset(&dev->desc_templates[slot], 0, sizeof(dev->desc_templates[slot]));
}

VKAPI_ATTR void VKAPI_CALL
venus_UpdateDescriptorSetWithTemplate(VkDevice device,
                                      VkDescriptorSet descriptorSet,
                                      VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                      const void *pData) {
    if (!device || !descriptorSet || !descriptorUpdateTemplate || !pData)
        return;
    struct venus_device *dev = (struct venus_device *)device;
    int set_slot = HANDLE_TO_SLOT(descriptorSet);
    int tpl_slot = HANDLE_TO_SLOT(descriptorUpdateTemplate);
    if (set_slot < 0 || set_slot >= (int)VENUS_MAX_DESC_SET_OBJECTS)
        return;
    if (tpl_slot < 0 || tpl_slot >= (int)VENUS_MAX_DESC_TPL_OBJECTS)
        return;
    struct venus_descriptor_set *set = &dev->desc_sets[set_slot];
    struct venus_descriptor_update_template *tpl = &dev->desc_templates[tpl_slot];
    if (!set->in_use || !tpl->in_use)
        return;

    const uint8_t *base = (const uint8_t *)pData;
    for (uint32_t i = 0; i < tpl->entry_count; i++) {
        if (!venus_desc_type_has_image(tpl->entries[i].descriptor_type))
            continue;
        uint32_t count = tpl->entries[i].descriptor_count;
        uint64_t stride = tpl->entries[i].stride;
        if (stride == 0)
            stride = sizeof(VkDescriptorImageInfo);
        for (uint32_t j = 0; j < count; j++) {
            const VkDescriptorImageInfo *info =
                (const VkDescriptorImageInfo *)(base +
                    tpl->entries[i].offset + (uint64_t)j * stride);
            if (!info || !info->imageView)
                continue;
            if (venus_desc_set_image_from_view(dev, set, info->imageView)) {
                if (venus_w3b4_logged_desc_template < 8u) {
                    venus_w3b4_logged_desc_template++;
                    printf("[VDESC] template set=%d image=%d type=%u entry=%u\n",
                           set_slot, set->image_slot,
                           tpl->entries[i].descriptor_type, i);
                }
                return;
            }
        }
    }
}

/* --- Pipeline Layout --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreatePipelineLayout(VkDevice device,
                           const VkPipelineLayoutCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipelineLayout *pLayout) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = pllayout_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_pipeline_layout *pl = &dev->pl_layouts[slot];
    pl->host_id = 0;
    pl->set_layout_count = pCreateInfo->setLayoutCount;
    pl->push_constant_range_count = pCreateInfo->pushConstantRangeCount;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t set_ids[64];
        if (pCreateInfo->setLayoutCount > 64u) {
            memset(pl, 0, sizeof(*pl));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++) {
            int dslot = HANDLE_TO_SLOT(pCreateInfo->pSetLayouts[i]);
            if (dslot < 0 || dslot >= (int)VENUS_MAX_DESC_LAYOUT_OBJECTS ||
                !dev->desc_layouts[dslot].in_use ||
                dev->desc_layouts[dslot].host_id == 0) {
                memset(pl, 0, sizeof(*pl));
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            set_ids[i] = dev->desc_layouts[dslot].host_id;
        }

        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreatePipelineLayout(dev->parent->wire,
                                                       dev->host_handle,
                                                       pCreateInfo, set_ids,
                                                       pCreateInfo->setLayoutCount,
                                                       &host_id);
        if (venus_w3b4_logged_pl_create < 64u) {
            venus_w3b4_logged_pl_create++;
            printf("[VPLAYOUT] create rc=%d host=%llu sets=%u push=%u\n",
                   rc, (unsigned long long)host_id,
                   pCreateInfo->setLayoutCount,
                   pCreateInfo->pushConstantRangeCount);
        }
        if (rc == 0 && host_id != 0) pl->host_id = host_id;
        else {
            memset(pl, 0, sizeof(*pl));
            return rc == 0 ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)rc;
        }
    }
    *pLayout = (VkPipelineLayout)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_PLLAYOUT);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout,
                            const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !layout) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(layout);
    if (slot < 0 || slot >= (int)VENUS_MAX_PL_LAYOUT_OBJECTS) return;
    struct venus_pipeline_layout *pl = &dev->pl_layouts[slot];
    if (!pl->in_use) return;
    if (dev->parent && dev->parent->wire && pl->host_id != 0)
        (void)venus_cmd_encode_DestroyPipelineLayout(dev->parent->wire,
                                                     dev->host_handle, pl->host_id);
    memset(pl, 0, sizeof(*pl));
}

/* --- Graphics Pipeline --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                              uint32_t createInfoCount,
                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator,
                              VkPipeline *pPipelines) {
    (void)pAllocator; (void)pipelineCache;
    if (!device || !pCreateInfos || !pPipelines || createInfoCount != 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;

    /* Enforce stage pair: VERTEX + FRAGMENT. */
    if (pCreateInfos[0].stageCount != 2) return VK_ERROR_INITIALIZATION_FAILED;
    const VkPipelineShaderStageCreateInfo *st0 = &pCreateInfos[0].pStages[0];
    const VkPipelineShaderStageCreateInfo *st1 = &pCreateInfos[0].pStages[1];
    int vs_ok = 0, fs_ok = 0;
    if (st0->stage == VK_SHADER_STAGE_VERTEX_BIT)   vs_ok = 1;
    if (st0->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_ok = 1;
    if (st1->stage == VK_SHADER_STAGE_VERTEX_BIT)   vs_ok = 1;
    if (st1->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_ok = 1;
    if (!vs_ok || !fs_ok) return VK_ERROR_INITIALIZATION_FAILED;

    int slot = pipeline_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_pipeline *pip = &dev->pipelines[slot];
    pip->host_id = 0;

    /* Resolve referenced host ids. */
    uint64_t vs_host = 0, fs_host = 0;
    const VkPipelineShaderStageCreateInfo *sts[2] = { st0, st1 };
    for (int i = 0; i < 2; i++) {
        int ss = HANDLE_TO_SLOT(sts[i]->module);
        if (ss < 0 || ss >= (int)VENUS_MAX_SHADER_OBJECTS) continue;
        if (!dev->shaders[ss].in_use) continue;
        if (sts[i]->stage == VK_SHADER_STAGE_VERTEX_BIT)   vs_host = dev->shaders[ss].host_id;
        if (sts[i]->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_host = dev->shaders[ss].host_id;
    }
    int lslot = HANDLE_TO_SLOT(pCreateInfos[0].layout);
    uint64_t layout_host = 0;
    if (lslot >= 0 && lslot < (int)VENUS_MAX_PL_LAYOUT_OBJECTS &&
        dev->pl_layouts[lslot].in_use)
        layout_host = dev->pl_layouts[lslot].host_id;
    int rslot = HANDLE_TO_SLOT(pCreateInfos[0].renderPass);
    uint64_t rp_host = 0;
    if (rslot >= 0 && rslot < (int)VENUS_MAX_RP_OBJECTS &&
        dev->render_passes[rslot].in_use)
        rp_host = dev->render_passes[rslot].host_id;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateGraphicsPipelines(
                dev->parent->wire, dev->host_handle, 0ull, 1u, &pCreateInfos[0],
                vs_host, fs_host, layout_host, rp_host, &host_id);
        if (venus_w3b4_logged_pipeline_create < 32u) {
            venus_w3b4_logged_pipeline_create++;
            printf("[VPIPE] create rc=%d host=%llu stages=%u vs=%llu fs=%llu layout=%llu rp=%llu renderPass=%p\n",
                   rc, (unsigned long long)host_id,
                   pCreateInfos[0].stageCount,
                   (unsigned long long)vs_host,
                   (unsigned long long)fs_host,
                   (unsigned long long)layout_host,
                   (unsigned long long)rp_host,
                   (void *)pCreateInfos[0].renderPass);
        }
        if (rc != VK_SUCCESS || host_id == 0) {
            memset(pip, 0, sizeof(*pip));
            pPipelines[0] = VK_NULL_HANDLE;
            return rc == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)rc;
        }
        pip->host_id = host_id;
    } else {
        memset(pip, 0, sizeof(*pip));
        pPipelines[0] = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pPipelines[0] = (VkPipeline)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_PIPELINE);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyPipeline(VkDevice device, VkPipeline pipeline,
                      const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !pipeline) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(pipeline);
    if (slot < 0 || slot >= (int)VENUS_MAX_PIPELINE_OBJECTS) return;
    struct venus_pipeline *p = &dev->pipelines[slot];
    if (!p->in_use) return;
    if (dev->parent && dev->parent->wire && p->host_id != 0)
        (void)venus_cmd_encode_DestroyPipeline(dev->parent->wire,
                                               dev->host_handle, p->host_id);
    memset(p, 0, sizeof(*p));
}

/* --- Command Pool --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateCommandPool(VkDevice device,
                        const VkCommandPoolCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkCommandPool *pPool) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = cmdpool_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct venus_cmd_pool *cp = &dev->cmd_pools[slot];
    cp->host_id            = 0;
    cp->queue_family_index = pCreateInfo->queueFamilyIndex;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_id = 0;
        int rc = venus_cmd_encode_CreateCommandPool(dev->parent->wire,
                                                    dev->host_handle,
                                                    pCreateInfo, &host_id);
        if (rc == 0 && host_id != 0) cp->host_id = host_id;
    }
    *pPool = (VkCommandPool)MAKE_SLOT_HANDLE(dev, slot, VENUS_H_MARKER_CMDPOOL);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyCommandPool(VkDevice device, VkCommandPool pool,
                         const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !pool) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = HANDLE_TO_SLOT(pool);
    if (slot < 0 || slot >= (int)VENUS_MAX_CMD_POOL_OBJECTS) return;
    struct venus_cmd_pool *cp = &dev->cmd_pools[slot];
    if (!cp->in_use) return;
    if (dev->parent && dev->parent->wire && cp->host_id != 0)
        (void)venus_cmd_encode_DestroyCommandPool(dev->parent->wire,
                                                  dev->host_handle, cp->host_id);
    memset(cp, 0, sizeof(*cp));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_ResetCommandPool(VkDevice device, VkCommandPool pool,
                       VkCommandPoolResetFlags flags) {
    (void)flags;
    if (!device || !pool) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int pslot = HANDLE_TO_SLOT(pool);
    if (pslot < 0 || pslot >= (int)VENUS_MAX_CMD_POOL_OBJECTS)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->cmd_pools[pslot].in_use)
        return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < VENUS_MAX_CMD_BUFFER_OBJECTS; i++) {
        struct venus_cmd_buffer *vcb = &dev->cmd_buffers[i];
        if (!vcb->in_use || vcb->pool_slot != pslot)
            continue;
        vcb->recording              = 0;
        vcb->recorded_vb_slot       = -1;
        vcb->recorded_vb_offset     = 0;
        vcb->recorded_vb_stride     = 0;
        vcb->recorded_vertex_count  = 0;
        vcb->recorded_first_vertex  = 0;
        vcb->last_drawn_image_slot  = -1;
        vcb->drew_flag              = 0;
        vcb->recorded_clear_color   = 0u;
        vcb->recorded_has_clear     = 0u;
        vcb->recorded_clear_image_slot = -1;
        vcb->recorded_has_copy_image = 0u;
        vcb->recorded_copy_src_image_slot = -1;
        vcb->recorded_copy_dst_image_slot = -1;
        vcb->recorded_has_copy_buffer_to_image = 0u;
        vcb->recorded_copy_src_buffer_slot = -1;
        vcb->recorded_copy_buffer_dst_image_slot = -1;
        vcb->recorded_sampled_image_slot = -1;
        vcb->recorded_copy_buffer_offset = 0u;
        vcb->recorded_copy_buffer_width = 0u;
        vcb->recorded_copy_buffer_height = 0u;
        vcb->recorded_copy_buffer_row_length = 0u;
    }
    return VK_SUCCESS;
}

/* --- Command Buffer (dispatchable) --- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateCommandBuffers(VkDevice device,
                             const VkCommandBufferAllocateInfo *pInfo,
                             VkCommandBuffer *pCmdBuffers) {
    if (!device || !pInfo || !pCmdBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int pslot = HANDLE_TO_SLOT(pInfo->commandPool);
    if (pslot < 0 || pslot >= (int)VENUS_MAX_CMD_POOL_OBJECTS)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_cmd_pool *cp = &dev->cmd_pools[pslot];
    if (!cp->in_use) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t count = pInfo->commandBufferCount;
    if (count == 0) return VK_SUCCESS;

    int slots[VENUS_MAX_CMD_BUFFER_OBJECTS];
    uint32_t allocated = 0;
    for (; allocated < count && allocated < VENUS_MAX_CMD_BUFFER_OBJECTS; allocated++) {
        int s = cmdbuf_slot_alloc(dev);
        if (s < 0) break;
        slots[allocated] = s;
        /* Clear everything except the in_use flag we just set. */
        struct venus_cmd_buffer *vcb = &dev->cmd_buffers[s];
        memset(&vcb->loader_data, 0, sizeof(VK_LOADER_DATA));
        set_loader_magic_value(&vcb->loader_data);
        vcb->owner     = dev;
        vcb->pool_slot = pslot;
        vcb->recording = 0;
        vcb->host_id   = 0;
        vcb->in_use    = 1;
        /* W3b.6: -1 sentinels mean "unbound" so the rasterizer skips. */
        vcb->recorded_vb_slot       = -1;
        vcb->recorded_vb_offset     = 0;
        vcb->recorded_vb_stride     = 0;
        vcb->recorded_vertex_count  = 0;
        vcb->recorded_first_vertex  = 0;
        vcb->last_drawn_image_slot  = -1;
        vcb->drew_flag              = 0;
        /* W4.8 — clear-only fast path. */
        vcb->recorded_clear_color       = 0u;
        vcb->recorded_has_clear         = 0u;
        vcb->recorded_clear_image_slot  = -1;
        vcb->recorded_has_copy_image    = 0u;
        vcb->recorded_copy_src_image_slot = -1;
        vcb->recorded_copy_dst_image_slot = -1;
        vcb->recorded_has_copy_buffer_to_image = 0u;
        vcb->recorded_copy_src_buffer_slot = -1;
        vcb->recorded_copy_buffer_dst_image_slot = -1;
        vcb->recorded_sampled_image_slot = -1;
        vcb->recorded_copy_buffer_offset = 0u;
        vcb->recorded_copy_buffer_width = 0u;
        vcb->recorded_copy_buffer_height = 0u;
        vcb->recorded_copy_buffer_row_length = 0u;
    }
    if (allocated != count) {
        for (uint32_t i = 0; i < allocated; i++) {
            memset(&dev->cmd_buffers[slots[i]], 0, sizeof(struct venus_cmd_buffer));
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if (dev->parent && dev->parent->wire && dev->host_handle != 0 &&
        cp->host_id != 0) {
        uint64_t ids[VENUS_MAX_CMD_BUFFER_OBJECTS] = {0};
        int rc = venus_cmd_encode_AllocateCommandBuffers(
                dev->parent->wire, dev->host_handle, cp->host_id,
                (uint32_t)pInfo->level, count, ids);
        if (rc == 0) {
            for (uint32_t i = 0; i < count; i++)
                dev->cmd_buffers[slots[i]].host_id = ids[i];
        }
    }

    for (uint32_t i = 0; i < count; i++)
        pCmdBuffers[i] = cb_slot_to_handle(dev, slots[i]);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_FreeCommandBuffers(VkDevice device, VkCommandPool pool,
                         uint32_t count, const VkCommandBuffer *pCmdBuffers) {
    if (!device || !pCmdBuffers || count == 0) return;
    struct venus_device *dev = (struct venus_device *)device;
    int pslot = HANDLE_TO_SLOT(pool);
    uint64_t pool_host = 0;
    if (pslot >= 0 && pslot < (int)VENUS_MAX_CMD_POOL_OBJECTS &&
        dev->cmd_pools[pslot].in_use)
        pool_host = dev->cmd_pools[pslot].host_id;

    uint64_t ids[VENUS_MAX_CMD_BUFFER_OBJECTS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < VENUS_MAX_CMD_BUFFER_OBJECTS; i++) {
        int s = cb_handle_to_slot(dev, pCmdBuffers[i]);
        if (s < 0 || s >= (int)VENUS_MAX_CMD_BUFFER_OBJECTS) continue;
        struct venus_cmd_buffer *cb = &dev->cmd_buffers[s];
        if (!cb->in_use) continue;
        ids[n++] = cb->host_id;
        memset(cb, 0, sizeof(*cb));
    }
    if (n > 0 && pool_host != 0 && dev->parent && dev->parent->wire &&
        dev->host_handle != 0) {
        (void)venus_cmd_encode_FreeCommandBuffers(dev->parent->wire,
                                                  dev->host_handle,
                                                  pool_host, n, ids);
    }
}

/* --- Command recording (dispatchable handle on first arg). --- */

static inline int cb_unwrap(VkCommandBuffer cb, struct venus_device **dev_out,
                            int *slot_out) {
    if (!cb) return 0;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb;
    struct venus_device *dev = vcb->owner;
    if (!dev) return 0;
    int slot = cb_handle_to_slot(dev, cb);
    if (slot < 0 || slot >= (int)VENUS_MAX_CMD_BUFFER_OBJECTS) return 0;
    if (!dev->cmd_buffers[slot].in_use) return 0;
    *dev_out  = dev;
    *slot_out = slot;
    return 1;
}

static uint32_t venus_pack_clear_bgra(const VkClearValue *clear) {
    if (!clear) return 0;
    const float *c = clear->color.float32;
    float r = c[0], g = c[1], b = c[2], a = c[3];
    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
    uint32_t bb = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t bg = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t br = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t ba = (uint32_t)(a * 255.0f + 0.5f);
    return bb | (bg << 8) | (br << 16) | (ba << 24);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_BeginCommandBuffer(VkCommandBuffer cb,
                         const VkCommandBufferBeginInfo *pBegin) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    vcb->recording = 1;
    /* W3b.6: clear recording state from any prior Begin/End cycle so the
     * rasterizer doesn't paint stale geometry. */
    vcb->recorded_vb_slot       = -1;
    vcb->recorded_vb_offset     = 0;
    vcb->recorded_vb_stride     = 0;
    vcb->recorded_vertex_count  = 0;
    vcb->recorded_first_vertex  = 0;
    vcb->last_drawn_image_slot  = -1;
    vcb->drew_flag              = 0;
    /* W4.8 — clear-only fast path. */
    vcb->recorded_clear_color       = 0u;
    vcb->recorded_has_clear         = 0u;
    vcb->recorded_clear_image_slot  = -1;
    vcb->recorded_has_copy_image    = 0u;
    vcb->recorded_copy_src_image_slot = -1;
    vcb->recorded_copy_dst_image_slot = -1;
    vcb->recorded_has_copy_buffer_to_image = 0u;
    vcb->recorded_copy_src_buffer_slot = -1;
    vcb->recorded_copy_buffer_dst_image_slot = -1;
    vcb->recorded_sampled_image_slot = -1;
    vcb->recorded_copy_buffer_offset = 0u;
    vcb->recorded_copy_buffer_width = 0u;
    vcb->recorded_copy_buffer_height = 0u;
    vcb->recorded_copy_buffer_row_length = 0u;
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_BeginCommandBuffer(dev->parent->wire,
                                                  dev->host_handle,
                                                  vcb->host_id,
                                                  pBegin ? pBegin->flags : 0u);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    (void)flags;
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    vcb->recording              = 0;
    vcb->recorded_vb_slot       = -1;
    vcb->recorded_vb_offset     = 0;
    vcb->recorded_vb_stride     = 0;
    vcb->recorded_vertex_count  = 0;
    vcb->recorded_first_vertex  = 0;
    vcb->last_drawn_image_slot  = -1;
    vcb->drew_flag              = 0;
    vcb->recorded_clear_color   = 0u;
    vcb->recorded_has_clear     = 0u;
    vcb->recorded_clear_image_slot = -1;
    vcb->recorded_has_copy_image = 0u;
    vcb->recorded_copy_src_image_slot = -1;
    vcb->recorded_copy_dst_image_slot = -1;
    vcb->recorded_has_copy_buffer_to_image = 0u;
    vcb->recorded_copy_src_buffer_slot = -1;
    vcb->recorded_copy_buffer_dst_image_slot = -1;
    vcb->recorded_sampled_image_slot = -1;
    vcb->recorded_copy_buffer_offset = 0u;
    vcb->recorded_copy_buffer_width = 0u;
    vcb->recorded_copy_buffer_height = 0u;
    vcb->recorded_copy_buffer_row_length = 0u;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_EndCommandBuffer(VkCommandBuffer cb) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    vcb->recording = 0;
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_EndCommandBuffer(dev->parent->wire,
                                                dev->host_handle, vcb->host_id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdBeginRenderPass(VkCommandBuffer cb,
                         const VkRenderPassBeginInfo *pBegin,
                         VkSubpassContents contents) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    if (!pBegin) return;
    int rpslot = HANDLE_TO_SLOT(pBegin->renderPass);
    int fbslot = HANDLE_TO_SLOT(pBegin->framebuffer);
    uint64_t rp_host = (rpslot >= 0 && rpslot < (int)VENUS_MAX_RP_OBJECTS &&
                        dev->render_passes[rpslot].in_use)
                       ? dev->render_passes[rpslot].host_id : 0ull;
    uint64_t fb_host = (fbslot >= 0 && fbslot < (int)VENUS_MAX_FB_OBJECTS &&
                        dev->framebuffers[fbslot].in_use)
                       ? dev->framebuffers[fbslot].host_id : 0ull;

    /* W3b.6 — record the framebuffer's first color attachment image for
     * the CPU-fallback rasterizer. drew_flag is reset; CmdDraw sets it. */
    vcb->last_drawn_image_slot = -1;
    vcb->drew_flag = 0;
    if (fbslot >= 0 && fbslot < (int)VENUS_MAX_FB_OBJECTS &&
        dev->framebuffers[fbslot].in_use) {
        vcb->last_drawn_image_slot = dev->framebuffers[fbslot].first_color_image_slot;
    }

    /* W4.8 — capture LOAD_OP_CLEAR's color from the first clear value.
     * VkClearValue.color.float32 is RGBA 0..1; pack into BGRA8 (compositor
     * expects little-endian u32 bytes [B,G,R,A]). The render pass usually
     * has multiple attachments but for the clear-only path we only honor
     * the first color attachment. */
    if (pBegin->clearValueCount > 0 && pBegin->pClearValues) {
        vcb->recorded_clear_color = venus_pack_clear_bgra(&pBegin->pClearValues[0]);
        vcb->recorded_has_clear   = 1u;
        /* Also tag the framebuffer's color image as the clear target — so
         * QueueSubmit can find the SHM even if no CmdDraw fires. */
        if (vcb->last_drawn_image_slot >= 0)
            vcb->recorded_clear_image_slot = vcb->last_drawn_image_slot;
    }
    if (venus_w3b4_logged_rt_begin < 32u) {
        venus_w3b4_logged_rt_begin++;
        printf("[VRT] BeginRenderPass image=%d clear=%u color=0x%x\n",
               vcb->last_drawn_image_slot, vcb->recorded_has_clear,
               vcb->recorded_clear_color);
    }

    if (dev->parent && dev->parent->wire && vcb->host_id != 0) {
        (void)venus_cmd_encode_CmdBeginRenderPass(
                dev->parent->wire, dev->host_handle, vcb->host_id,
                rp_host, fb_host,
                pBegin->renderArea.offset.x, pBegin->renderArea.offset.y,
                pBegin->renderArea.extent.width, pBegin->renderArea.extent.height,
                pBegin->clearValueCount, pBegin->pClearValues,
                (uint32_t)contents);
    }
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdEndRenderPass(VkCommandBuffer cb) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdEndRenderPass(dev->parent->wire,
                                                dev->host_handle, vcb->host_id);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdBeginRendering(VkCommandBuffer cb,
                        const VkRenderingInfo *pRenderingInfo) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    if (!pRenderingInfo) return;

    vcb->last_drawn_image_slot = -1;
    vcb->drew_flag = 0;

    if (pRenderingInfo->colorAttachmentCount > 0 &&
        pRenderingInfo->pColorAttachments) {
        const VkRenderingAttachmentInfo *att =
            &pRenderingInfo->pColorAttachments[0];
        int vs = HANDLE_TO_SLOT(att->imageView);
        if (vs >= 0 && vs < (int)VENUS_MAX_IMAGE_VIEW_OBJECTS &&
            dev->image_views[vs].in_use)
            vcb->last_drawn_image_slot = dev->image_views[vs].image_slot;

        if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
            vcb->last_drawn_image_slot >= 0) {
            vcb->recorded_clear_color =
                venus_pack_clear_bgra(&att->clearValue);
            vcb->recorded_has_clear = 1u;
            vcb->recorded_clear_image_slot = vcb->last_drawn_image_slot;
        }
    }
    if (venus_w3b4_logged_rt_begin < 32u) {
        venus_w3b4_logged_rt_begin++;
        printf("[VRT] BeginRendering image=%d clear=%u color=0x%x\n",
               vcb->last_drawn_image_slot, vcb->recorded_has_clear,
               vcb->recorded_clear_color);
    }
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdBeginRendering(dev->parent->wire,
                                                 dev, vcb->host_id,
                                                 pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdEndRendering(VkCommandBuffer cb) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdEndRendering(dev->parent->wire,
                                               vcb->host_id);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bindPoint,
                            VkPipelineLayout layout, uint32_t firstSet,
                            uint32_t descriptorSetCount,
                            const VkDescriptorSet *pDescriptorSets,
                            uint32_t dynamicOffsetCount,
                            const uint32_t *pDynamicOffsets) {
    (void)bindPoint; (void)layout; (void)firstSet;
    (void)dynamicOffsetCount; (void)pDynamicOffsets;
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    if (!pDescriptorSets) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    int tracked = 0;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        int set_slot = HANDLE_TO_SLOT(pDescriptorSets[i]);
        if (set_slot < 0 || set_slot >= (int)VENUS_MAX_DESC_SET_OBJECTS)
            continue;
        struct venus_descriptor_set *set = &dev->desc_sets[set_slot];
        if (!set->in_use || set->image_slot < 0)
            continue;
        vcb->recorded_sampled_image_slot = set->image_slot;
        tracked = 1;
        if (!venus_w3b4_logged_desc_bind) {
            venus_w3b4_logged_desc_bind = 1u;
            printf("[VDESC] bind set=%d image=%d cb_image=%d\n",
                   set_slot, set->image_slot, vcb->last_drawn_image_slot);
        }
        break;
    }

    if (dev->parent && dev->parent->wire && vcb->host_id != 0) {
        int rc = venus_cmd_encode_CmdBindDescriptorSets(dev->parent->wire,
                                                        dev, vcb->host_id,
                                                        (uint32_t)bindPoint,
                                                        layout, firstSet,
                                                        descriptorSetCount,
                                                        pDescriptorSets,
                                                        dynamicOffsetCount,
                                                        pDynamicOffsets);
        if (rc != 0) {
            static uint32_t log_count;
            if (log_count < 32u) {
                log_count++;
                printf("[VDBIND-H] failed rc=%d tracked=%d sets=%u first=%u; disabling host cb\n",
                       rc, tracked, descriptorSetCount, firstSet);
            }
            vcb->host_id = 0;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                      VkPipeline pipeline) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];
    int pslot = HANDLE_TO_SLOT(pipeline);
    uint64_t pip_host = (pslot >= 0 && pslot < (int)VENUS_MAX_PIPELINE_OBJECTS &&
                         dev->pipelines[pslot].in_use)
                        ? dev->pipelines[pslot].host_id : 0ull;
    if (pip_host == 0) {
        if (venus_w3b4_logged_bad_pipeline_bind < 32u) {
            venus_w3b4_logged_bad_pipeline_bind++;
            printf("[VPIPE] skip bind invalid pipeline=%p slot=%d bp=%u\n",
                   (void *)pipeline, pslot, (uint32_t)bp);
        }
        return;
    }
    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdBindPipeline(dev->parent->wire,
                                               dev->host_handle, vcb->host_id,
                                               (uint32_t)bp, pip_host);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdDraw(VkCommandBuffer cb, uint32_t vertexCount,
              uint32_t instanceCount, uint32_t firstVertex,
              uint32_t firstInstance) {
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];

    /* W3b.6 — record draw bookkeeping for the CPU-fallback rasterizer. */
    vcb->recorded_vertex_count = vertexCount;
    vcb->recorded_first_vertex = firstVertex;
    vcb->drew_flag             = 1u;
    if (venus_w3b4_logged_draw < 32u) {
        venus_w3b4_logged_draw++;
        printf("[VDRAW] Draw verts=%u image=%d sampled=%d\n",
               vertexCount, vcb->last_drawn_image_slot,
               vcb->recorded_sampled_image_slot);
    }

    if (dev->parent && dev->parent->wire && vcb->host_id != 0)
        (void)venus_cmd_encode_CmdDraw(dev->parent->wire, dev->host_handle,
                                       vcb->host_id, vertexCount, instanceCount,
                                       firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount,
                     uint32_t instanceCount, uint32_t firstIndex,
                     int32_t vertexOffset, uint32_t firstInstance) {
    (void)firstIndex;
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];

    /* DXVK primarily emits indexed draws. Until the host Venus path executes
     * real draws, treat them as draw work for present-time CPU diagnostics. */
    vcb->recorded_vertex_count = indexCount;
    vcb->recorded_first_vertex = vertexOffset > 0 ? (uint32_t)vertexOffset : 0u;
    vcb->drew_flag             = 1u;
    if (venus_w3b4_logged_draw < 32u) {
        venus_w3b4_logged_draw++;
        printf("[VDRAW] DrawIndexed indices=%u image=%d sampled=%d\n",
               indexCount, vcb->last_drawn_image_slot,
               vcb->recorded_sampled_image_slot);
    }

    if (dev->parent && dev->parent->wire && vcb->host_id != 0) {
        (void)venus_cmd_encode_CmdDraw(dev->parent->wire, dev->host_handle,
                                       vcb->host_id, indexCount, instanceCount,
                                       vcb->recorded_first_vertex,
                                       firstInstance);
    }
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer,
                             VkDeviceSize offset, uint32_t drawCount,
                             uint32_t stride) {
    (void)buffer; (void)offset; (void)stride;
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];

    vcb->recorded_vertex_count = drawCount ? 3u : 0u;
    vcb->recorded_first_vertex = 0u;
    vcb->drew_flag             = drawCount ? 1u : 0u;
}

VKAPI_ATTR void VKAPI_CALL
venus_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer,
                      VkDeviceSize offset, uint32_t drawCount,
                      uint32_t stride) {
    (void)buffer; (void)offset; (void)stride;
    struct venus_device *dev; int slot;
    if (!cb_unwrap(cb, &dev, &slot)) return;
    struct venus_cmd_buffer *vcb = &dev->cmd_buffers[slot];

    vcb->recorded_vertex_count = drawCount ? 3u : 0u;
    vcb->recorded_first_vertex = 0u;
    vcb->drew_flag             = drawCount ? 1u : 0u;
}
