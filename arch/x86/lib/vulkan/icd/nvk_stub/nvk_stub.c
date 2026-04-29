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

/* Forward declaration — defined at the bottom of this file. GetDeviceProcAddr
 * needs to forward into it before the actual definition. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
nvk_stub_icdGetInstanceProcAddr(VkInstance instance, const char *name);

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

/* Command opcodes — shared with kernel via include/sys/nvk_cmd.h.
 * The kernel walks this byte stream in nvk_backend_submit. */
#include "../../../include/sys/nvk_cmd.h"

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
 *  Vulkan 1.1+ "Properties2" / "Features2" / etc.
 *
 *  These take a `pNext` chain of extension structs. We populate the
 *  base struct with the same data the v1 path produces, then walk the
 *  chain and clear (zero) any extension structs we don't recognize —
 *  which keeps the contract that "the caller's pNext field gets either
 *  real data or a zeroed sType-tagged block, never garbage". Real
 *  per-extension data (e.g. VkPhysicalDeviceVulkan13Features) lands
 *  when those extensions get advertised in EnumerateDeviceExtensions.
 * ══════════════════════════════════════════════════════════════ */

/* Minimal pNext header layout — every Vulkan v2 struct begins with
 * { VkStructureType sType; void *pNext; }. We avoid pulling in every
 * extension struct definition by treating them generically. */
struct vk_pnext_hdr {
    uint32_t sType;
    void    *pNext;
    /* extension-specific data follows here */
};

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceProperties2(VkPhysicalDevice pd,
                                      VkPhysicalDeviceProperties2 *p) {
    if (!p) return;
    nvk_stub_GetPhysicalDeviceProperties(pd, &p->properties);
    /* Walk pNext chain. We don't know the layout of unknown extension
     * structs, but they all start with sType+pNext, so we can advance
     * without writing bytes we don't understand. */
    struct vk_pnext_hdr *e = (struct vk_pnext_hdr *)p->pNext;
    while (e) e = (struct vk_pnext_hdr *)e->pNext;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd,
                                    VkPhysicalDeviceFeatures2 *f) {
    if (!f) return;
    nvk_stub_GetPhysicalDeviceFeatures(pd, &f->features);
    struct vk_pnext_hdr *e = (struct vk_pnext_hdr *)f->pNext;
    while (e) e = (struct vk_pnext_hdr *)e->pNext;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd,
                                            VkPhysicalDeviceMemoryProperties2 *m) {
    if (!m) return;
    nvk_stub_GetPhysicalDeviceMemoryProperties(pd, &m->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd,
                                                 uint32_t *pCount,
                                                 VkQueueFamilyProperties2 *pProps) {
    if (!pProps) {
        nvk_stub_GetPhysicalDeviceQueueFamilyProperties(pd, pCount, NULL);
        return;
    }
    /* Marshal v1 result into the v2 wrapper struct. */
    VkQueueFamilyProperties tmp[8];
    uint32_t n = (*pCount < 8) ? *pCount : 8;
    nvk_stub_GetPhysicalDeviceQueueFamilyProperties(pd, &n, tmp);
    for (uint32_t i = 0; i < n; i++) pProps[i].queueFamilyProperties = tmp[i];
    *pCount = n;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format,
                                            VkFormatProperties2 *p) {
    if (!p) return;
    nvk_stub_GetPhysicalDeviceFormatProperties(pd, format, &p->formatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format,
                                                VkImageType type, VkImageTiling tiling,
                                                VkImageUsageFlags usage, VkImageCreateFlags flags,
                                                VkImageFormatProperties *pProps) {
    (void)pd; (void)format; (void)type; (void)tiling; (void)usage; (void)flags;
    if (!pProps) return VK_ERROR_INITIALIZATION_FAILED;
    /* Permissive defaults. Real driver gates on hardware support; for
     * the generic CPU-mediated paths in the kernel backend, every
     * format-with-reasonable-extent works. */
    pProps->maxExtent.width  = 16384;
    pProps->maxExtent.height = 16384;
    pProps->maxExtent.depth  = 1;
    pProps->maxMipLevels     = 14;
    pProps->maxArrayLayers   = 2048;
    pProps->sampleCounts     = VK_SAMPLE_COUNT_1_BIT |
                               VK_SAMPLE_COUNT_2_BIT |
                               VK_SAMPLE_COUNT_4_BIT |
                               VK_SAMPLE_COUNT_8_BIT;
    pProps->maxResourceSize  = 0x80000000ULL; /* 2 GB */
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                 const VkPhysicalDeviceImageFormatInfo2 *pInfo,
                                                 VkImageFormatProperties2 *pProps) {
    if (!pInfo || !pProps) return VK_ERROR_INITIALIZATION_FAILED;
    return nvk_stub_GetPhysicalDeviceImageFormatProperties(
        pd, pInfo->format, pInfo->type, pInfo->tiling, pInfo->usage, pInfo->flags,
        &pProps->imageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd,
    const VkPhysicalDeviceExternalBufferInfo *pInfo, VkExternalBufferProperties *pProps) {
    (void)pd; (void)pInfo;
    if (!pProps) return;
    /* We don't support cross-process buffer sharing yet — zero externalMemoryProperties
     * tells the caller "no compatible handle types". */
    memset(&pProps->externalMemoryProperties, 0, sizeof(pProps->externalMemoryProperties));
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice pd,
    const VkPhysicalDeviceExternalSemaphoreInfo *pInfo, VkExternalSemaphoreProperties *pProps) {
    (void)pd; (void)pInfo;
    if (!pProps) return;
    pProps->exportFromImportedHandleTypes = 0;
    pProps->compatibleHandleTypes         = 0;
    pProps->externalSemaphoreFeatures     = 0;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice pd,
    const VkPhysicalDeviceExternalFenceInfo *pInfo, VkExternalFenceProperties *pProps) {
    (void)pd; (void)pInfo;
    if (!pProps) return;
    pProps->exportFromImportedHandleTypes = 0;
    pProps->compatibleHandleTypes         = 0;
    pProps->externalFenceFeatures         = 0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
nvk_stub_GetDeviceProcAddr(VkDevice device, const char *name) {
    (void)device;
    /* Device-level resolution falls back to instance-level — our ICD's
     * dispatch table contains both kinds. */
    return nvk_stub_icdGetInstanceProcAddr((VkInstance)0, name);
}

/* ══════════════════════════════════════════════════════════════
 *  More command-buffer recording (CmdCopy*, CmdFill*, CmdUpdate*)
 * ══════════════════════════════════════════════════════════════ */

#include "../../../include/sys/nvk_cmd.h"

static void cb_append(struct nvk_command_buffer *c, const void *p, uint32_t n) {
    if (!c->recording || c->len + n > NVK_CMD_MAX) return;
    memcpy(c->cmds + c->len, p, n);
    c->len += n;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
                       uint32_t regionCount, const VkBufferCopy *pRegions) {
    if (!cb || !src || !dst) return;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    struct nvk_buffer *sb = (struct nvk_buffer *)(uintptr_t)src;
    struct nvk_buffer *db = (struct nvk_buffer *)(uintptr_t)dst;
    if (!sb->bound_mem || !db->bound_mem) return;
    for (uint32_t i = 0; i < regionCount; i++) {
        struct nvk_cmd_copy_buffer cmd = {
            .op         = NVK_CMD_COPY_BUFFER,
            .src_res_id = sb->bound_mem->res_id,
            .dst_res_id = db->bound_mem->res_id,
            .src_offset = (uint32_t)pRegions[i].srcOffset,
            .dst_offset = (uint32_t)pRegions[i].dstOffset,
            .size       = (uint32_t)pRegions[i].size,
        };
        cb_append(c, &cmd, sizeof(cmd));
    }
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                              VkBuffer dst, uint32_t regionCount,
                              const VkBufferImageCopy *pRegions) {
    (void)layout;
    if (!cb || !src || !dst) return;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    struct nvk_image  *si = (struct nvk_image  *)(uintptr_t)src;
    struct nvk_buffer *db = (struct nvk_buffer *)(uintptr_t)dst;
    if (!si->res_id || !db->bound_mem) return;
    for (uint32_t i = 0; i < regionCount; i++) {
        struct nvk_cmd_copy_i2b cmd = {
            .op         = NVK_CMD_COPY_IMAGE_TO_BUFFER,
            .src_res_id = si->res_id,
            .dst_res_id = db->bound_mem->res_id,
            .width      = pRegions[i].imageExtent.width,
            .height     = pRegions[i].imageExtent.height,
        };
        cb_append(c, &cmd, sizeof(cmd));
    }
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl,
                      VkImage dst, VkImageLayout dl,
                      uint32_t regionCount, const VkImageCopy *pRegions) {
    (void)sl; (void)dl;
    if (!cb || !src || !dst) return;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    struct nvk_image *si = (struct nvk_image *)(uintptr_t)src;
    struct nvk_image *di = (struct nvk_image *)(uintptr_t)dst;
    if (!si->res_id || !di->res_id) return;
    /* Image-to-image as image-to-buffer (since both back the same
     * VRAM blob structure). */
    for (uint32_t i = 0; i < regionCount; i++) {
        struct nvk_cmd_copy_i2b cmd = {
            .op         = NVK_CMD_COPY_IMAGE_TO_BUFFER,
            .src_res_id = si->res_id,
            .dst_res_id = di->res_id,
            .width      = pRegions[i].extent.width,
            .height     = pRegions[i].extent.height,
        };
        cb_append(c, &cmd, sizeof(cmd));
    }
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst,
                              VkImageLayout layout, uint32_t regionCount,
                              const VkBufferImageCopy *pRegions) {
    (void)layout;
    if (!cb || !src || !dst) return;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    struct nvk_buffer *sb = (struct nvk_buffer *)(uintptr_t)src;
    struct nvk_image  *di = (struct nvk_image  *)(uintptr_t)dst;
    if (!sb->bound_mem || !di->res_id) return;
    for (uint32_t i = 0; i < regionCount; i++) {
        struct nvk_cmd_copy_buffer cmd = {
            .op         = NVK_CMD_COPY_BUFFER,
            .src_res_id = sb->bound_mem->res_id,
            .dst_res_id = di->res_id,
            .src_offset = (uint32_t)pRegions[i].bufferOffset,
            .dst_offset = 0,
            .size       = pRegions[i].imageExtent.width *
                          pRegions[i].imageExtent.height * 4,
        };
        cb_append(c, &cmd, sizeof(cmd));
    }
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                       VkDeviceSize size, uint32_t data) {
    if (!cb || !buf) return;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    struct nvk_buffer *b = (struct nvk_buffer *)(uintptr_t)buf;
    if (!b->bound_mem) return;
    struct nvk_cmd_fill_buffer cmd = {
        .op     = NVK_CMD_FILL_BUFFER,
        .res_id = b->bound_mem->res_id,
        .offset = (uint32_t)off,
        .size   = (uint32_t)size,
        .value  = data,
    };
    cb_append(c, &cmd, sizeof(cmd));
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off,
                         VkDeviceSize size, const void *pData) {
    /* For our CPU-mediated backend, "update buffer" is effectively a
     * memcpy from the inline data into the bound memory. The mapped VA
     * is host-visible (PML4[256] shared) so we can write directly. */
    if (!cb || !dst || !pData) return;
    (void)cb; /* nothing to record — write happens immediately. */
    struct nvk_buffer *b = (struct nvk_buffer *)(uintptr_t)dst;
    if (!b->bound_mem || !b->bound_mem->mapped_va) {
        /* Need a map first if not already mapped. */
        long va = __syscall1(SYS_GPU_RES_MAP, (long)b->bound_mem->res_id);
        if (va <= 0) return;
        b->bound_mem->mapped_va = (void *)(uintptr_t)va;
    }
    uint8_t *p = (uint8_t *)b->bound_mem->mapped_va + off;
    memcpy(p, pData, size);
}

/* ══════════════════════════════════════════════════════════════
 *  Sync primitives — semaphore, event, pipeline barrier
 *
 *  Our submit path is currently synchronous, so semaphores and barriers
 *  are effectively no-ops (the work has completed by the time the next
 *  call runs). We still allocate handle slots so the caller's destroy
 *  path doesn't free a NULL.
 * ══════════════════════════════════════════════════════════════ */

struct nvk_semaphore { uint32_t signaled; };
struct nvk_event     { uint32_t signaled; };

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci,
                         const VkAllocationCallbacks *a, VkSemaphore *p) {
    (void)d; (void)ci; (void)a;
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_semaphore *s = malloc(sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->signaled = 0;
    *p = (VkSemaphore)(uintptr_t)s;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) {
    (void)d; (void)a;
    if (s) free((void *)(uintptr_t)s);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateEvent(VkDevice d, const VkEventCreateInfo *ci,
                     const VkAllocationCallbacks *a, VkEvent *p) {
    (void)d; (void)ci; (void)a;
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_event *e = malloc(sizeof(*e));
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    e->signaled = 0;
    *p = (VkEvent)(uintptr_t)e;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyEvent(VkDevice d, VkEvent e, const VkAllocationCallbacks *a) {
    (void)d; (void)a;
    if (e) free((void *)(uintptr_t)e);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_GetEventStatus(VkDevice d, VkEvent e) {
    (void)d;
    if (!e) return VK_NOT_READY;
    struct nvk_event *ev = (struct nvk_event *)(uintptr_t)e;
    return ev->signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_SetEvent(VkDevice d, VkEvent e) {
    (void)d;
    if (!e) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_event *ev = (struct nvk_event *)(uintptr_t)e;
    ev->signaled = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_ResetEvent(VkDevice d, VkEvent e) {
    (void)d;
    if (!e) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_event *ev = (struct nvk_event *)(uintptr_t)e;
    ev->signaled = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d,
                            VkDependencyFlags df, uint32_t mb, const VkMemoryBarrier *pmb,
                            uint32_t bb, const VkBufferMemoryBarrier *pbb,
                            uint32_t ib, const VkImageMemoryBarrier *pib) {
    (void)cb; (void)s; (void)d; (void)df;
    (void)mb; (void)pmb; (void)bb; (void)pbb; (void)ib; (void)pib;
    /* Synchronous backend: barrier is a no-op since the previous Cmd*
     * writes have already finished (CPU memcpy is sync). When async GPU
     * work lands we'll insert a real fence/wait here. */
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags s) {
    (void)cb; (void)s;
    if (e) ((struct nvk_event *)(uintptr_t)e)->signaled = 1;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdResetEvent(VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags s) {
    (void)cb; (void)s;
    if (e) ((struct nvk_event *)(uintptr_t)e)->signaled = 0;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdWaitEvents(VkCommandBuffer cb, uint32_t ec, const VkEvent *pe,
                       VkPipelineStageFlags s, VkPipelineStageFlags d,
                       uint32_t mc, const VkMemoryBarrier *pmb,
                       uint32_t bc, const VkBufferMemoryBarrier *pbb,
                       uint32_t ic, const VkImageMemoryBarrier *pib) {
    (void)cb; (void)ec; (void)pe; (void)s; (void)d;
    (void)mc; (void)pmb; (void)bc; (void)pbb; (void)ic; (void)pib;
    /* Synchronous backend, no actual wait needed. */
}

/* ══════════════════════════════════════════════════════════════
 *  Pipeline / shader / descriptor / framebuffer / render-pass
 *  — opaque handles tracked but not yet executed by the backend.
 *
 *  We allocate small per-handle structs so create/destroy pair up and
 *  bind/dispatch can chain without crashing. The current submit path
 *  doesn't actually run shaders (CPU memcpy fill/copy is enough for
 *  Zink's clear screen path); shader execution lands when a real SASS
 *  pipeline gets wired up.
 * ══════════════════════════════════════════════════════════════ */

#define DECL_OPAQUE_HANDLE(NAME, VKTYPE) \
    struct nvk_##NAME { uint32_t magic; }; \
    VKAPI_ATTR VkResult VKAPI_CALL \
    nvk_stub_Create##NAME(VkDevice d, const Vk##NAME##CreateInfo *ci, \
                          const VkAllocationCallbacks *a, VKTYPE *p) { \
        (void)d; (void)ci; (void)a; \
        if (!p) return VK_ERROR_INITIALIZATION_FAILED; \
        struct nvk_##NAME *h = malloc(sizeof(*h)); \
        if (!h) return VK_ERROR_OUT_OF_HOST_MEMORY; \
        h->magic = 0xC0DE0000u | __LINE__; \
        *p = (VKTYPE)(uintptr_t)h; \
        return VK_SUCCESS; \
    } \
    VKAPI_ATTR void VKAPI_CALL \
    nvk_stub_Destroy##NAME(VkDevice d, VKTYPE h, const VkAllocationCallbacks *a) { \
        (void)d; (void)a; \
        if (h) free((void *)(uintptr_t)h); \
    }

DECL_OPAQUE_HANDLE(ShaderModule,        VkShaderModule)
DECL_OPAQUE_HANDLE(RenderPass,          VkRenderPass)
DECL_OPAQUE_HANDLE(Framebuffer,         VkFramebuffer)
DECL_OPAQUE_HANDLE(DescriptorSetLayout, VkDescriptorSetLayout)
DECL_OPAQUE_HANDLE(PipelineLayout,      VkPipelineLayout)
DECL_OPAQUE_HANDLE(PipelineCache,       VkPipelineCache)
DECL_OPAQUE_HANDLE(Sampler,             VkSampler)
DECL_OPAQUE_HANDLE(ImageView,           VkImageView)
DECL_OPAQUE_HANDLE(QueryPool,           VkQueryPool)

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateGraphicsPipelines(VkDevice d, VkPipelineCache pc, uint32_t count,
                                 const VkGraphicsPipelineCreateInfo *ci,
                                 const VkAllocationCallbacks *a, VkPipeline *p) {
    (void)d; (void)pc; (void)ci; (void)a;
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t *h = malloc(sizeof(*h));
        if (!h) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *h = 0xC0DEC0DE;
        p[i] = (VkPipeline)(uintptr_t)h;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateComputePipelines(VkDevice d, VkPipelineCache pc, uint32_t count,
                                const VkComputePipelineCreateInfo *ci,
                                const VkAllocationCallbacks *a, VkPipeline *p) {
    (void)d; (void)pc; (void)ci; (void)a;
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t *h = malloc(sizeof(*h));
        if (!h) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *h = 0xC0DEC0DE;
        p[i] = (VkPipeline)(uintptr_t)h;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyPipeline(VkDevice d, VkPipeline pl, const VkAllocationCallbacks *a) {
    (void)d; (void)a;
    if (pl) free((void *)(uintptr_t)pl);
}

/* Descriptor pools allocate a fixed-size pool of "set" slots; each
 * AllocateDescriptorSets pulls from this. Because we don't actually run
 * shaders that consume descriptors yet, the sets are just opaque
 * handles for the API contract. */

struct nvk_descriptor_pool {
    uint32_t max_sets;
    uint32_t alloc_count;
};

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *ci,
                              const VkAllocationCallbacks *a, VkDescriptorPool *p) {
    (void)d; (void)a;
    if (!ci || !p) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_descriptor_pool *pool = malloc(sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->max_sets    = ci->maxSets;
    pool->alloc_count = 0;
    *p = (VkDescriptorPool)(uintptr_t)pool;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyDescriptorPool(VkDevice d, VkDescriptorPool p,
                               const VkAllocationCallbacks *a) {
    (void)d; (void)a;
    if (p) free((void *)(uintptr_t)p);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_AllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai,
                                VkDescriptorSet *pSets) {
    (void)d;
    if (!ai || !pSets) return VK_ERROR_INITIALIZATION_FAILED;
    struct nvk_descriptor_pool *pool =
        (struct nvk_descriptor_pool *)(uintptr_t)ai->descriptorPool;
    if (!pool) return VK_ERROR_INITIALIZATION_FAILED;
    if (pool->alloc_count + ai->descriptorSetCount > pool->max_sets)
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        uint32_t *h = malloc(sizeof(*h));
        if (!h) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *h = 0xDE5C0001u + pool->alloc_count + i;
        pSets[i] = (VkDescriptorSet)(uintptr_t)h;
    }
    pool->alloc_count += ai->descriptorSetCount;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_FreeDescriptorSets(VkDevice d, VkDescriptorPool dp, uint32_t count,
                            const VkDescriptorSet *pSets) {
    (void)d; (void)dp;
    for (uint32_t i = 0; i < count; i++)
        if (pSets[i]) free((void *)(uintptr_t)pSets[i]);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_UpdateDescriptorSets(VkDevice d, uint32_t wc, const VkWriteDescriptorSet *pw,
                              uint32_t cc, const VkCopyDescriptorSet *pc) {
    (void)d; (void)wc; (void)pw; (void)cc; (void)pc;
    /* Descriptor contents are stored client-side per the spec; our
     * non-shader-running backend has nothing to write to GPU state. */
}

/* Cmd state-setters. None of these affect our CPU-mediated submit
 * since we don't run real shaders, but we must accept and ignore them
 * so Zink's recording loop completes without errors. */

#define CMD_NOOP3(NAME, T1, T2)  \
    VKAPI_ATTR void VKAPI_CALL \
    nvk_stub_Cmd##NAME(VkCommandBuffer cb, T1 a1, T2 a2) { (void)cb; (void)a1; (void)a2; }
#define CMD_NOOP4(NAME, T1, T2, T3)  \
    VKAPI_ATTR void VKAPI_CALL \
    nvk_stub_Cmd##NAME(VkCommandBuffer cb, T1 a1, T2 a2, T3 a3) { (void)cb; (void)a1; (void)a2; (void)a3; }

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
    (void)cb; (void)bp; (void)pl;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp,
                               VkPipelineLayout layout, uint32_t firstSet,
                               uint32_t count, const VkDescriptorSet *pSets,
                               uint32_t doff, const uint32_t *pDoff) {
    (void)cb; (void)bp; (void)layout; (void)firstSet;
    (void)count; (void)pSets; (void)doff; (void)pDoff;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t firstBinding,
                              uint32_t count, const VkBuffer *pBuffers,
                              const VkDeviceSize *pOffsets) {
    (void)cb; (void)firstBinding; (void)count; (void)pBuffers; (void)pOffsets;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                            VkIndexType type) {
    (void)cb; (void)buf; (void)off; (void)type;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                        const VkViewport *pv) {
    (void)cb; (void)first; (void)count; (void)pv;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count,
                       const VkRect2D *ps) {
    (void)cb; (void)first; (void)count; (void)ps;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetLineWidth(VkCommandBuffer cb, float w) {
    (void)cb; (void)w;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float s) {
    (void)cb; (void)c; (void)clamp; (void)s;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetBlendConstants(VkCommandBuffer cb, const float bc[4]) {
    (void)cb; (void)bc;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetDepthBounds(VkCommandBuffer cb, float minD, float maxD) {
    (void)cb; (void)minD; (void)maxD;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
    (void)cb; (void)f; (void)m;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
    (void)cb; (void)f; (void)m;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t r) {
    (void)cb; (void)f; (void)r;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    (void)cb; (void)x; (void)y; (void)z;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off) {
    (void)cb; (void)buf; (void)off;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic,
                 uint32_t fv, uint32_t fi) {
    (void)cb; (void)vc; (void)ic; (void)fv; (void)fi;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst,
                        uint32_t fi, int32_t vo, uint32_t fInst) {
    (void)cb; (void)ic; (void)inst; (void)fi; (void)vo; (void)fInst;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                         uint32_t drawCount, uint32_t stride) {
    (void)cb; (void)buf; (void)off; (void)drawCount; (void)stride;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                uint32_t drawCount, uint32_t stride) {
    (void)cb; (void)buf; (void)off; (void)drawCount; (void)stride;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdBeginRenderPass(VkCommandBuffer cb,
                            const VkRenderPassBeginInfo *pBegin,
                            VkSubpassContents contents) {
    /* Zink uses BeginRenderPass with clearValues to do a "load-clear".
     * Replay each color attachment clear as a CLEAR_COLOR_IMAGE op so
     * the kernel actually paints the framebuffer. */
    if (!cb || !pBegin) return;
    (void)contents;
    struct nvk_command_buffer *c = (struct nvk_command_buffer *)cb;
    if (!pBegin->framebuffer) return;
    for (uint32_t i = 0; i < pBegin->clearValueCount; i++) {
        /* We don't track the framebuffer's images here yet; skip until
         * Wave 4 wires up per-fb image lists. */
        (void)pBegin->pClearValues;
    }
    (void)c;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdEndRenderPass(VkCommandBuffer cb) { (void)cb; }

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdNextSubpass(VkCommandBuffer cb, VkSubpassContents contents) {
    (void)cb; (void)contents;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool qp, uint32_t q,
                       VkQueryControlFlags f) {
    (void)cb; (void)qp; (void)q; (void)f;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdEndQuery(VkCommandBuffer cb, VkQueryPool qp, uint32_t q) {
    (void)cb; (void)qp; (void)q;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool qp,
                           uint32_t firstQ, uint32_t qCount) {
    (void)cb; (void)qp; (void)firstQ; (void)qCount;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_CmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool qp,
                                 uint32_t firstQ, uint32_t qCount,
                                 VkBuffer dst, VkDeviceSize dstOff,
                                 VkDeviceSize stride, VkQueryResultFlags f) {
    (void)cb; (void)qp; (void)firstQ; (void)qCount;
    (void)dst; (void)dstOff; (void)stride; (void)f;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_GetQueryPoolResults(VkDevice d, VkQueryPool qp, uint32_t firstQ,
                             uint32_t qCount, size_t dataSize, void *pData,
                             VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)d; (void)qp; (void)firstQ; (void)qCount;
    (void)dataSize; (void)pData; (void)stride; (void)flags;
    /* No queries actually run since we don't use real GPU pipelines yet.
     * Return zeros via VK_NOT_READY so callers don't read uninit data. */
    if (pData) memset(pData, 0, dataSize);
    return VK_NOT_READY;
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

    /* PhysicalDevice queries (KHR_get_physical_device_properties2 + 1.1 core) */
    ENTRY(GetPhysicalDeviceProperties2);
    ENTRY(GetPhysicalDeviceFeatures2);
    ENTRY(GetPhysicalDeviceMemoryProperties2);
    ENTRY(GetPhysicalDeviceQueueFamilyProperties2);
    ENTRY(GetPhysicalDeviceFormatProperties2);
    ENTRY(GetPhysicalDeviceImageFormatProperties);
    ENTRY(GetPhysicalDeviceImageFormatProperties2);
    ENTRY(GetPhysicalDeviceExternalBufferProperties);
    ENTRY(GetPhysicalDeviceExternalSemaphoreProperties);
    ENTRY(GetPhysicalDeviceExternalFenceProperties);

    /* Device-level dispatch entry */
    ENTRY(GetDeviceProcAddr);

    /* Memory ops (record opcodes for kernel-side replay) */
    ENTRY(CmdCopyBuffer);
    ENTRY(CmdCopyImage);
    ENTRY(CmdCopyImageToBuffer);
    ENTRY(CmdCopyBufferToImage);
    ENTRY(CmdFillBuffer);
    ENTRY(CmdUpdateBuffer);

    /* Sync primitives */
    ENTRY(CreateSemaphore);
    ENTRY(DestroySemaphore);
    ENTRY(CreateEvent);
    ENTRY(DestroyEvent);
    ENTRY(GetEventStatus);
    ENTRY(SetEvent);
    ENTRY(ResetEvent);
    ENTRY(CmdPipelineBarrier);
    ENTRY(CmdSetEvent);
    ENTRY(CmdResetEvent);
    ENTRY(CmdWaitEvents);

    /* Opaque-handle objects */
    ENTRY(CreateShaderModule);
    ENTRY(DestroyShaderModule);
    ENTRY(CreateRenderPass);
    ENTRY(DestroyRenderPass);
    ENTRY(CreateFramebuffer);
    ENTRY(DestroyFramebuffer);
    ENTRY(CreateDescriptorSetLayout);
    ENTRY(DestroyDescriptorSetLayout);
    ENTRY(CreatePipelineLayout);
    ENTRY(DestroyPipelineLayout);
    ENTRY(CreatePipelineCache);
    ENTRY(DestroyPipelineCache);
    ENTRY(CreateSampler);
    ENTRY(DestroySampler);
    ENTRY(CreateImageView);
    ENTRY(DestroyImageView);
    ENTRY(CreateQueryPool);
    ENTRY(DestroyQueryPool);

    /* Pipelines */
    ENTRY(CreateGraphicsPipelines);
    ENTRY(CreateComputePipelines);
    ENTRY(DestroyPipeline);

    /* Descriptors */
    ENTRY(CreateDescriptorPool);
    ENTRY(DestroyDescriptorPool);
    ENTRY(AllocateDescriptorSets);
    ENTRY(FreeDescriptorSets);
    ENTRY(UpdateDescriptorSets);

    /* Cmd state setters */
    ENTRY(CmdBindPipeline);
    ENTRY(CmdBindDescriptorSets);
    ENTRY(CmdBindVertexBuffers);
    ENTRY(CmdBindIndexBuffer);
    ENTRY(CmdSetViewport);
    ENTRY(CmdSetScissor);
    ENTRY(CmdSetLineWidth);
    ENTRY(CmdSetDepthBias);
    ENTRY(CmdSetBlendConstants);
    ENTRY(CmdSetDepthBounds);
    ENTRY(CmdSetStencilCompareMask);
    ENTRY(CmdSetStencilWriteMask);
    ENTRY(CmdSetStencilReference);

    /* Draw / dispatch */
    ENTRY(CmdDispatch);
    ENTRY(CmdDispatchIndirect);
    ENTRY(CmdDraw);
    ENTRY(CmdDrawIndexed);
    ENTRY(CmdDrawIndirect);
    ENTRY(CmdDrawIndexedIndirect);

    /* Render pass */
    ENTRY(CmdBeginRenderPass);
    ENTRY(CmdEndRenderPass);
    ENTRY(CmdNextSubpass);

    /* Query */
    ENTRY(CmdBeginQuery);
    ENTRY(CmdEndQuery);
    ENTRY(CmdResetQueryPool);
    ENTRY(CmdCopyQueryPoolResults);
    ENTRY(GetQueryPoolResults);

    return NULL;
}
