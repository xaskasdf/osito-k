/*
 * venus_w3b5_objects.c — W3b.5 public entry points + SHM upgrade path.
 *
 * Behavior: mirrors venus_w3b4_objects.c — every VKAPI_ATTR body here
 * attempts a wire round-trip only where the wave actually forwards
 * (nothing in W3b.5 today), and falls back to the guest-local encoder
 * for every path. This keeps hello-swapchain functional on --no-gl
 * boots while the wire is still ramping up.
 *
 * Handle tagging (per-object markers):
 *   VENUS_H_MARKER_SURFACE    0xFE00...
 *   VENUS_H_MARKER_QUEUE      0xFE01...   (dispatchable, but we tag anyway)
 *   VENUS_H_MARKER_FENCE      0xFE02...
 *   VENUS_H_MARKER_SEMA       0xFE03...
 *   VENUS_H_MARKER_SWAPCHAIN  0xFE04...
 *
 * Slot decode uses the usual (handle >> 48) & 0x0FFF shape to avoid
 * the W3b.3 marker-leak trap.
 *
 * SHM upgrade path (G5 integration):
 *   When vkBindImageMemory is called and the image is is_swapchain_owned,
 *   the memory slot's malloc-backed buffer (if any) is freed and a
 *   SYS_SHM_MKSURFACE surface is created in its place. The new mapped
 *   SHM pointer becomes m->local_ptr so that vkMapMemory continues to
 *   return the same pointer and the app can dump BGRA pixels into the
 *   compositor window buffer. We override (via --allow-multiple-definition
 *   at link time — same pattern W3b.4 uses) the W3b.4 BindImageMemory
 *   body to inject this upgrade.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern long  __syscall1(long, long);
extern long  __syscall3(long, long, long, long);

/* ---- Forward decls for encoders (defined in sibling TUs). --- */
extern int venus_cmd_encode_CreateOsitokCompositorSurface(
        struct venus_instance *, uint32_t, uint32_t, uint32_t, uint64_t *);
extern int venus_cmd_encode_DestroySurface(struct venus_instance *, uint64_t);
extern int venus_cmd_encode_GetDeviceQueue(
        struct venus_device *, uint32_t, uint32_t, struct venus_queue **);
extern int venus_cmd_encode_QueueSubmit(
        struct venus_device *, uint32_t, const VkSubmitInfo *, uint64_t);
extern int venus_cmd_encode_QueueWaitIdle(struct venus_device *);
extern int venus_cmd_encode_DeviceWaitIdle(struct venus_device *);
extern int venus_cmd_encode_CreateFence(
        struct venus_device *, const VkFenceCreateInfo *, int *);
extern int venus_cmd_encode_DestroyFence(struct venus_device *, int);
extern int venus_cmd_encode_ResetFences(
        struct venus_device *, uint32_t, const VkFence *);
extern int venus_cmd_encode_WaitForFences(
        struct venus_device *, uint32_t, const VkFence *,
        uint32_t, uint64_t);
extern int venus_cmd_encode_GetFenceStatus(struct venus_device *, VkFence);
extern int venus_cmd_encode_CreateSemaphore(
        struct venus_device *, const VkSemaphoreCreateInfo *, int *);
extern int venus_cmd_encode_DestroySemaphore(struct venus_device *, int);
extern int venus_cmd_encode_CreateSwapchainKHR(
        struct venus_device *, const VkSwapchainCreateInfoKHR *, int *);
extern int venus_cmd_encode_DestroySwapchainKHR(struct venus_device *, int);
extern int venus_cmd_encode_GetSwapchainImagesKHR(
        struct venus_device *, int, uint32_t *, VkImage *);
extern int venus_cmd_encode_AcquireNextImageKHR(
        struct venus_device *, int, uint64_t, VkSemaphore, VkFence,
        uint32_t *);
extern int venus_cmd_encode_QueuePresentKHR(
        struct venus_device *, const VkPresentInfoKHR *);
extern int venus_cmd_encode_GetPhysicalDeviceSurfaceCapabilitiesKHR(
        struct venus_instance *, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
extern int venus_cmd_encode_GetPhysicalDeviceSurfaceFormatsKHR(
        struct venus_instance *, VkSurfaceKHR,
        uint32_t *, VkSurfaceFormatKHR *);
extern int venus_cmd_encode_GetPhysicalDeviceSurfacePresentModesKHR(
        struct venus_instance *, VkSurfaceKHR,
        uint32_t *, VkPresentModeKHR *);
extern int venus_cmd_encode_GetPhysicalDeviceSurfaceSupportKHR(
        struct venus_instance *, uint32_t, VkSurfaceKHR, VkBool32 *);

/* ---- Handle markers + slot helpers. ---- */
#define VENUS_H_MARKER_SURFACE    0xFE00000000000000ull
#define VENUS_H_MARKER_FENCE      0xFE02000000000000ull
#define VENUS_H_MARKER_SEMA       0xFE03000000000000ull
#define VENUS_H_MARKER_SWAPCHAIN  0xFE04000000000000ull
#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define VENUS_H_PTR_MASK_W3B5     0x0000FFFFFFFFFFFFull

static inline uint64_t make_nd_handle(void *owner, int slot, uint64_t marker) {
    return ((uint64_t)(uint32_t)slot & VENUS_H_SLOT_MASK_W3B5) << 48
         | ((uint64_t)(uintptr_t)owner & VENUS_H_PTR_MASK_W3B5)
         | marker;
}

static inline int nd_handle_slot(uint64_t h) {
    return (int)((h >> 48) & VENUS_H_SLOT_MASK_W3B5);
}

/* ---- Syscall constants. ---- */
#define SYS_SHM_MAP              501L
#define SYS_SHM_MKSURFACE        506L
#define SHM_FMT_BGRA             0x41524742L   /* 'BGRA' LE */

/* ---- Surface (instance-scoped). ---- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateOsitokCompositorSurfaceKHR(
        VkInstance instance,
        const VkOsitoCompositorSurfaceCreateInfoOSITOK *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface) {
    (void)pAllocator;
    if (!instance || !pCreateInfo || !pSurface)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance *inst = (struct venus_instance *)instance;
    /* The OsitoK extension uses `shmHandle` to carry a caller-provided
     * window id OR an existing SHM handle. For W3b.5 we interpret it
     * as an opaque window_id and create fresh SHM surfaces lazily at
     * BindImageMemory time. Extent defaults come from the plan's
     * VkOsitokCompositorSurfaceCreateInfo (initial_width/height); we
     * encode a W3b.5-extended struct by padding after shmHandle. To
     * stay ABI-compatible with the existing 1000710000 struct, we
     * fall back to 512x512 if the caller passes the short form. */
    uint32_t window_id = pCreateInfo->shmHandle;
    uint32_t w = 512u, h = 512u;
    /* Best-effort probe of the extended fields: if the pNext/flags
     * area fits a width/height appendix, honor it. Since the short
     * form only has {sType, pNext, flags, shmHandle} (32 bytes), any
     * caller wanting custom extent sets pNext to a VkExtent2D inside
     * the same allocation. Skip parsing that for now — default 512. */
    uint64_t sh = 0;
    int rc = venus_cmd_encode_CreateOsitokCompositorSurface(
            inst, window_id, w, h, &sh);
    if (rc != 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pSurface = (VkSurfaceKHR)sh;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                        const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!instance || !surface) return;
    struct venus_instance *inst = (struct venus_instance *)instance;
    (void)venus_cmd_encode_DestroySurface(inst, (uint64_t)surface);
}

/* ---- Queue + device-wait. ---- */

VKAPI_ATTR void VKAPI_CALL
venus_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                     uint32_t queueIndex, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    struct venus_device *dev = (struct venus_device *)device;
    struct venus_queue *q = 0;
    int rc = venus_cmd_encode_GetDeviceQueue(dev, queueFamilyIndex, queueIndex, &q);
    if (rc != 0 || !q) {
        *pQueue = VK_NULL_HANDLE;
        return;
    }
    *pQueue = (VkQueue)q;
}

static inline struct venus_device *queue_to_dev(VkQueue q) {
    if (!q) return 0;
    struct venus_queue *vq = (struct venus_queue *)q;
    return vq->owner;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_QueueSubmit(VkQueue queue, uint32_t submitCount,
                  const VkSubmitInfo *pSubmits, VkFence fence) {
    struct venus_device *dev = queue_to_dev(queue);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    int rc = venus_cmd_encode_QueueSubmit(dev, submitCount, pSubmits,
                                          (uint64_t)fence);
    if (rc < 0) return VK_ERROR_DEVICE_LOST;
    return (VkResult)rc;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_QueueWaitIdle(VkQueue queue) {
    struct venus_device *dev = queue_to_dev(queue);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    return (VkResult)venus_cmd_encode_QueueWaitIdle(dev);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_DeviceWaitIdle(VkDevice device) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    return (VkResult)venus_cmd_encode_DeviceWaitIdle(dev);
}

/* ---- Fence. ---- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pFence) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = -1;
    int rc = venus_cmd_encode_CreateFence(dev, pCreateInfo, &slot);
    if (rc != 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pFence = (VkFence)make_nd_handle(dev, slot, VENUS_H_MARKER_FENCE);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyFence(VkDevice device, VkFence fence,
                   const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !fence) return;
    struct venus_device *dev = (struct venus_device *)device;
    (void)venus_cmd_encode_DestroyFence(dev, nd_handle_slot((uint64_t)fence));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_ResetFences(VkDevice device, uint32_t count, const VkFence *pFences) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    return (VkResult)venus_cmd_encode_ResetFences(dev, count, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_WaitForFences(VkDevice device, uint32_t count, const VkFence *pFences,
                    VkBool32 waitAll, uint64_t timeout) {
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int rc = venus_cmd_encode_WaitForFences(dev, count, pFences,
                                            (uint32_t)waitAll, timeout);
    return (VkResult)rc;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_GetFenceStatus(VkDevice device, VkFence fence) {
    if (!device || !fence) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int rc = venus_cmd_encode_GetFenceStatus(dev, fence);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return (VkResult)rc;
}

/* ---- Semaphore. ---- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateSemaphore(VkDevice device,
                      const VkSemaphoreCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkSemaphore *pSemaphore) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = -1;
    int rc = venus_cmd_encode_CreateSemaphore(dev, pCreateInfo, &slot);
    if (rc != 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pSemaphore = (VkSemaphore)make_nd_handle(dev, slot, VENUS_H_MARKER_SEMA);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroySemaphore(VkDevice device, VkSemaphore semaphore,
                       const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !semaphore) return;
    struct venus_device *dev = (struct venus_device *)device;
    (void)venus_cmd_encode_DestroySemaphore(dev, nd_handle_slot((uint64_t)semaphore));
}

/* ---- Swapchain. ---- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateSwapchainKHR(VkDevice device,
                         const VkSwapchainCreateInfoKHR *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkSwapchainKHR *pSwapchain) {
    (void)pAllocator;
    if (!device || !pCreateInfo || !pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int slot = -1;
    int rc = venus_cmd_encode_CreateSwapchainKHR(dev, pCreateInfo, &slot);
    if (rc != 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pSwapchain = (VkSwapchainKHR)make_nd_handle(dev, slot, VENUS_H_MARKER_SWAPCHAIN);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device || !swapchain) return;
    struct venus_device *dev = (struct venus_device *)device;
    (void)venus_cmd_encode_DestroySwapchainKHR(
            dev, nd_handle_slot((uint64_t)swapchain));
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                            uint32_t *pCount, VkImage *pImages) {
    if (!device || !swapchain || !pCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int rc = venus_cmd_encode_GetSwapchainImagesKHR(
            dev, nd_handle_slot((uint64_t)swapchain), pCount, pImages);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return (VkResult)rc;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                          uint64_t timeout, VkSemaphore semaphore,
                          VkFence fence, uint32_t *pImageIndex) {
    if (!device || !swapchain || !pImageIndex)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int rc = venus_cmd_encode_AcquireNextImageKHR(
            dev, nd_handle_slot((uint64_t)swapchain), timeout,
            semaphore, fence, pImageIndex);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return (VkResult)rc;
}

/* ---- Present. ---- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    struct venus_device *dev = queue_to_dev(queue);
    if (!dev || !pPresentInfo) return VK_ERROR_INITIALIZATION_FAILED;
    int rc = venus_cmd_encode_QueuePresentKHR(dev, pPresentInfo);
    if (rc < 0) return VK_ERROR_DEVICE_LOST;
    return (VkResult)rc;
}

/* ---- Physical-device surface queries. ---- */

VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR *pCaps) {
    if (!physicalDevice || !surface || !pCaps)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* The physicalDevice is the instance pointer (W3a single-device model). */
    struct venus_instance *inst = (struct venus_instance *)physicalDevice;
    int rc = venus_cmd_encode_GetPhysicalDeviceSurfaceCapabilitiesKHR(
            inst, surface, pCaps);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfaceFormatsKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
        uint32_t *pFormatCount, VkSurfaceFormatKHR *pFormats) {
    if (!physicalDevice || !pFormatCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance *inst = (struct venus_instance *)physicalDevice;
    int rc = venus_cmd_encode_GetPhysicalDeviceSurfaceFormatsKHR(
            inst, surface, pFormatCount, pFormats);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return (VkResult)rc;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfacePresentModesKHR(
        VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
        uint32_t *pModeCount, VkPresentModeKHR *pModes) {
    if (!physicalDevice || !pModeCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance *inst = (struct venus_instance *)physicalDevice;
    int rc = venus_cmd_encode_GetPhysicalDeviceSurfacePresentModesKHR(
            inst, surface, pModeCount, pModes);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return (VkResult)rc;
}

VKAPI_ATTR VkResult VKAPI_CALL
venus_GetPhysicalDeviceSurfaceSupportKHR(
        VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
        VkSurfaceKHR surface, VkBool32 *pSupported) {
    if (!physicalDevice || !pSupported)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance *inst = (struct venus_instance *)physicalDevice;
    int rc = venus_cmd_encode_GetPhysicalDeviceSurfaceSupportKHR(
            inst, queueFamilyIndex, surface, pSupported);
    if (rc < 0) return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

/* ---- BindImageMemory override — SHM upgrade for swapchain images.
 *
 * When the bound image is is_swapchain_owned and the memory slot is
 * NOT yet SHM-backed, we upgrade it in-place. The caller's existing
 * vkMapMemory pointer (local_ptr) is replaced; the new pointer is
 * the SHM mapping. This is safe because the caller hasn't yet called
 * vkMapMemory for swapchain images per our suggested usage — but
 * even if they did, the old malloc buffer had undefined content
 * anyway (we memset to 0 at AllocateMemory time).
 *
 * Ordering: this body is defined with the same symbol
 * `venus_BindImageMemory` used by venus_w3b4_objects.c. When linked
 * with --allow-multiple-definition, the linker picks the first
 * definition seen. We place this TU BEFORE venus_w3b4_objects.c in
 * the Makefile SRC list (T9). If that ordering is ever reversed the
 * swapchain upgrade breaks silently — guard is documented in the
 * plan invariants.
 * ---- */
VKAPI_ATTR VkResult VKAPI_CALL
venus_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                      VkDeviceSize memoryOffset) {
    if (!device || !image || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_device *dev = (struct venus_device *)device;
    int islot = (int)(((uint64_t)image  >> 48) & VENUS_H_SLOT_MASK_W3B5);
    int mslot = (int)(((uint64_t)memory >> 48) & VENUS_H_SLOT_MASK_W3B5);
    if (islot < 0 || islot >= (int)VENUS_MAX_IMAGE_OBJECTS)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_image  *img = &dev->images[islot];
    struct venus_memory *m   = &dev->memories[mslot];
    if (!img->in_use || !m->in_use) return VK_ERROR_INITIALIZATION_FAILED;

    img->bound_mem_slot = mslot;
    img->bound_offset   = (uint64_t)memoryOffset;

    /* SHM upgrade for swapchain-owned images. */
    if (img->is_swapchain_owned && !m->is_shm_backed) {
        uint32_t w = img->width, h = img->height;
        long shm = __syscall3(SYS_SHM_MKSURFACE, (long)w, (long)h, SHM_FMT_BGRA);
        if (shm > 0) {
            long mapped = __syscall1(SYS_SHM_MAP, shm);
            if (mapped != 0) {
                /* Free the old malloc-backed buffer (if any). */
                if (m->local_ptr && !m->is_shm_backed) {
                    free(m->local_ptr);
                    m->local_ptr = 0;
                }
                m->local_ptr     = (void *)(uintptr_t)mapped;
                m->is_shm_backed = 1;
                m->shm_handle    = (uint32_t)shm;
                m->shm_width     = w;
                m->shm_height    = h;
            }
        }
    }

    /* Keep wire forwarding only when both sides have a real host id. */
    if (dev->parent && dev->parent->wire && img->host_id != 0 && m->host_id != 0) {
        extern int venus_cmd_encode_BindImageMemory(struct venus_wire *,
                uint64_t, uint64_t, uint64_t, uint64_t);
        int rc = venus_cmd_encode_BindImageMemory(dev->parent->wire,
                                                  dev->host_handle,
                                                  img->host_id, m->host_id,
                                                  (uint64_t)memoryOffset);
        if (rc < 0) return VK_ERROR_DEVICE_LOST;
        if (rc != 0) return (VkResult)rc;
    }
    return VK_SUCCESS;
}
