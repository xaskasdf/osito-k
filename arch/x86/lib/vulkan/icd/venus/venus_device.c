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

    if (parent->wire && pCreateInfo &&
        (parent->caps & VENUS_GPU_CAP_VENUS_READY) && parent->ctx_id > 0) {
        uint64_t dev_id = 0;
        int rc = venus_cmd_encode_CreateDevice(parent->wire, parent->host_handle,
                                               pCreateInfo, &dev_id);
        if (rc == 0 && dev_id != 0) {
            dev->host_handle = dev_id;
        }
        /* If the wire call failed we still return the guest-local device;
         * downstream ops will also fall back and hello-memory stays
         * functional end-to-end. */
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
        if (m->local_ptr) { free(m->local_ptr); m->local_ptr = 0; }
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

    for (uint32_t i = 0; i < VENUS_MAX_QUEUE_OBJECTS; i++) {
        struct venus_queue *q = &dev->queues[i];
        if (q->in_use && q->queue_family_index == queueFamilyIndex &&
            q->queue_index == queueIndex) {
            *pQueue = (VkQueue)q;
            return;
        }
    }

    for (uint32_t i = 0; i < VENUS_MAX_QUEUE_OBJECTS; i++) {
        struct venus_queue *q = &dev->queues[i];
        if (!q->in_use) {
            memset(q, 0, sizeof(*q));
            set_loader_magic_value(&q->loader_data);
            q->owner = dev;
            q->queue_family_index = queueFamilyIndex;
            q->queue_index = queueIndex;
            q->in_use = 1;
            printf("[VGQ] queue=%p owner=%p family=%u index=%u\n",
                   (void *)q, (void *)q->owner, queueFamilyIndex, queueIndex);
            *pQueue = (VkQueue)q;
            return;
        }
    }

    *pQueue = VK_NULL_HANDLE;
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
    m->size       = pAllocateInfo->allocationSize;
    m->type_index = pAllocateInfo->memoryTypeIndex;
    m->host_id    = 0;
    m->mapped     = 0;

    /* Always allocate guest-local backing — this is what vkMapMemory
     * returns to the app in W3b.3. Real host-coherent mapping is W3b.4. */
    printf("[VENUSdev] AllocateMemory slot=%d size=%llu type=%u\n",
           slot, (unsigned long long)m->size, m->type_index);
    venus_dev_log("[VENUSdev] AllocateMemory malloc begin\n");
    m->local_ptr = malloc(m->size ? m->size : 1);
    venus_dev_log("[VENUSdev] AllocateMemory malloc done\n");
    if (!m->local_ptr) {
        m->in_use = 0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    /* Vulkan allocation contents are undefined. Zeroing large DXVK heap
     * chunks burns a long time in the guest and can look like an allocation
     * hang, so only preserve zero-init behavior for small test allocations. */
    if (m->size <= 64u * 1024u)
        memset(m->local_ptr, 0, m->size);
    else
        printf("[VENUSdev] AllocateMemory skip zero size=%llu\n",
               (unsigned long long)m->size);

    /* Keep allocations guest-local for now. Some virglrenderer/Venus builds
     * accept vkCreateDevice and image creation but never reply to
     * vkAllocateMemory, which stalls DXVK's allocator while constructing the
     * first D3D11 backbuffer. BindImageMemory only forwards when both image and
     * memory have host ids, so host_id=0 intentionally selects the local path. */

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
    /* W3b.5: SHM-backed memory was obtained via SYS_SHM_CREATE +
     * SYS_SHM_MAP, not malloc. Unmap and destroy the kernel SHM object
     * instead of calling free() on the kernel-owned pointer. */
    if (m->is_shm_backed) {
        extern long __syscall1(long, long);
        if (m->local_ptr) {
            (void)__syscall1(502L /* SYS_SHM_UNMAP */,
                             (long)m->shm_handle);
            m->local_ptr = 0;
        }
        if (m->shm_handle) {
            (void)__syscall1(503L /* SYS_SHM_DESTROY */,
                             (long)m->shm_handle);
        }
    } else if (m->local_ptr) {
        free(m->local_ptr); m->local_ptr = 0;
    }
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
