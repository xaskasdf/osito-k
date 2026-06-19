#include "venus.h"

/* OsitoK libc — 6-argument fixed-arity syscall gates. */
extern long __syscall0(long);
extern long __syscall1(long, long);
extern long __syscall2(long, long, long);

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance) {
    (void)pCreateInfo; (void)pAllocator;
    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_instance *self = malloc(sizeof(*self));
    if (!self) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(self, 0, sizeof(*self));
    set_loader_magic_value(self);

    /* Probe kernel capability. Caps is u32 passed by reference. */
    uint32_t caps = 0;
    (void)__syscall1(VENUS_SYS_GPU_CAPS, (long)&caps);
    self->caps = caps;

    /* If venus not ready in the host, we still create an instance — the
     * loader will see us enumerate zero devices, same as nvk-stub. This
     * keeps vkCreateInstance succeeding even on hosts without virgl. */
    if (caps & VENUS_GPU_CAP_VENUS_READY) {
        long rc = __syscall1(VENUS_SYS_GPU_CTX_CREATE, (long)VENUS_GPU_CTX_VENUS);
        if (rc > 0) {
            self->ctx_id = (int32_t)rc;

            /* Open the wire and issue the real CreateInstance call (W3b.1). */
            extern struct venus_wire *venus_wire_open(int32_t);
            extern int venus_cmd_encode_CreateInstance(struct venus_wire *,
                                                       const VkInstanceCreateInfo *,
                                                       uint64_t *);
            self->wire = venus_wire_open(self->ctx_id);
            if (self->wire) {
                uint64_t host_handle = 0;
                int r = venus_cmd_encode_CreateInstance(self->wire, pCreateInfo,
                                                        &host_handle);
                if (r == 0 /* VK_SUCCESS */ && host_handle != 0) {
                    self->host_handle = host_handle;
                } else {
                    /* Host rejected or wire stalled — fall back to guest-local
                     * instance (no rendering but enumeration still returns 0). */
                    self->caps &= ~VENUS_GPU_CAP_VENUS_READY;
                }
            } else {
                self->caps &= ~VENUS_GPU_CAP_VENUS_READY;
            }
        } else {
            /* Kernel reported ready but context creation failed — treat as
             * not-ready for this instance. No reason to fail the whole call. */
            self->caps &= ~VENUS_GPU_CAP_VENUS_READY;
        }
    }

    *pInstance = (VkInstance)self;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!instance) return;
    struct venus_instance *self = (struct venus_instance *)instance;
    if (self->wire) {
        extern void venus_wire_close(struct venus_wire *);
        venus_wire_close(self->wire);
        self->wire = 0;
    }
    if (self->ctx_id > 0) {
        (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, (long)(uint32_t)self->ctx_id);
        self->ctx_id = 0;
    }
    free(self);
}

/* Single-device model for W3a. The physical-device handle IS the instance
 * pointer; loader dispatch follows `VK_LOADER_DATA` magic either way. */

VKAPI_ATTR VkResult VKAPI_CALL
venus_EnumeratePhysicalDevices(VkInstance instance,
                               uint32_t *pPhysicalDeviceCount,
                               VkPhysicalDevice *pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance *self = (struct venus_instance *)instance;

    /* W4.7-fix: always report 1 phys device. Every W3b.x encoder has a
     * guest-local fallback path, so even without virglrenderer/VENUS_READY
     * apps get a usable Vulkan stack (no real GPU work, but build through). */
    (void)self;
    uint32_t count = 1u;

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount < count) {
        *pPhysicalDeviceCount = count;
        return VK_INCOMPLETE;
    }
    if (count > 0) {
        /* Reuse the instance pointer as the physical-device handle.
         * Dispatchable-object magic works either way because both structs
         * begin with VK_LOADER_DATA. */
        pPhysicalDevices[0] = (VkPhysicalDevice)self;
    }
    *pPhysicalDeviceCount = count;
    return VK_SUCCESS;
}

/* Guest-local fallback used when the wire is absent or a round-trip
 * fails. Matches the W3a hardcoded identity so apps that don't have a
 * real virgl host still see a predictable device. */
static void venus_props_fallback(VkPhysicalDeviceProperties *pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion       = VK_API_VERSION_1_4;
    pProperties->driverVersion    = VK_MAKE_VERSION(0, 3, 0);
    pProperties->vendorID         = 0x1AF4;  /* Red Hat / virtio */
    pProperties->deviceID         = 0x1050;  /* virtio-gpu */
    pProperties->deviceType       = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    static const char name[] = "OsitoK venus virtio-gpu";
    unsigned long i;
    for (i = 0; i < sizeof(name) && i < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1; i++)
        pProperties->deviceName[i] = name[i];
    pProperties->deviceName[i] = '\0';

    pProperties->limits.maxImageDimension1D = 16384;
    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxImageDimension3D = 2048;
    pProperties->limits.maxImageDimensionCube = 16384;
    pProperties->limits.maxImageArrayLayers = 2048;
    pProperties->limits.maxTexelBufferElements = 128u * 1024u * 1024u;
    pProperties->limits.maxUniformBufferRange = 65536u;
    pProperties->limits.maxStorageBufferRange = 128u * 1024u * 1024u;
    pProperties->limits.maxPushConstantsSize = 128u;
    pProperties->limits.maxMemoryAllocationCount = 4096u;
    pProperties->limits.maxSamplerAllocationCount = 4000u;
    pProperties->limits.bufferImageGranularity = 64u;
    pProperties->limits.maxBoundDescriptorSets = 8u;
    pProperties->limits.maxPerStageDescriptorSamplers = 16u;
    pProperties->limits.maxPerStageDescriptorUniformBuffers = 12u;
    pProperties->limits.maxPerStageDescriptorStorageBuffers = 64u;
    pProperties->limits.maxPerStageDescriptorSampledImages = 128u;
    pProperties->limits.maxPerStageDescriptorStorageImages = 64u;
    pProperties->limits.maxPerStageDescriptorInputAttachments = 8u;
    pProperties->limits.maxPerStageResources = 128u;
    pProperties->limits.maxDescriptorSetSamplers = 128u;
    pProperties->limits.maxDescriptorSetUniformBuffers = 72u;
    pProperties->limits.maxDescriptorSetUniformBuffersDynamic = 8u;
    pProperties->limits.maxDescriptorSetStorageBuffers = 64u;
    pProperties->limits.maxDescriptorSetStorageBuffersDynamic = 4u;
    pProperties->limits.maxDescriptorSetSampledImages = 128u;
    pProperties->limits.maxDescriptorSetStorageImages = 64u;
    pProperties->limits.maxDescriptorSetInputAttachments = 8u;
    pProperties->limits.maxVertexInputAttributes = 32u;
    pProperties->limits.maxVertexInputBindings = 32u;
    pProperties->limits.maxVertexInputAttributeOffset = 2047u;
    pProperties->limits.maxVertexInputBindingStride = 2048u;
    pProperties->limits.maxVertexOutputComponents = 64u;
    pProperties->limits.maxFragmentInputComponents = 64u;
    pProperties->limits.maxFragmentOutputAttachments = 8u;
    pProperties->limits.maxFragmentDualSrcAttachments = 1u;
    pProperties->limits.maxFragmentCombinedOutputResources = 16u;
    pProperties->limits.maxComputeSharedMemorySize = 32768u;
    pProperties->limits.maxComputeWorkGroupCount[0] = 65535u;
    pProperties->limits.maxComputeWorkGroupCount[1] = 65535u;
    pProperties->limits.maxComputeWorkGroupCount[2] = 65535u;
    pProperties->limits.maxComputeWorkGroupInvocations = 1024u;
    pProperties->limits.maxComputeWorkGroupSize[0] = 1024u;
    pProperties->limits.maxComputeWorkGroupSize[1] = 1024u;
    pProperties->limits.maxComputeWorkGroupSize[2] = 64u;
    pProperties->limits.subPixelPrecisionBits = 8u;
    pProperties->limits.subTexelPrecisionBits = 8u;
    pProperties->limits.mipmapPrecisionBits = 8u;
    pProperties->limits.maxDrawIndexedIndexValue = 0xFFFFFFFFu;
    pProperties->limits.maxDrawIndirectCount = 0xFFFFFFFFu;
    pProperties->limits.maxSamplerLodBias = 16.0f;
    pProperties->limits.maxSamplerAnisotropy = 16.0f;
    pProperties->limits.maxViewports = 16u;
    pProperties->limits.maxViewportDimensions[0] = 16384u;
    pProperties->limits.maxViewportDimensions[1] = 16384u;
    pProperties->limits.viewportBoundsRange[0] = -32768.0f;
    pProperties->limits.viewportBoundsRange[1] = 32767.0f;
    pProperties->limits.viewportSubPixelBits = 8u;
    pProperties->limits.minMemoryMapAlignment = 64u;
    pProperties->limits.minTexelBufferOffsetAlignment = 16u;
    pProperties->limits.minUniformBufferOffsetAlignment = 256u;
    pProperties->limits.minStorageBufferOffsetAlignment = 256u;
    pProperties->limits.minTexelOffset = -8;
    pProperties->limits.maxTexelOffset = 7u;
    pProperties->limits.minTexelGatherOffset = -8;
    pProperties->limits.maxTexelGatherOffset = 7u;
    pProperties->limits.minInterpolationOffset = -0.5f;
    pProperties->limits.maxInterpolationOffset = 0.5f;
    pProperties->limits.subPixelInterpolationOffsetBits = 4u;
    pProperties->limits.maxFramebufferWidth = 16384u;
    pProperties->limits.maxFramebufferHeight = 16384u;
    pProperties->limits.maxFramebufferLayers = 256u;
    pProperties->limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.maxColorAttachments = 8u;
    pProperties->limits.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->limits.timestampComputeAndGraphics = VK_TRUE;
    pProperties->limits.timestampPeriod = 1.0f;
    pProperties->limits.optimalBufferCopyOffsetAlignment = 16u;
    pProperties->limits.optimalBufferCopyRowPitchAlignment = 16u;
    pProperties->limits.nonCoherentAtomSize = 64u;
}

static int venus_props_invalid(const VkPhysicalDeviceProperties *pProperties) {
    return pProperties->apiVersion == 0 ||
           pProperties->limits.maxImageDimension2D == 0 ||
           pProperties->limits.bufferImageGranularity == 0 ||
           pProperties->limits.nonCoherentAtomSize == 0;
}

static void venus_memory_fallback(VkPhysicalDeviceMemoryProperties *pMem) {
    memset(pMem, 0, sizeof(*pMem));
    pMem->memoryHeapCount = 1;
    pMem->memoryHeaps[0].size = (uint64_t)1024 * 1024 * 1024;
    pMem->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    pMem->memoryTypeCount = 2;
    pMem->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMem->memoryTypes[0].heapIndex = 0;
    pMem->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMem->memoryTypes[1].heapIndex = 0;
}

static int venus_memory_invalid(const VkPhysicalDeviceMemoryProperties *pMem) {
    if (pMem->memoryHeapCount == 0 || pMem->memoryTypeCount == 0)
        return 1;
    if (pMem->memoryHeapCount > VK_MAX_MEMORY_HEAPS ||
        pMem->memoryTypeCount > VK_MAX_MEMORY_TYPES)
        return 1;
    for (uint32_t i = 0; i < pMem->memoryHeapCount; i++) {
        if (pMem->memoryHeaps[i].size == 0)
            return 1;
    }
    for (uint32_t i = 0; i < pMem->memoryTypeCount; i++) {
        if (pMem->memoryTypes[i].heapIndex >= pMem->memoryHeapCount)
            return 1;
    }
    return 0;
}

static void venus_queue_families_fallback(uint32_t *pCount,
                                          VkQueueFamilyProperties *pFamilies) {
    if (!pFamilies) {
        *pCount = 1;
        return;
    }
    if (*pCount >= 1) {
        memset(&pFamilies[0], 0, sizeof(pFamilies[0]));
        pFamilies[0].queueFlags = VK_QUEUE_GRAPHICS_BIT |
                                  VK_QUEUE_COMPUTE_BIT |
                                  VK_QUEUE_TRANSFER_BIT;
        pFamilies[0].queueCount = 8;
        pFamilies[0].timestampValidBits = 64;
        pFamilies[0].minImageTransferGranularity.width = 1;
        pFamilies[0].minImageTransferGranularity.height = 1;
        pFamilies[0].minImageTransferGranularity.depth = 1;
    }
    *pCount = 1;
}

/* W3b.2: real host query, fallback on any wire error. */
extern int venus_cmd_encode_GetPhysicalDeviceProperties(
        struct venus_wire *, uint64_t, VkPhysicalDeviceProperties *);
extern int venus_cmd_encode_GetPhysicalDeviceFeatures(
        struct venus_wire *, uint64_t, VkPhysicalDeviceFeatures *);
extern int venus_cmd_encode_GetPhysicalDeviceQueueFamilyProperties(
        struct venus_wire *, uint64_t, uint32_t *,
        VkQueueFamilyProperties *);
extern int venus_cmd_encode_GetPhysicalDeviceMemoryProperties(
        struct venus_wire *, uint64_t, VkPhysicalDeviceMemoryProperties *);

/* TODO(w3b.3): in the real venus protocol, physical-device handles are
 * distinct from instance handles and are obtained via a separate
 * enumeration handshake. W3a reuses the instance pointer as the
 * VkPhysicalDevice token; here we correspondingly pass the host
 * instance id as `pd_id`, which a real host will likely reject. Until
 * W3b.3 adds proper physical-device-id bookkeeping, expect these wire
 * calls to fail on a virgl host and the guest-local fallback path to
 * fire. The fallback produces spec-legal zero/stub values, so callers
 * don't observe UB. */
VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceProperties *pProperties) {
    if (!pProperties) return;
    /* Until Venus tracks real host physical-device handles, the guest token is
     * the instance object. Sending host_handle as pd_id may never receive a
     * reply from virglrenderer, so use the spec-valid guest fallback here. */
    (void)physicalDevice;
    venus_props_fallback(pProperties);
    return;
#if 0
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire) {
        venus_props_fallback(pProperties);
        return;
    }
    int rc = venus_cmd_encode_GetPhysicalDeviceProperties(
            self->wire, self->host_handle, pProperties);
    if (rc != 0 || venus_props_invalid(pProperties))
        venus_props_fallback(pProperties);
#endif
}

/* W4.7-fix: enable a sensible subset of features for Zink/Mesa. Without
 * these, Zink rejects the device during cap probing and returns NULL. */
static void venus_features_fallback(VkPhysicalDeviceFeatures *pF) {
    memset(pF, 0, sizeof(*pF));
    pF->robustBufferAccess                       = VK_TRUE;
    pF->fullDrawIndexUint32                      = VK_TRUE;
    pF->imageCubeArray                           = VK_TRUE;
    pF->independentBlend                         = VK_TRUE;
    pF->geometryShader                           = VK_TRUE;
    pF->tessellationShader                       = VK_TRUE;
    pF->sampleRateShading                        = VK_TRUE;
    pF->dualSrcBlend                             = VK_TRUE;
    pF->logicOp                                  = VK_TRUE;
    pF->multiDrawIndirect                        = VK_TRUE;
    pF->drawIndirectFirstInstance                = VK_TRUE;
    pF->depthClamp                               = VK_TRUE;
    pF->depthBiasClamp                           = VK_TRUE;
    pF->fillModeNonSolid                         = VK_TRUE;
    pF->depthBounds                              = VK_TRUE;
    pF->wideLines                                = VK_TRUE;
    pF->largePoints                              = VK_TRUE;
    pF->alphaToOne                               = VK_TRUE;
    pF->multiViewport                            = VK_TRUE;
    pF->samplerAnisotropy                        = VK_TRUE;
    pF->textureCompressionETC2                   = VK_TRUE;
    pF->textureCompressionASTC_LDR               = VK_TRUE;
    pF->textureCompressionBC                     = VK_TRUE;
    pF->occlusionQueryPrecise                    = VK_TRUE;
    pF->pipelineStatisticsQuery                  = VK_TRUE;
    pF->vertexPipelineStoresAndAtomics           = VK_TRUE;
    pF->fragmentStoresAndAtomics                 = VK_TRUE;
    pF->shaderTessellationAndGeometryPointSize   = VK_TRUE;
    pF->shaderImageGatherExtended                = VK_TRUE;
    pF->shaderStorageImageExtendedFormats        = VK_TRUE;
    pF->shaderUniformBufferArrayDynamicIndexing  = VK_TRUE;
    pF->shaderSampledImageArrayDynamicIndexing   = VK_TRUE;
    pF->shaderStorageBufferArrayDynamicIndexing  = VK_TRUE;
    pF->shaderStorageImageArrayDynamicIndexing   = VK_TRUE;
    pF->shaderClipDistance                       = VK_TRUE;
    pF->shaderCullDistance                       = VK_TRUE;
    pF->shaderFloat64                            = VK_TRUE;
    pF->shaderInt64                              = VK_TRUE;
    pF->shaderInt16                              = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceFeatures *pFeatures) {
    if (!pFeatures) return;
    (void)physicalDevice;
    venus_features_fallback(pFeatures);
    return;
#if 0
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire) {
        venus_features_fallback(pFeatures);
        return;
    }
    int rc = venus_cmd_encode_GetPhysicalDeviceFeatures(
            self->wire, self->host_handle, pFeatures);
    if (rc != 0) venus_features_fallback(pFeatures);
#endif
}

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                             uint32_t *pCount,
                                             VkQueueFamilyProperties *pFamilies) {
    if (!pCount) return;
    (void)physicalDevice;
    venus_queue_families_fallback(pCount, pFamilies);
    return;
#if 0
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    /* W4.7-fix: always advertise 1 graphics+compute+transfer queue family
     * (8 queues), so Zink + DXVK can find a usable queue. */
    if (!self || !self->wire) {
        venus_queue_families_fallback(pCount, pFamilies);
        return;
    }
    uint32_t cap = pFamilies ? *pCount : 0;
    int rc = venus_cmd_encode_GetPhysicalDeviceQueueFamilyProperties(
            self->wire, self->host_handle, pCount, pFamilies);
    if (rc != 0 || *pCount == 0) {
        if (pFamilies)
            *pCount = cap;
        venus_queue_families_fallback(pCount, pFamilies);
    }
#endif
}

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceMemoryProperties *pMem) {
    if (!pMem) return;
    memset(pMem, 0, sizeof(*pMem));
    (void)physicalDevice;
    venus_memory_fallback(pMem);
    return;
#if 0
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    /* W4.7-fix: advertise 1 heap (1 GiB DEVICE_LOCAL) with 2 memory types:
     *   [0] DEVICE_LOCAL                                — for VRAM
     *   [1] DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT — for staging
     * Zink requires at least one DEVICE_LOCAL heap and one HOST_VISIBLE
     * type to function. */
    if (!self || !self->wire) {
        venus_memory_fallback(pMem);
        return;
    }
    int rc = venus_cmd_encode_GetPhysicalDeviceMemoryProperties(
            self->wire, self->host_handle, pMem);
    if (rc != 0 || venus_memory_invalid(pMem))
        venus_memory_fallback(pMem);
#endif
}
