/*
 * venus_device.c — device + memory + buffer entry points for the venus ICD.
 *
 * W3b.3 behavior: every entry point attempts a real venus wire round-trip
 * when `parent->wire` is live; if that fails (or the wire is absent) we
 * fall back to a guest-local implementation so callers still see valid
 * handles and data. hello-memory exercises both paths.
 */
#include "venus.h"

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern int   printf(const char *, ...);
extern long  __syscall1(long, long);
extern long  __syscall2(long, long, long);

#define VENUS_SYS_GPU_RES_CREATE       603L
#define VENUS_SYS_GPU_RES_MAP          604L
#define VENUS_SYS_SHM_UNMAP            502L
#define VENUS_SYS_SHM_DESTROY          503L

#define VENUS_GPU_RES_KIND_BUFFER      0u
#define VENUS_GPU_RES_FLAG_HOST_COHERENT (1u << 0)
#define VENUS_GPU_RES_FLAG_TRANSFER_SRC  (1u << 1)
#define VENUS_GPU_RES_FLAG_TRANSFER_DST  (1u << 2)

struct venus_gpu_res_create_args {
    uint32_t kind;
    uint32_t flags;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
};

#ifdef __OSITO_K__
extern long write(int, const void *, unsigned long);
static void venus_dev_log(const char *msg) {
    unsigned long len = 0;
    while (msg[len]) len++;
    write(2, msg, len);
}
#else
static void venus_dev_log(const char *msg) { (void)msg; }
#endif

/* Forward decls for encoders (keep out of venus.h to keep that header
 * app-facing / handle-only). */
extern int venus_cmd_encode_CreateDevice(struct venus_wire *, uint64_t,
                                         const VkDeviceCreateInfo *,
                                         uint64_t *);
extern int venus_cmd_encode_DestroyDevice(struct venus_wire *, uint64_t);
extern int venus_cmd_encode_AllocateMemory(struct venus_wire *, uint64_t,
                                           uint64_t, uint32_t, uint64_t *);
extern int venus_cmd_encode_FreeMemory(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_MapMemory(struct venus_wire *, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint32_t,
                                      uint64_t *);
extern int venus_cmd_encode_UnmapMemory(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_CreateBuffer(struct venus_wire *, uint64_t,
                                         const VkBufferCreateInfo *,
                                         uint64_t *);
extern int venus_cmd_encode_DestroyBuffer(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_GetBufferMemoryRequirements(
        struct venus_wire *, uint64_t, uint64_t, VkMemoryRequirements *);
extern int venus_cmd_encode_BindBufferMemory(struct venus_wire *, uint64_t,
                                             uint64_t, uint64_t, uint64_t);
extern int venus_cmd_encode_GetDeviceQueue(
        struct venus_device *, uint32_t, uint32_t, struct venus_queue **);

static uint32_t venus_log_device_create;
static uint32_t venus_log_device_host_disabled;
static uint32_t venus_log_mem_alloc_diag;
static uint32_t venus_log_buf_create_diag;
static uint32_t venus_log_buf_bind_diag;
static uint32_t venus_log_gpu_mem_diag;

/* --- Small slot allocators ---------------------------------------------- */

static int venus_mem_slot_alloc(struct venus_device *dev) {
    for (uint32_t i = 0; i < VENUS_MAX_MEM_OBJECTS; i++)
        if (!dev->memories[i].in_use) { dev->memories[i].in_use = 1; return (int)i; }
    return -1;
}
static int venus_buf_slot_alloc(struct venus_device *dev) {
    for (uint32_t i = 0; i < VENUS_MAX_BUF_OBJECTS; i++)
        if (!dev->buffers[i].in_use) { dev->buffers[i].in_use = 1; return (int)i; }
    return -1;
}

/* Map slot index <-> VkDeviceMemory / VkBuffer handle.
 *
 * Memory and buffer handles are non-dispatchable (uint64_t) — we pack
 * (device_ptr | slot) into the handle so the owning device can be
 * recovered without a table. Layout: top 16 bits = 1 (marker) + slot,
 * low 48 bits = device pointer. This keeps sign-extension clean on
 * x86-64 canonical addresses. */

/* Handle layout (64-bit non-dispatchable):
 *   bit 63     : marker (always 1 for venus ICD handles)
 *   bit 62     : object tag (0 = memory, 1 = buffer)
 *   bits 61:60 : reserved (0)
 *   bits 59:48 : slot index (12 bits, VENUS_MAX_* << 4096)
 *   bits 47:0  : device pointer (canonical low 48 bits)
 *
 * Decoders MUST mask out bits 63..60 so the slot doesn't pick up the
 * marker/tag (which would make every slot >= 0x8000 and fail the
 * `slot >= VENUS_MAX_*` guard).
 */
#define VENUS_H_MARKER_MEM   0x8000000000000000ull
#define VENUS_H_MARKER_BUF   0xC000000000000000ull
#define VENUS_H_SLOT_MASK    0x0FFFull   /* 12 bits — plenty of headroom */
#define VENUS_H_PTR_MASK     0x0000FFFFFFFFFFFFull

static inline VkDeviceMemory mem_slot_to_handle(struct venus_device *dev, int slot) {
    uint64_t h = ((uint64_t)(uint32_t)slot & VENUS_H_SLOT_MASK) << 48
               | ((uint64_t)(uintptr_t)dev & VENUS_H_PTR_MASK);
    h |= VENUS_H_MARKER_MEM;
    return (VkDeviceMemory)h;
}
static inline VkBuffer buf_slot_to_handle(struct venus_device *dev, int slot) {
    uint64_t h = ((uint64_t)(uint32_t)slot & VENUS_H_SLOT_MASK) << 48
               | ((uint64_t)(uintptr_t)dev & VENUS_H_PTR_MASK);
    h |= VENUS_H_MARKER_BUF;
    return (VkBuffer)h;
}
static inline int mem_handle_to_slot(VkDeviceMemory h) {
    uint64_t v = (uint64_t)h;
    return (int)((v >> 48) & VENUS_H_SLOT_MASK);  /* strip marker/tag bits */
}
static inline int buf_handle_to_slot(VkBuffer h) {
    uint64_t v = (uint64_t)h;
    return (int)((v >> 48) & VENUS_H_SLOT_MASK);  /* strip marker/tag bits */
}

static void venus_memory_release_local(struct venus_memory *m) {
    if (!m) return;
    if (m->is_shm_backed) {
        if (m->local_ptr)
            (void)__syscall1(VENUS_SYS_SHM_UNMAP, (long)m->shm_handle);
        if (m->shm_handle)
            (void)__syscall1(VENUS_SYS_SHM_DESTROY, (long)m->shm_handle);
    } else if (m->is_gpu_backed) {
        /* No explicit resource-destroy syscall exists yet; kernel process
         * cleanup drops GPU resources. Do not free() kernel mappings. */
    } else if (m->local_ptr) {
        free(m->local_ptr);
    }
    m->local_ptr = 0;
    m->is_shm_backed = 0;
    m->is_gpu_backed = 0;
    m->gpu_res_id = 0;
    m->shm_handle = 0;
    m->shm_width = 0;
    m->shm_height = 0;
}

static int venus_try_gpu_backing(struct venus_device *dev,
                                 struct venus_memory *m) {
    if (!dev || !dev->parent || !m) return 0;
    if (dev->parent->ctx_id <= 0) return 0;
    if (!(dev->parent->caps & VENUS_GPU_CAP_VENUS_READY)) return 0;

    struct venus_gpu_res_create_args args;
    memset(&args, 0, sizeof(args));
    args.kind = VENUS_GPU_RES_KIND_BUFFER;
    args.flags = VENUS_GPU_RES_FLAG_HOST_COHERENT |
                 VENUS_GPU_RES_FLAG_TRANSFER_SRC |
                 VENUS_GPU_RES_FLAG_TRANSFER_DST;
    args.size = m->size ? m->size : 1u;

    long rid = __syscall2(VENUS_SYS_GPU_RES_CREATE,
                          (long)(uint32_t)dev->parent->ctx_id,
                          (long)&args);
    if (rid <= 0)
        return 0;

    long va = __syscall1(VENUS_SYS_GPU_RES_MAP, rid);
    if (va == 0)
        return 0;

    m->local_ptr = (void *)(uintptr_t)va;
    m->is_gpu_backed = 1;
    m->gpu_res_id = (uint32_t)rid;
    if (venus_log_gpu_mem_diag < 24u) {
        venus_log_gpu_mem_diag++;
        printf("[VGPUM] memory res=%u size=%llu va=%p type=%u\n",
               m->gpu_res_id, (unsigned long long)m->size,
               m->local_ptr, m->type_index);
    }
    return 1;
}

static void venus_mask_host_enabled_features(
        VkPhysicalDeviceFeatures *enabled,
        const VkPhysicalDeviceFeatures *supported) {
#define VENUS_MASK_FEATURE(name) \
    do { if (!supported->name) enabled->name = VK_FALSE; } while (0)

    VENUS_MASK_FEATURE(robustBufferAccess);
    VENUS_MASK_FEATURE(fullDrawIndexUint32);
    VENUS_MASK_FEATURE(imageCubeArray);
    VENUS_MASK_FEATURE(independentBlend);
    VENUS_MASK_FEATURE(geometryShader);
    VENUS_MASK_FEATURE(tessellationShader);
    VENUS_MASK_FEATURE(sampleRateShading);
    VENUS_MASK_FEATURE(dualSrcBlend);
    VENUS_MASK_FEATURE(logicOp);
    VENUS_MASK_FEATURE(multiDrawIndirect);
    VENUS_MASK_FEATURE(drawIndirectFirstInstance);
    VENUS_MASK_FEATURE(depthClamp);
    VENUS_MASK_FEATURE(depthBiasClamp);
    VENUS_MASK_FEATURE(fillModeNonSolid);
    VENUS_MASK_FEATURE(depthBounds);
    VENUS_MASK_FEATURE(wideLines);
    VENUS_MASK_FEATURE(largePoints);
    VENUS_MASK_FEATURE(alphaToOne);
    VENUS_MASK_FEATURE(multiViewport);
    VENUS_MASK_FEATURE(samplerAnisotropy);
    VENUS_MASK_FEATURE(textureCompressionETC2);
    VENUS_MASK_FEATURE(textureCompressionASTC_LDR);
    VENUS_MASK_FEATURE(textureCompressionBC);
    VENUS_MASK_FEATURE(occlusionQueryPrecise);
    VENUS_MASK_FEATURE(pipelineStatisticsQuery);
    VENUS_MASK_FEATURE(vertexPipelineStoresAndAtomics);
    VENUS_MASK_FEATURE(fragmentStoresAndAtomics);
    VENUS_MASK_FEATURE(shaderTessellationAndGeometryPointSize);
    VENUS_MASK_FEATURE(shaderImageGatherExtended);
    VENUS_MASK_FEATURE(shaderStorageImageExtendedFormats);
    VENUS_MASK_FEATURE(shaderStorageImageMultisample);
    VENUS_MASK_FEATURE(shaderStorageImageReadWithoutFormat);
    VENUS_MASK_FEATURE(shaderStorageImageWriteWithoutFormat);
    VENUS_MASK_FEATURE(shaderUniformBufferArrayDynamicIndexing);
    VENUS_MASK_FEATURE(shaderSampledImageArrayDynamicIndexing);
    VENUS_MASK_FEATURE(shaderStorageBufferArrayDynamicIndexing);
    VENUS_MASK_FEATURE(shaderStorageImageArrayDynamicIndexing);
    VENUS_MASK_FEATURE(shaderClipDistance);
    VENUS_MASK_FEATURE(shaderCullDistance);
    VENUS_MASK_FEATURE(shaderFloat64);
    VENUS_MASK_FEATURE(shaderInt64);
    VENUS_MASK_FEATURE(shaderInt16);
    VENUS_MASK_FEATURE(shaderResourceResidency);
    VENUS_MASK_FEATURE(shaderResourceMinLod);
    VENUS_MASK_FEATURE(sparseBinding);
    VENUS_MASK_FEATURE(sparseResidencyBuffer);
    VENUS_MASK_FEATURE(sparseResidencyImage2D);
    VENUS_MASK_FEATURE(sparseResidencyImage3D);
    VENUS_MASK_FEATURE(sparseResidency2Samples);
    VENUS_MASK_FEATURE(sparseResidency4Samples);
    VENUS_MASK_FEATURE(sparseResidency8Samples);
    VENUS_MASK_FEATURE(sparseResidency16Samples);
    VENUS_MASK_FEATURE(sparseResidencyAliased);
    VENUS_MASK_FEATURE(variableMultisampleRate);
    VENUS_MASK_FEATURE(inheritedQueries);

#undef VENUS_MASK_FEATURE
}

/* --- Device lifecycle --------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDevice *pDevice) {
    (void)pAllocator;
    if (!physicalDevice || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_instance *parent = (struct venus_instance *)physicalDevice;
    /* W4.8: previously bailed when VENUS_READY was off, leaving Mesa+Zink
     * with no VkDevice and zink_create_screen returning NULL. Now we always
     * allocate a guest-local device — every encoder already has a fallback
     * path, and the W3b.5 SHM upgrade in vkBindImageMemory + the W4.8 SHM
     * clear in vkQueueSubmit don't need a host wire to work. */

    struct venus_device *dev = malloc(sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(dev, 0, sizeof(*dev));
    set_loader_magic_value(dev);
    dev->parent = parent;
    set_loader_magic_value(&dev->queue_loader_data);
    dev->host_handle = 0;

    if (parent->wire &&
        (parent->caps & VENUS_GPU_CAP_VENUS_READY) &&
        parent->ctx_id > 0 && parent->host_handle != 0 &&
        parent->phys_handle != 0 && pCreateInfo) {
        uint64_t host_dev = 0;
        const VkDeviceCreateInfo *host_ci = pCreateInfo;
        VkDeviceCreateInfo host_ci_storage;
        VkPhysicalDeviceFeatures host_features_storage;

        if (pCreateInfo->pEnabledFeatures && parent->host_features_valid) {
            host_features_storage = *pCreateInfo->pEnabledFeatures;
            venus_mask_host_enabled_features(&host_features_storage,
                                             &parent->host_features);
            host_ci_storage = *pCreateInfo;
            host_ci_storage.pEnabledFeatures = &host_features_storage;
            host_ci = &host_ci_storage;
            printf("[VENUSD] feature mask geom=%u->%u cull=%u->%u f64=%u->%u\n",
                   pCreateInfo->pEnabledFeatures->geometryShader,
                   host_features_storage.geometryShader,
                   pCreateInfo->pEnabledFeatures->shaderCullDistance,
                   host_features_storage.shaderCullDistance,
                   pCreateInfo->pEnabledFeatures->shaderFloat64,
                   host_features_storage.shaderFloat64);
        }

        int rc = venus_cmd_encode_CreateDevice(parent->wire,
                                               parent->phys_handle,
                                               host_ci,
                                               &host_dev);
        printf("[VENUSD] host CreateDevice rc=%d phys=%llu dev=%llu ctx=%d\n",
               rc, (unsigned long long)parent->phys_handle,
               (unsigned long long)host_dev, parent->ctx_id);
        if (rc == VK_SUCCESS && host_dev != 0)
            dev->host_handle = host_dev;
    } else if (parent->wire && parent->host_handle != 0 &&
               !venus_log_device_host_disabled) {
        venus_log_device_host_disabled = 1u;
        printf("[VENUSD] host CreateDevice skipped insthost=%llu phys=%llu ctx=%d\n",
               (unsigned long long)parent->host_handle,
               (unsigned long long)parent->phys_handle, parent->ctx_id);
    }

    if (!venus_log_device_create) {
        venus_log_device_create = 1u;
        printf("[VENUSD] create wire=%u caps=0x%x ctx=%d insthost=%llu devhost=%llu\n",
               parent->wire ? 1u : 0u, parent->caps, parent->ctx_id,
               (unsigned long long)parent->host_handle,
               (unsigned long long)dev->host_handle);
    }

    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device) return;
    struct venus_device *dev = (struct venus_device *)device;

    /* Orphan-cleanup: apps SHOULD destroy children first (Vulkan spec), but
     * if they don't, do not leak host-side state. Walk both tables and issue
     * DestroyBuffer / FreeMemory wire calls for in-use slots with a real
     * host id, then drop local_ptr + mark slot free. */
    for (uint32_t i = 0; i < VENUS_MAX_BUF_OBJECTS; i++) {
        struct venus_buffer *b = &dev->buffers[i];
        if (!b->in_use) continue;
        if (dev->parent && dev->parent->wire && b->host_id != 0 && dev->host_handle != 0) {
            (void)venus_cmd_encode_DestroyBuffer(dev->parent->wire,
                                                 dev->host_handle, b->host_id);
        }
        b->in_use = 0;
        b->host_id = 0;
    }
    for (uint32_t i = 0; i < VENUS_MAX_MEM_OBJECTS; i++) {
        struct venus_memory *m = &dev->memories[i];
        if (!m->in_use) continue;
        if (dev->parent && dev->parent->wire && m->host_id != 0 && dev->host_handle != 0) {
            (void)venus_cmd_encode_FreeMemory(dev->parent->wire,
                                              dev->host_handle, m->host_id);
        }
        venus_memory_release_local(m);
        m->in_use = 0;
        m->host_id = 0;
        m->size = 0;
    }

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        (void)venus_cmd_encode_DestroyDevice(dev->parent->wire, dev->host_handle);
    }
    free(dev);
}

VKAPI_ATTR void VKAPI_CALL
venus_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                     uint32_t queueIndex, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    struct venus_device *dev = (struct venus_device *)device;
    struct venus_queue *q = 0;
    int rc = venus_cmd_encode_GetDeviceQueue(dev, queueFamilyIndex,
                                             queueIndex, &q);
    if (rc != 0 || !q) {
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    printf("[VGQ] queue=%p owner=%p host=%llu family=%u index=%u\n",
           (void *)q, (void *)q->owner,
           (unsigned long long)q->host_id,
           queueFamilyIndex, queueIndex);
    *pQueue = (VkQueue)q;
}

/* --- Memory ------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_AllocateMemory(VkDevice device,
                     const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkDeviceMemory *pMemory) {
    venus_dev_log("[VENUSdev] AllocateMemory enter\n");
    (void)pAllocator;
    if (!device || !pAllocateInfo || !pMemory)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_device *dev = (struct venus_device *)device;
    int slot = venus_mem_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;

    struct venus_memory *m = &dev->memories[slot];
    memset(m, 0, sizeof(*m));
    m->in_use     = 1;
    m->size       = pAllocateInfo->allocationSize;
    m->type_index = pAllocateInfo->memoryTypeIndex;
    m->host_id    = 0;
    m->mapped     = 0;

    if (m->size == 0 || m->size == 0xCDCDCDCDCDCDCDCDULL ||
        m->size > (2ULL * 1024ULL * 1024ULL * 1024ULL)) {
        printf("[VENUSdev] AllocateMemory invalid size=%llu type=%u\n",
               (unsigned long long)m->size, m->type_index);
        memset(m, 0, sizeof(*m));
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    /* Always allocate guest-local backing — this is what vkMapMemory
     * returns to the app in W3b.3. Real host-coherent mapping is W3b.4. */
    static uint32_t alloc_log_count;
    if (alloc_log_count < 16u) {
        printf("[VENUSdev] AllocateMemory slot=%d size=%llu type=%u\n",
               slot, (unsigned long long)m->size, m->type_index);
        alloc_log_count++;
    }
    if (m->type_index == 1u)
        (void)venus_try_gpu_backing(dev, m);

    if (!m->local_ptr) {
        venus_dev_log("[VENUSdev] AllocateMemory malloc begin\n");
        m->local_ptr = malloc(m->size ? m->size : 1);
        venus_dev_log("[VENUSdev] AllocateMemory malloc done\n");
    }
    if (!m->local_ptr) {
        m->in_use = 0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    /* Vulkan allocation contents are undefined. Zeroing large DXVK heap
     * chunks burns a long time in the guest and can look like an allocation
     * hang, so only preserve zero-init behavior for small test allocations. */
    if (m->size <= 64u * 1024u)
        memset(m->local_ptr, 0, m->size);
    else if (alloc_log_count < 16u)
        printf("[VENUSdev] AllocateMemory skip zero size=%llu\n",
               (unsigned long long)m->size);

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_mem_id = 0;
        int rc = venus_cmd_encode_AllocateMemory(dev->parent->wire,
                                                 dev->host_handle,
                                                 m->size, m->type_index,
                                                 &host_mem_id);
        if (rc == VK_SUCCESS && host_mem_id != 0) {
            m->host_id = host_mem_id;
        } else if (venus_log_mem_alloc_diag < 24u) {
            printf("[VMEMD] host alloc failed slot=%d rc=%d size=%llu type=%u host=%llu\n",
                   slot, rc, (unsigned long long)m->size, m->type_index,
                   (unsigned long long)host_mem_id);
        }
    }

    if (venus_log_mem_alloc_diag < 24u) {
        venus_log_mem_alloc_diag++;
        printf("[VMEMD] alloc slot=%d size=%llu type=%u host=%llu local=%u shm=%u gpu=%u res=%u\n",
               slot, (unsigned long long)m->size, m->type_index,
               (unsigned long long)m->host_id,
               m->local_ptr ? 1u : 0u, m->is_shm_backed,
               m->is_gpu_backed, m->gpu_res_id);
    }

    *pMemory = mem_slot_to_handle(dev, slot);
    venus_dev_log("[VENUSdev] AllocateMemory done\n");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_FreeMemory(VkDevice device, VkDeviceMemory memory,
                 const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !memory) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = mem_handle_to_slot(memory);
    if (slot < 0 || slot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    struct venus_memory *m = &dev->memories[slot];
    if (!m->in_use) return;

    if (dev->parent && dev->parent->wire && m->host_id != 0) {
        (void)venus_cmd_encode_FreeMemory(dev->parent->wire,
                                          dev->host_handle, m->host_id);
    }
    venus_memory_release_local(m);
    memset(m, 0, sizeof(*m));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_MapMemory(VkDevice device, VkDeviceMemory memory,
                VkDeviceSize offset, VkDeviceSize size,
                VkMemoryMapFlags flags, void **ppData) {
    (void)flags;
    if (!device || !memory || !ppData) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = mem_handle_to_slot(memory);
    if (slot < 0 || slot >= (int)VENUS_MAX_MEM_OBJECTS) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_memory *m = &dev->memories[slot];
    if (!m->in_use || !m->local_ptr) return VK_ERROR_MEMORY_MAP_FAILED;

    /* Issue wire call so the host can drop a reference on its side, but
     * we do NOT use the host-returned pointer — see encoder comment. */
    if (dev->parent && dev->parent->wire && m->host_id != 0) {
        uint64_t host_ptr = 0;
        uint64_t wsize = (size == VK_WHOLE_SIZE) ? m->size : (uint64_t)size;
        (void)venus_cmd_encode_MapMemory(dev->parent->wire,
                                         dev->host_handle, m->host_id,
                                         (uint64_t)offset, wsize,
                                         (uint32_t)flags, &host_ptr);
    }

    m->mapped = 1;
    *ppData = (uint8_t *)m->local_ptr + (uint64_t)offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_UnmapMemory(VkDevice device, VkDeviceMemory memory) {
    if (!device || !memory) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = mem_handle_to_slot(memory);
    if (slot < 0 || slot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    struct venus_memory *m = &dev->memories[slot];
    if (!m->in_use) return;
    m->mapped = 0;
    if (dev->parent && dev->parent->wire && m->host_id != 0) {
        (void)venus_cmd_encode_UnmapMemory(dev->parent->wire,
                                           dev->host_handle, m->host_id);
    }
}

/* --- Buffer ------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = venus_buf_slot_alloc(dev);
    if (slot < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;

    struct venus_buffer *b = &dev->buffers[slot];
    b->size           = pCreateInfo->size;
    b->usage          = pCreateInfo->usage;
    b->host_id        = 0;
    b->bound_mem_slot = -1;
    b->bound_offset   = 0;

    if (dev->parent && dev->parent->wire && dev->host_handle != 0) {
        uint64_t host_buf_id = 0;
        int rc = venus_cmd_encode_CreateBuffer(dev->parent->wire,
                                               dev->host_handle,
                                               pCreateInfo, &host_buf_id);
        if (rc == 0 && host_buf_id != 0) b->host_id = host_buf_id;
    }

    if (venus_log_buf_create_diag < 24u) {
        venus_log_buf_create_diag++;
        printf("[VBUFD] create slot=%d size=%llu usage=0x%x host=%llu\n",
               slot, (unsigned long long)b->size, b->usage,
               (unsigned long long)b->host_id);
    }

    *pBuffer = buf_slot_to_handle(dev, slot);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyBuffer(VkDevice device, VkBuffer buffer,
                    const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !buffer) return;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = buf_handle_to_slot(buffer);
    if (slot < 0 || slot >= (int)VENUS_MAX_BUF_OBJECTS) return;
    struct venus_buffer *b = &dev->buffers[slot];
    if (!b->in_use) return;
    if (dev->parent && dev->parent->wire && b->host_id != 0) {
        (void)venus_cmd_encode_DestroyBuffer(dev->parent->wire,
                                             dev->host_handle, b->host_id);
    }
    memset(b, 0, sizeof(*b));
}

VKAPI_ATTR void VKAPI_CALL
venus_GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                  VkMemoryRequirements *pMemoryRequirements) {
    if (!device || !buffer || !pMemoryRequirements) return;
    memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
    struct venus_device *dev = (struct venus_device *)device;
    int slot = buf_handle_to_slot(buffer);
    if (slot < 0 || slot >= (int)VENUS_MAX_BUF_OBJECTS) return;
    struct venus_buffer *b = &dev->buffers[slot];
    if (!b->in_use) return;

    if (dev->parent && dev->parent->wire && b->host_id != 0) {
        int rc = venus_cmd_encode_GetBufferMemoryRequirements(
                dev->parent->wire, dev->host_handle, b->host_id,
                pMemoryRequirements);
        if (rc == 0 && pMemoryRequirements->size != 0 &&
            pMemoryRequirements->alignment != 0 &&
            pMemoryRequirements->memoryTypeBits != 0)
            return;
    }
    /* Guest-local fallback: tight pack, 16-byte alignment, any type bit. */
    pMemoryRequirements->size           = b->size;
    pMemoryRequirements->alignment      = 16u;
    pMemoryRequirements->memoryTypeBits = 0xFFFFFFFFu;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_BindBufferMemory(VkDevice device, VkBuffer buffer,
                       VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    if (!device || !buffer || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int bslot = buf_handle_to_slot(buffer);
    int mslot = mem_handle_to_slot(memory);
    if (bslot < 0 || bslot >= (int)VENUS_MAX_BUF_OBJECTS) return VK_ERROR_INITIALIZATION_FAILED;
    if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_buffer *b = &dev->buffers[bslot];
    struct venus_memory *m = &dev->memories[mslot];
    if (!b->in_use || !m->in_use) return VK_ERROR_INITIALIZATION_FAILED;

    b->bound_mem_slot = mslot;
    b->bound_offset   = (uint64_t)memoryOffset;

    if (venus_log_buf_bind_diag < 24u) {
        venus_log_buf_bind_diag++;
        printf("[VBUFD] bind b=%d bhost=%llu m=%d mhost=%llu offset=%llu forward=%u\n",
               bslot, (unsigned long long)b->host_id,
               mslot, (unsigned long long)m->host_id,
               (unsigned long long)memoryOffset,
               (dev->parent && dev->parent->wire &&
                b->host_id != 0 && m->host_id != 0) ? 1u : 0u);
    }

    if (dev->parent && dev->parent->wire && b->host_id != 0 && m->host_id != 0) {
        int rc = venus_cmd_encode_BindBufferMemory(dev->parent->wire,
                                                   dev->host_handle,
                                                   b->host_id, m->host_id,
                                                   (uint64_t)memoryOffset);
        /* Transport negatives (-EINVAL / -ENOMEM / -EIO) must not leak to
         * the Vulkan caller as an undefined VkResult enum. Map to a known
         * failure; positive/zero is already a VkResult (0 == VK_SUCCESS). */
        if (rc < 0) return VK_ERROR_DEVICE_LOST;
        if (rc != 0) return (VkResult)rc;
    }
    return VK_SUCCESS;
}
