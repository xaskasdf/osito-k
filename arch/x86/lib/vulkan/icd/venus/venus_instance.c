#include "venus.h"

/* OsitoK libc — 6-argument fixed-arity syscall gates. */
extern long __syscall0(long);
extern long __syscall1(long, long);
extern long __syscall2(long, long, long);

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern unsigned long strlen(const char *);
extern int printf(const char *, ...);

#define VN_CMD_TYPE_vkEnumeratePhysicalDevices 2u
#define VN_CMD_GENERATE_REPLY 1u

struct vni_writer {
    uint8_t *buf;
    uint32_t off;
    uint32_t cap;
    int err;
};

static void vni_wr_bytes(struct vni_writer *w, const void *src, uint32_t n) {
    if (w->err) return;
    if (w->off + n > w->cap || w->off + n < w->off) {
        w->err = -12;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        w->buf[w->off + i] = ((const uint8_t *)src)[i];
    w->off += n;
}

static void vni_wr_u32(struct vni_writer *w, uint32_t v) {
    vni_wr_bytes(w, &v, 4);
}

static void vni_wr_i32(struct vni_writer *w, int32_t v) {
    vni_wr_bytes(w, &v, 4);
}

static void vni_wr_u64(struct vni_writer *w, uint64_t v) {
    vni_wr_bytes(w, &v, 8);
}

static int venus_cmd_encode_EnumeratePhysicalDevices(
        struct venus_wire *w, uint64_t instance_id,
        uint64_t *out_phys_id, uint32_t *out_count) {
    if (!w || !instance_id || !out_phys_id || !out_count) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);
    extern int venus_wire_submit_reply(struct venus_wire *, const void *,
                                       uint32_t, void *, uint32_t);

    enum { VENUS_EPD_MAX = 4 };
    uint8_t cmd[96];
    uint8_t reply[80];
    struct vni_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t phys_ids[VENUS_EPD_MAX];
    uint32_t requested_count = VENUS_EPD_MAX;
    for (uint32_t i = 0; i < VENUS_EPD_MAX; i++) {
        phys_ids[i] = venus_wire_alloc_object_id(w);
        if (!phys_ids[i]) return -12;
    }

    vni_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkEnumeratePhysicalDevices);
    vni_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vni_wr_u64(&wr, instance_id);
    vni_wr_u64(&wr, 1);                 /* pPhysicalDeviceCount */
    vni_wr_u32(&wr, requested_count);
    vni_wr_u64(&wr, requested_count);   /* pPhysicalDevices[] */
    for (uint32_t i = 0; i < requested_count; i++)
        vni_wr_u64(&wr, phys_ids[i]);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < 36) return rc < 0 ? rc : -5;

    uint32_t reply_cmd  = *(uint32_t *)(reply + 0);
    uint32_t vk_result  = *(uint32_t *)(reply + 4);
    uint64_t cnt_present = *(uint64_t *)(reply + 8);
    uint32_t count      = *(uint32_t *)(reply + 16);
    uint64_t arr_size   = *(uint64_t *)(reply + 20);
    uint64_t phys_reply = *(uint64_t *)(reply + 28);

    printf("[VEPD] reply cmd=%u vk=%u cntp=%llu count=%u arr=%llu phys=%llu guest=%llu bytes=%u\n",
           reply_cmd, vk_result, (unsigned long long)cnt_present, count,
           (unsigned long long)arr_size, (unsigned long long)phys_reply,
           (unsigned long long)phys_ids[0], wr.off);

    if (reply_cmd != VN_CMD_TYPE_vkEnumeratePhysicalDevices)
        return -5;
    if (vk_result != VK_SUCCESS && vk_result != VK_INCOMPLETE)
        return (int)vk_result;
    if (!cnt_present || count == 0 || arr_size == 0 || phys_reply == 0)
        return -5;

    *out_count = count;
    /* The reply may include the renderer's native handle value. Future Venus
     * commands must use the guest object id that this request registered. */
    *out_phys_id = phys_ids[0];
    return (int)vk_result;
}

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
    long caps_rc = __syscall1(VENUS_SYS_GPU_CAPS, (long)&caps);
    printf("[VENUSI] caps rc=%ld caps=0x%x\n", caps_rc, caps);
    self->caps = caps;

    /* If venus not ready in the host, we still create an instance — the
     * loader will see us enumerate zero devices, same as nvk-stub. This
     * keeps vkCreateInstance succeeding even on hosts without virgl. */
    if (caps & VENUS_GPU_CAP_VENUS_READY) {
        long rc = __syscall1(VENUS_SYS_GPU_CTX_CREATE, (long)VENUS_GPU_CTX_VENUS);
        printf("[VENUSI] ctx rc=%ld\n", rc);
        if (rc > 0) {
            self->ctx_id = (int32_t)rc;

            /* Open the wire and issue the real CreateInstance call (W3b.1). */
            extern struct venus_wire *venus_wire_open(int32_t);
            extern int venus_cmd_encode_CreateInstance(struct venus_wire *,
                                                       const VkInstanceCreateInfo *,
                                                       uint64_t *);
            self->wire = venus_wire_open(self->ctx_id);
            printf("[VENUSI] wire=%u ctx=%d\n", self->wire ? 1u : 0u,
                   self->ctx_id);
            if (self->wire) {
                uint64_t host_handle = 0;
                int r = venus_cmd_encode_CreateInstance(self->wire, pCreateInfo,
                                                        &host_handle);
                printf("[VENUSI] create-instance rc=%d host=%llu\n", r,
                       (unsigned long long)host_handle);
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
        if (self->wire &&
            (self->caps & VENUS_GPU_CAP_VENUS_READY) &&
            self->ctx_id > 0 && self->host_handle != 0 &&
            self->phys_handle == 0) {
            uint64_t host_phys = 0;
            uint32_t host_count = 0;
            int rc = venus_cmd_encode_EnumeratePhysicalDevices(
                    self->wire, self->host_handle, &host_phys, &host_count);
            printf("[VENUSI] enumerate-physical rc=%d count=%u phys=%llu\n",
                   rc, host_count, (unsigned long long)host_phys);
            if ((rc == VK_SUCCESS || rc == VK_INCOMPLETE) && host_phys != 0)
                self->phys_handle = host_phys;
        }

        /* Reuse the instance pointer as the physical-device handle.
         * Dispatchable-object magic works either way because both structs
         * begin with VK_LOADER_DATA. The real host physical-device id, when
         * available, is stored separately in self->phys_handle. */
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
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire || !self->phys_handle) {
        venus_features_fallback(pFeatures);
        return;
    }
    int rc = venus_cmd_encode_GetPhysicalDeviceFeatures(
            self->wire, self->phys_handle, pFeatures);
    if (rc != 0) {
        self->host_features_valid = 0;
        venus_features_fallback(pFeatures);
        return;
    }

    self->host_features = *pFeatures;
    self->host_features_valid = 1;

    /* DXVK requires these base feature bits to expose D3D11 FL11 paths.
     * The host CreateDevice path masks them back out when the renderer did
     * not report support, so this only affects guest-side capability gating. */
    pFeatures->geometryShader = VK_TRUE;
    pFeatures->shaderCullDistance = VK_TRUE;
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
