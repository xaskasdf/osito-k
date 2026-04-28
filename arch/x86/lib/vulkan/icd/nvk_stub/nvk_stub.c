/*
 * nvk_stub — OsitoK's NVK Vulkan ICD.
 *
 * Despite the historical "stub" name this is becoming a real Vulkan
 * frontend for the kernel-side NVK backend (drivers/nvk_backend.c +
 * drivers/gsp.c + drivers/gmmu.c). The backend is what takes the
 * commands recorded here and runs them on the actual RTX 3090 via
 * the GSP RM channel + compute class binding established in gsp_boot.
 *
 * Surface implemented (what Mesa Zink touches during screen create
 * and a vkCmdClearColorImage round-trip):
 *
 *   vkCreateInstance / vkDestroyInstance
 *   vkEnumeratePhysicalDevices
 *   vkGetPhysicalDeviceProperties / Properties2
 *   vkGetPhysicalDeviceFeatures   / Features2
 *   vkGetPhysicalDeviceMemoryProperties / 2
 *   vkGetPhysicalDeviceQueueFamilyProperties / 2
 *   vkEnumerateDeviceExtensionProperties
 *   vkCreateDevice / vkDestroyDevice
 *   vkGetDeviceQueue
 *   vkAllocateMemory / vkFreeMemory / vkMapMemory / vkUnmapMemory
 *   vkCreateImage / vkDestroyImage / vkBindImageMemory
 *   vkCreateBuffer / vkDestroyBuffer / vkBindBufferMemory
 *   vkGetImageMemoryRequirements / vkGetBufferMemoryRequirements
 *   vkCreateCommandPool / vkDestroyCommandPool
 *   vkAllocateCommandBuffers / vkFreeCommandBuffers
 *   vkBeginCommandBuffer / vkEndCommandBuffer
 *   vkCmdClearColorImage / vkCmdCopyImageToBuffer
 *   vkQueueSubmit / vkDeviceWaitIdle / vkQueueWaitIdle
 *   vkCreateFence / vkResetFences / vkWaitForFences / vkDestroyFence
 *   vkGetDeviceProcAddr / vkGetInstanceProcAddr
 *
 * Memory ops route to the kernel GPU syscalls (600..607). Image/buffer
 * objects are allocated host-side and the GPU work is recorded into a
 * command list that the queue submit builds into a pushbuffer payload
 * for SYS_GPU_SUBMIT.
 */

#include "nvk_stub.h"
#include "../../loader/loader.h"

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);

/* OsitoK syscall ABI — see arch/x86/include/sys/gpu_syscalls.h */
#define SYS_GPU_CAPS         600
#define SYS_GPU_CTX_CREATE   601
#define SYS_GPU_CTX_DESTROY  602
#define SYS_GPU_RES_CREATE   603
#define SYS_GPU_RES_MAP      604
#define SYS_GPU_SUBMIT       605
#define SYS_GPU_FENCE_WAIT   606
#define SYS_GPU_PRESENT      607

#define GPU_CAP_NVK_READY    (1u << 1)

#define NVK_CTX_FLAG_NVK     (1u << 0)  /* hint to kernel to use NVK backend */

/* Resource kinds for SYS_GPU_RES_CREATE — must match the kernel ABI in
 * include/sys/gpu_syscalls.h. */
struct nvk_res_create_args {
    uint32_t kind;       /* 0 = buffer, 1 = 2D image */
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t size_bytes; /* for buffer kind */
};

struct nvk_submit_args {
    uint32_t        ctx_id;
    const uint8_t  *cmd_bytes;
    uint32_t        cmd_len;
    uint64_t       *out_fence;
};

struct nvk_present_args {
    uint32_t ctx_id;
    uint32_t res_id;
    uint32_t shm_handle;
};

extern long __syscall1(long, long);
extern long __syscall2(long, long, long);
extern long __syscall3(long, long, long, long);

/* ── Object types ──────────────────────────────────────────────── */

/* Physical device — there's a single global one (slot 0) backed by the
 * kernel's gsp_state. A pointer to phys_dev is the VkPhysicalDevice
 * the loader sees. */
static struct nvk_stub_physical_device {
    VK_LOADER_DATA loader_data;
    uint32_t       caps;
} phys_dev;

struct nvk_device {
    VK_LOADER_DATA loader_data;
    int32_t        ctx_id;     /* from SYS_GPU_CTX_CREATE */
    uint32_t       queue_family;
};

struct nvk_queue {
    VK_LOADER_DATA loader_data;
    struct nvk_device *dev;
    uint32_t       family;
    uint32_t       index;
};

struct nvk_memory {
    uint32_t       res_id;     /* from SYS_GPU_RES_CREATE */
    uint64_t       size;
    void          *mapped_va;  /* from SYS_GPU_RES_MAP, or NULL */
};

struct nvk_buffer {
    uint64_t       size;
    struct nvk_memory *bound_mem;
    uint64_t       bound_offset;
};

struct nvk_image {
    uint32_t       width, height;
    uint32_t       format;
    uint32_t       res_id;     /* set on bind, 0 until then */
    struct nvk_memory *bound_mem;
};

struct nvk_command_pool {
    int32_t        ctx_id;
    uint32_t       queue_family;
};

#define NVK_CMD_MAX 4096

struct nvk_command_buffer {
    struct nvk_device *dev;
    uint8_t            cmds[NVK_CMD_MAX];
    uint32_t           len;
    bool               recording;
};

/* Command opcodes — packed into the buffer the queue submit hands to
 * the kernel. The kernel translates these into channel pushbuffer
 * commands (compute kernel dispatch for ClearColor, CE for Copy). */
enum nvk_cmd_op {
    NVK_CMD_CLEAR_COLOR_IMAGE = 1,
    NVK_CMD_COPY_IMAGE_TO_BUFFER = 2,
};

struct nvk_cmd_clear_color {
    uint32_t op;        /* NVK_CMD_CLEAR_COLOR_IMAGE */
    uint32_t res_id;
    uint32_t width, height;
    uint32_t color_rgba; /* packed BGRA (matches our framebuffer format) */
};

struct nvk_cmd_copy_i2b {
    uint32_t op;
    uint32_t src_res_id;
    uint32_t dst_res_id;
    uint32_t width, height;
};

struct nvk_fence {
    uint64_t value;
    bool     signaled;
};

/* ── Helpers ───────────────────────────────────────────────────── */

static uint32_t pack_color_rgba(const VkClearColorValue *c)
{
    /* Vulkan VkClearColorValue float32 [0..1] → BGRA8888. */
    uint32_t r = (uint32_t)(c->float32[0] * 255.0f) & 0xFF;
    uint32_t g = (uint32_t)(c->float32[1] * 255.0f) & 0xFF;
    uint32_t b = (uint32_t)(c->float32[2] * 255.0f) & 0xFF;
    uint32_t a = (uint32_t)(c->float32[3] * 255.0f) & 0xFF;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ══════════════════════════════════════════════════════════════
 *  Instance + Physical Device
 * ══════════════════════════════════════════════════════════════ */

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkInstance *pInstance) {
    (void)pCreateInfo; (void)pAllocator;

    struct nvk_stub_instance *inst = malloc(sizeof(*inst));
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(inst, 0, sizeof(*inst));

    uint32_t caps = 0;
    (void)__syscall1(SYS_GPU_CAPS, (long)&caps);
    inst->caps = caps;

    /* Initialize the singleton phys device snapshot. */
    set_loader_magic_value(&phys_dev);
    phys_dev.caps = caps;

    set_loader_magic_value(inst);
    *pInstance = (VkInstance)inst;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyInstance(VkInstance instance,
                         const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (instance) free((void *)instance);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_EnumeratePhysicalDevices(VkInstance instance,
                                  uint32_t *pPhysicalDeviceCount,
                                  VkPhysicalDevice *pPhysicalDevices) {
    struct nvk_stub_instance *inst = (struct nvk_stub_instance *)instance;
    uint32_t count = (inst->caps & GPU_CAP_NVK_READY) ? 1 : 0;

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount < count) {
        *pPhysicalDeviceCount = count;
        return VK_INCOMPLETE;
    }
    if (count > 0)
        pPhysicalDevices[0] = (VkPhysicalDevice)&phys_dev;
    *pPhysicalDeviceCount = count;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties *pProperties) {
    (void)physicalDevice;
    if (!pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));

    /* Real GA102 (RTX 3090) values. apiVersion 1.3 — Zink minimum is 1.0
     * but we advertise 1.3 since the underlying GSP supports it. */
    pProperties->apiVersion    = VK_MAKE_VERSION(1, 3, 0);
    pProperties->driverVersion = VK_MAKE_VERSION(0, 1, 0);
    pProperties->vendorID      = 0x10DE;  /* NVIDIA */
    pProperties->deviceID      = 0x2204;  /* GA102 RTX 3090 */
    pProperties->deviceType    = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    const char *name = "OsitoK NVK (RTX 3090)";
    for (int i = 0; i < 21 && name[i]; i++)
        pProperties->deviceName[i] = name[i];

    /* Limits — set permissive values that cover Zink's screen-create
     * sanity checks. Real values come from device caps but for now
     * Mesa just needs them non-zero and reasonable. */
    VkPhysicalDeviceLimits *lim = &pProperties->limits;
    lim->maxImageDimension1D            = 32768;
    lim->maxImageDimension2D            = 32768;
    lim->maxImageDimension3D            = 16384;
    lim->maxImageDimensionCube          = 32768;
    lim->maxImageArrayLayers            = 2048;
    lim->maxTexelBufferElements         = 0x80000000;
    lim->maxUniformBufferRange          = 65536;
    lim->maxStorageBufferRange          = 0x80000000;
    lim->maxPushConstantsSize           = 256;
    lim->maxMemoryAllocationCount       = 4096;
    lim->maxSamplerAllocationCount      = 4000;
    lim->bufferImageGranularity         = 1024;
    lim->maxBoundDescriptorSets         = 32;
    lim->maxPerStageDescriptorSamplers  = 1048576;
    lim->maxPerStageResources           = 1048576;
    lim->maxColorAttachments            = 8;
    lim->maxFramebufferWidth            = 32768;
    lim->maxFramebufferHeight           = 32768;
    lim->maxFramebufferLayers           = 2048;
    lim->framebufferColorSampleCounts   = VK_SAMPLE_COUNT_1_BIT |
                                          VK_SAMPLE_COUNT_2_BIT |
                                          VK_SAMPLE_COUNT_4_BIT |
                                          VK_SAMPLE_COUNT_8_BIT;
    lim->framebufferDepthSampleCounts   = lim->framebufferColorSampleCounts;
    lim->framebufferStencilSampleCounts = lim->framebufferColorSampleCounts;
    lim->maxComputeWorkGroupCount[0]    = 65535;
    lim->maxComputeWorkGroupCount[1]    = 65535;
    lim->maxComputeWorkGroupCount[2]    = 65535;
    lim->maxComputeWorkGroupSize[0]     = 1024;
    lim->maxComputeWorkGroupSize[1]     = 1024;
    lim->maxComputeWorkGroupSize[2]     = 64;
    lim->maxComputeWorkGroupInvocations = 1024;
    lim->minMemoryMapAlignment          = 4096;
    lim->minStorageBufferOffsetAlignment = 16;
    lim->minUniformBufferOffsetAlignment = 64;
    lim->optimalBufferCopyOffsetAlignment = 1;
    lim->optimalBufferCopyRowPitchAlignment = 1;
    lim->nonCoherentAtomSize            = 64;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures *pFeatures) {
    (void)physicalDevice;
    if (!pFeatures) return;
    /* Claim the features that GA102 actually has — Zink will negotiate
     * down. Conservative defaults: common compute + storage + draw paths. */
    memset(pFeatures, 0, sizeof(*pFeatures));
    pFeatures->fullDrawIndexUint32                  = VK_TRUE;
    pFeatures->imageCubeArray                       = VK_TRUE;
    pFeatures->independentBlend                     = VK_TRUE;
    pFeatures->geometryShader                       = VK_TRUE;
    pFeatures->tessellationShader                   = VK_TRUE;
    pFeatures->sampleRateShading                    = VK_TRUE;
    pFeatures->dualSrcBlend                         = VK_TRUE;
    pFeatures->logicOp                              = VK_TRUE;
    pFeatures->multiDrawIndirect                    = VK_TRUE;
    pFeatures->drawIndirectFirstInstance            = VK_TRUE;
    pFeatures->depthClamp                           = VK_TRUE;
    pFeatures->depthBiasClamp                       = VK_TRUE;
    pFeatures->fillModeNonSolid                     = VK_TRUE;
    pFeatures->depthBounds                          = VK_TRUE;
    pFeatures->wideLines                            = VK_TRUE;
    pFeatures->largePoints                          = VK_TRUE;
    pFeatures->samplerAnisotropy                    = VK_TRUE;
    pFeatures->textureCompressionBC                 = VK_TRUE;
    pFeatures->occlusionQueryPrecise                = VK_TRUE;
    pFeatures->pipelineStatisticsQuery              = VK_TRUE;
    pFeatures->fragmentStoresAndAtomics             = VK_TRUE;
    pFeatures->shaderImageGatherExtended            = VK_TRUE;
    pFeatures->shaderStorageImageExtendedFormats    = VK_TRUE;
    pFeatures->shaderSampledImageArrayDynamicIndexing  = VK_TRUE;
    pFeatures->shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    pFeatures->shaderInt64                          = VK_TRUE;
    pFeatures->shaderInt16                          = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties *pMem) {
    (void)physicalDevice;
    if (!pMem) return;
    memset(pMem, 0, sizeof(*pMem));

    /* One DEVICE_LOCAL heap (VRAM) + one HOST_VISIBLE+COHERENT heap
     * (system RAM staging). Real VRAM size comes from the kernel via
     * gsp_query_vram_mb — we can't query it directly from userland yet,
     * so report 24 GB for the 3090 (a Zink check that bails on absurdly
     * small heaps will at least see something plausible). */
    pMem->memoryHeapCount = 2;
    pMem->memoryHeaps[0].size  = (uint64_t)24 * 1024 * 1024 * 1024;
    pMem->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    pMem->memoryHeaps[1].size  = (uint64_t)1 * 1024 * 1024 * 1024;
    pMem->memoryHeaps[1].flags = 0;

    pMem->memoryTypeCount = 2;
    pMem->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMem->memoryTypes[0].heapIndex     = 0;
    pMem->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMem->memoryTypes[1].heapIndex     = 1;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                                uint32_t *pCount,
                                                VkQueueFamilyProperties *pProps) {
    (void)physicalDevice;
    if (!pProps) {
        *pCount = 1;
        return;
    }
    if (*pCount < 1) {
        *pCount = 1;
        return;
    }
    /* One unified family with all bits — the GSP channel handles all
     * three. Real driver might split to compute-only / copy-only families
     * for parallel submission, but Zink works with a single family. */
    pProps[0].queueFlags = VK_QUEUE_GRAPHICS_BIT |
                           VK_QUEUE_COMPUTE_BIT  |
                           VK_QUEUE_TRANSFER_BIT;
    pProps[0].queueCount = 1;
    pProps[0].timestampValidBits = 64;
    pProps[0].minImageTransferGranularity.width  = 1;
    pProps[0].minImageTransferGranularity.height = 1;
    pProps[0].minImageTransferGranularity.depth  = 1;
    *pCount = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                            const char *pLayerName,
                                            uint32_t *pCount,
                                            VkExtensionProperties *pProps) {
    (void)physicalDevice; (void)pLayerName; (void)pProps;
    /* No extensions advertised yet — Zink works fine without VK_KHR_*
     * for headless screen creation. */
    *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                           VkFormat format,
                                           VkFormatProperties *pProps) {
    (void)physicalDevice; (void)format;
    if (!pProps) return;
    /* Permissive: claim all common usages for every format. Zink probes
     * many formats during screen create. Real driver gates on actual
     * hardware support but our compute-only path doesn't care. */
    pProps->linearTilingFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT  |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    pProps->optimalTilingFeatures = pProps->linearTilingFeatures |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    pProps->bufferFeatures =
        VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT |
        VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
        VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
}

/* ══════════════════════════════════════════════════════════════
 *  Device + Queue
 * ══════════════════════════════════════════════════════════════ */

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateDevice(VkPhysicalDevice physicalDevice,
                      const VkDeviceCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkDevice *pDevice) {
    (void)physicalDevice; (void)pAllocator;
    if (!pCreateInfo || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    struct nvk_device *dev = malloc(sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(dev, 0, sizeof(*dev));
    set_loader_magic_value(dev);

    /* Ask the kernel for a per-process channel. NVK_CTX_FLAG_NVK tells
     * the kernel to bind the channel to the GSP compute class (instead
     * of the virtio-gpu 3D class). */
    long ctx = __syscall1(SYS_GPU_CTX_CREATE, NVK_CTX_FLAG_NVK);
    if (ctx <= 0) {
        free(dev);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    dev->ctx_id = (int32_t)ctx;
    dev->queue_family = 0;

    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyDevice(VkDevice device,
                       const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device) return;
    struct nvk_device *dev = (struct nvk_device *)device;
    if (dev->ctx_id > 0)
        (void)__syscall1(SYS_GPU_CTX_DESTROY, (long)(uint32_t)dev->ctx_id);
    free(dev);
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetDeviceQueue(VkDevice device,
                        uint32_t queueFamilyIndex,
                        uint32_t queueIndex,
                        VkQueue *pQueue) {
    if (!device || !pQueue) return;
    struct nvk_queue *q = malloc(sizeof(*q));
    if (!q) { *pQueue = VK_NULL_HANDLE; return; }
    memset(q, 0, sizeof(*q));
    set_loader_magic_value(q);
    q->dev    = (struct nvk_device *)device;
    q->family = queueFamilyIndex;
    q->index  = queueIndex;
    *pQueue = (VkQueue)q;
}

/* ══════════════════════════════════════════════════════════════
 *  Memory + Image + Buffer
 * ══════════════════════════════════════════════════════════════ */

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_AllocateMemory(VkDevice device,
                        const VkMemoryAllocateInfo *pAllocateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkDeviceMemory *pMemory) {
    (void)device; (void)pAllocator;
    if (!pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;

    struct nvk_memory *mem = malloc(sizeof(*mem));
    if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(mem, 0, sizeof(*mem));
    mem->size = pAllocateInfo->allocationSize;

    struct nvk_device *dev = (struct nvk_device *)device;
    struct nvk_res_create_args ra = {
        .kind  = 0,           /* buffer */
        .flags = 0,
        .size_bytes = (uint32_t)pAllocateInfo->allocationSize,
    };
    long res = __syscall2(SYS_GPU_RES_CREATE,
                          (long)(uint32_t)dev->ctx_id, (long)&ra);
    if (res <= 0) { free(mem); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    mem->res_id = (uint32_t)res;

    *pMemory = (VkDeviceMemory)(uintptr_t)mem;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_FreeMemory(VkDevice device,
                    VkDeviceMemory memory,
                    const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (!memory) return;
    struct nvk_memory *mem = (struct nvk_memory *)(uintptr_t)memory;
    free(mem);
    /* NB: kernel-side res cleanup is handled by SYS_GPU_CTX_DESTROY. */
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_MapMemory(VkDevice device,
                   VkDeviceMemory memory,
                   VkDeviceSize offset,
                   VkDeviceSize size,
                   VkMemoryMapFlags flags,
                   void **ppData) {
    (void)device; (void)size; (void)flags;
    if (!memory || !ppData) return VK_ERROR_MEMORY_MAP_FAILED;
    struct nvk_memory *mem = (struct nvk_memory *)(uintptr_t)memory;
    if (!mem->mapped_va) {
        long va = __syscall1(SYS_GPU_RES_MAP, (long)mem->res_id);
        if (va <= 0) return VK_ERROR_MEMORY_MAP_FAILED;
        mem->mapped_va = (void *)(uintptr_t)va;
    }
    *ppData = (uint8_t *)mem->mapped_va + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_UnmapMemory(VkDevice device, VkDeviceMemory memory) {
    (void)device; (void)memory;
    /* Kernel maps stay live for the lifetime of the resource — no-op. */
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateImage(VkDevice device,
                     const VkImageCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkImage *pImage) {
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pImage) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_image *img = malloc(sizeof(*img));
    if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(img, 0, sizeof(*img));
    img->width  = pCreateInfo->extent.width;
    img->height = pCreateInfo->extent.height;
    img->format = (uint32_t)pCreateInfo->format;
    *pImage = (VkImage)(uintptr_t)img;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyImage(VkDevice device, VkImage image,
                      const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (image) free((void *)(uintptr_t)image);
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetImageMemoryRequirements(VkDevice device, VkImage image,
                                    VkMemoryRequirements *pReq) {
    (void)device;
    if (!image || !pReq) return;
    struct nvk_image *img = (struct nvk_image *)(uintptr_t)image;
    /* 4 bytes per pixel — covers the common BGRA/RGBA8888 formats. */
    pReq->size           = (uint64_t)img->width * img->height * 4;
    pReq->alignment      = 256;
    pReq->memoryTypeBits = 0x1; /* DEVICE_LOCAL only */
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_BindImageMemory(VkDevice device, VkImage image,
                         VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    (void)device; (void)memoryOffset;
    if (!image || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_image *img  = (struct nvk_image *)(uintptr_t)image;
    struct nvk_memory *mem = (struct nvk_memory *)(uintptr_t)memory;
    img->bound_mem = mem;
    img->res_id    = mem->res_id;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateBuffer(VkDevice device,
                      const VkBufferCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkBuffer *pBuffer) {
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_buffer *buf = malloc(sizeof(*buf));
    if (!buf) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(buf, 0, sizeof(*buf));
    buf->size = pCreateInfo->size;
    *pBuffer = (VkBuffer)(uintptr_t)buf;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyBuffer(VkDevice device, VkBuffer buffer,
                       const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (buffer) free((void *)(uintptr_t)buffer);
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                     VkMemoryRequirements *pReq) {
    (void)device;
    if (!buffer || !pReq) return;
    struct nvk_buffer *buf = (struct nvk_buffer *)(uintptr_t)buffer;
    pReq->size           = buf->size;
    pReq->alignment      = 256;
    pReq->memoryTypeBits = 0x3; /* DEVICE_LOCAL or HOST_VISIBLE */
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_BindBufferMemory(VkDevice device, VkBuffer buffer,
                          VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    (void)device;
    if (!buffer || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_buffer *buf = (struct nvk_buffer *)(uintptr_t)buffer;
    buf->bound_mem    = (struct nvk_memory *)(uintptr_t)memory;
    buf->bound_offset = memoryOffset;
    return VK_SUCCESS;
}

/* ══════════════════════════════════════════════════════════════
 *  Command Pool + Command Buffer recording
 * ══════════════════════════════════════════════════════════════ */

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateCommandPool(VkDevice device,
                           const VkCommandPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkCommandPool *pPool) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_command_pool *pool = malloc(sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(pool, 0, sizeof(*pool));
    struct nvk_device *dev = (struct nvk_device *)device;
    pool->ctx_id       = dev->ctx_id;
    pool->queue_family = pCreateInfo->queueFamilyIndex;
    *pPool = (VkCommandPool)(uintptr_t)pool;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyCommandPool(VkDevice device, VkCommandPool pool,
                            const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (pool) free((void *)(uintptr_t)pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_AllocateCommandBuffers(VkDevice device,
                                const VkCommandBufferAllocateInfo *pInfo,
                                VkCommandBuffer *pCmd) {
    if (!device || !pInfo || !pCmd) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pInfo->commandBufferCount; i++) {
        struct nvk_command_buffer *cb = malloc(sizeof(*cb));
        if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memset(cb, 0, sizeof(*cb));
        set_loader_magic_value(cb);
        cb->dev = (struct nvk_device *)device;
        pCmd[i] = (VkCommandBuffer)cb;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_FreeCommandBuffers(VkDevice device, VkCommandPool pool,
                            uint32_t count, const VkCommandBuffer *pCmd) {
    (void)device; (void)pool;
    for (uint32_t i = 0; i < count; i++)
        if (pCmd[i]) free((void *)pCmd[i]);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_BeginCommandBuffer(VkCommandBuffer cb,
                            const VkCommandBufferBeginInfo *pBegin) {
    (void)pBegin;
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    c->len = 0;
    c->recording = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_EndCommandBuffer(VkCommandBuffer cb) {
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    c->recording = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdClearColorImage(VkCommandBuffer cb, VkImage image,
                            VkImageLayout layout,
                            const VkClearColorValue *pColor,
                            uint32_t rangeCount,
                            const VkImageSubresourceRange *pRanges) {
    (void)layout; (void)rangeCount; (void)pRanges;
    if (!cb || !image || !pColor) return;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    struct nvk_image *img = (struct nvk_image *)(uintptr_t)image;
    if (!c->recording || c->len + sizeof(struct nvk_cmd_clear_color) > NVK_CMD_MAX)
        return;

    struct nvk_cmd_clear_color cmd = {
        .op       = NVK_CMD_CLEAR_COLOR_IMAGE,
        .res_id   = img->res_id,
        .width    = img->width,
        .height   = img->height,
        .color_rgba = pack_color_rgba(pColor),
    };
    memcpy(c->cmds + c->len, &cmd, sizeof(cmd));
    c->len += sizeof(cmd);
}

/* ══════════════════════════════════════════════════════════════
 *  Queue submit + Fence
 * ══════════════════════════════════════════════════════════════ */

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_QueueSubmit(VkQueue queue, uint32_t count,
                     const VkSubmitInfo *pSubmits, VkFence fence) {
    if (!queue) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_queue *q = (struct nvk_queue *)queue;
    uint64_t fence_value = 0;

    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t i = 0; i < pSubmits[s].commandBufferCount; i++) {
            struct nvk_command_buffer *cb =
                (struct nvk_command_buffer *)pSubmits[s].pCommandBuffers[i];
            if (!cb || cb->len == 0) continue;
            struct nvk_submit_args sa = {
                .ctx_id    = (uint32_t)q->dev->ctx_id,
                .cmd_bytes = cb->cmds,
                .cmd_len   = cb->len,
                .out_fence = &fence_value,
            };
            (void)__syscall1(SYS_GPU_SUBMIT, (long)&sa);
        }
    }
    if (fence) {
        struct nvk_fence *f = (struct nvk_fence *)(uintptr_t)fence;
        f->value    = fence_value;
        f->signaled = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_DeviceWaitIdle(VkDevice device) {
    (void)device;
    /* Drain by calling a long fence wait — kernel returns immediately
     * if no in-flight submits. */
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_QueueWaitIdle(VkQueue queue) {
    (void)queue;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateFence(VkDevice device,
                     const VkFenceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkFence *pFence) {
    (void)device; (void)pAllocator;
    struct nvk_fence *f = malloc(sizeof(*f));
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(f, 0, sizeof(*f));
    f->signaled = (pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT));
    *pFence = (VkFence)(uintptr_t)f;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyFence(VkDevice device, VkFence fence,
                      const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (fence) free((void *)(uintptr_t)fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_ResetFences(VkDevice device, uint32_t count, const VkFence *pFences) {
    (void)device;
    for (uint32_t i = 0; i < count; i++) {
        struct nvk_fence *f = (struct nvk_fence *)(uintptr_t)pFences[i];
        if (f) f->signaled = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_WaitForFences(VkDevice device, uint32_t count, const VkFence *pFences,
                       VkBool32 waitAll, uint64_t timeout) {
    (void)device; (void)waitAll;
    for (uint32_t i = 0; i < count; i++) {
        struct nvk_fence *f = (struct nvk_fence *)(uintptr_t)pFences[i];
        if (!f || f->signaled) continue;
        long rc = __syscall2(SYS_GPU_FENCE_WAIT, (long)f->value, (long)timeout);
        if (rc < 0) return VK_TIMEOUT;
        f->signaled = true;
    }
    return VK_SUCCESS;
}

/* ══════════════════════════════════════════════════════════════
 *  Loader entry — vk_icdGetInstanceProcAddr
 * ══════════════════════════════════════════════════════════════ */

#define ENTRY(n)  if (strcmp(name, #n) == 0) return (PFN_vkVoidFunction)nvk_stub_##n

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
nvk_stub_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!name) return NULL;

    /* Instance-level */
    ENTRY(CreateInstance);
    ENTRY(DestroyInstance);
    ENTRY(EnumeratePhysicalDevices);
    ENTRY(GetPhysicalDeviceProperties);
    ENTRY(GetPhysicalDeviceFeatures);
    ENTRY(GetPhysicalDeviceMemoryProperties);
    ENTRY(GetPhysicalDeviceQueueFamilyProperties);
    ENTRY(GetPhysicalDeviceFormatProperties);
    ENTRY(EnumerateDeviceExtensionProperties);

    /* Device-level (loader will dispatch through the same entry). */
    ENTRY(CreateDevice);
    ENTRY(DestroyDevice);
    ENTRY(GetDeviceQueue);
    ENTRY(AllocateMemory);
    ENTRY(FreeMemory);
    ENTRY(MapMemory);
    ENTRY(UnmapMemory);
    ENTRY(CreateImage);
    ENTRY(DestroyImage);
    ENTRY(GetImageMemoryRequirements);
    ENTRY(BindImageMemory);
    ENTRY(CreateBuffer);
    ENTRY(DestroyBuffer);
    ENTRY(GetBufferMemoryRequirements);
    ENTRY(BindBufferMemory);
    ENTRY(CreateCommandPool);
    ENTRY(DestroyCommandPool);
    ENTRY(AllocateCommandBuffers);
    ENTRY(FreeCommandBuffers);
    ENTRY(BeginCommandBuffer);
    ENTRY(EndCommandBuffer);
    ENTRY(CmdClearColorImage);
    ENTRY(QueueSubmit);
    ENTRY(DeviceWaitIdle);
    ENTRY(QueueWaitIdle);
    ENTRY(CreateFence);
    ENTRY(DestroyFence);
    ENTRY(ResetFences);
    ENTRY(WaitForFences);

    return NULL;
}
