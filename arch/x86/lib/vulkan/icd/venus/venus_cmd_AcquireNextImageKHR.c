/*
 * Encoder for vkAcquireNextImageKHR. Guest-local: bump current_index
 * modulo image_count, write *pImageIndex. Signal the provided
 * semaphore and/or fence.
 *
 * See master plan §W3b.5 T4.
 */
#include "venus.h"

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

int venus_cmd_encode_AcquireNextImageKHR(
        struct venus_device *dev, int sc_slot, uint64_t timeout_ns,
        VkSemaphore semaphore, VkFence fence,
        uint32_t *out_image_index) {
    (void)timeout_ns;
    if (!dev || !out_image_index) return -22;
    if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) return -22;
    struct venus_swapchain *sc = &dev->swapchains[sc_slot];
    if (!sc->in_use) return -22;
    if (sc->image_count == 0) return -22;

    uint32_t idx = sc->current_index;
    sc->current_index = (sc->current_index + 1u) % sc->image_count;
    *out_image_index = idx;

    /* Signal semaphore + fence if provided. */
    if (semaphore) {
        int slot = (int)(((uint64_t)semaphore >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (slot >= 0 && slot < (int)VENUS_MAX_SEMA_OBJECTS &&
            dev->semaphores[slot].in_use)
            dev->semaphores[slot].signaled = 1;
    }
    if (fence) {
        int slot = (int)(((uint64_t)fence >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (slot >= 0 && slot < (int)VENUS_MAX_FENCE_OBJECTS &&
            dev->fences[slot].in_use)
            dev->fences[slot].signaled = 1;
    }
    return 0;
}
