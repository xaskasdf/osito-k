/*
 * Encoder for vkQueuePresentKHR.
 *
 * This is the real integration point — it bridges Vulkan swapchain
 * presentation to the OsitoK compositor via SYS_GUI_FLIP (507).
 *
 * For each (swapchain, image_index) pair in pPresentInfo:
 *   1. Look up the swapchain slot.
 *   2. Look up the image slot at image_index.
 *   3. Look up the memory bound to the image (venus_memory slot from
 *      img->bound_mem_slot set in vkBindImageMemory).
 *   4. If that memory slot is SHM-backed (is_shm_backed=1 and
 *      shm_handle != 0), issue syscall(SYS_GUI_FLIP=507, shm_handle).
 *   5. If NOT SHM-backed (defensive path — happens when the app
 *      allocated non-swapchain memory, or the bind path didn't run
 *      its upgrade logic), silently return VK_SUCCESS. No crash.
 *
 * The app picks one flip per vkQueuePresentKHR call. Semaphores are
 * waited on guest-local (no-op, since no real GPU work is pending).
 *
 * See master plan §W3b.5 T5 + the SHM integration note in §G5.
 */
#include "venus.h"

extern long __syscall1(long, long);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define SYS_GUI_FLIP              507L

int venus_cmd_encode_QueuePresentKHR(
        struct venus_device *dev,
        const VkPresentInfoKHR *pPresentInfo) {
    if (!dev || !pPresentInfo) return -22;

    /* Wait semaphores: guest-local — clear the "signaled" bit to
     * model consumption. No actual wait (there's no host GPU work). */
    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
        VkSemaphore sh = pPresentInfo->pWaitSemaphores[i];
        if (!sh) continue;
        int slot = (int)(((uint64_t)sh >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (slot >= 0 && slot < (int)VENUS_MAX_SEMA_OBJECTS &&
            dev->semaphores[slot].in_use)
            dev->semaphores[slot].signaled = 0;
    }

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        VkSwapchainKHR sch = pPresentInfo->pSwapchains[i];
        uint32_t idx = pPresentInfo->pImageIndices[i];
        int sc_slot = (int)(((uint64_t)sch >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) continue;
        struct venus_swapchain *sc = &dev->swapchains[sc_slot];
        if (!sc->in_use) continue;
        if (idx >= sc->image_count) continue;
        int img_slot = sc->image_slots[idx];
        if (img_slot < 0 || img_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) continue;
        struct venus_image *img = &dev->images[img_slot];
        if (!img->in_use) continue;
        int mslot = img->bound_mem_slot;
        if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) continue;
        struct venus_memory *m = &dev->memories[mslot];
        if (!m->in_use) continue;

        if (m->is_shm_backed && m->shm_handle != 0) {
            (void)__syscall1(SYS_GUI_FLIP, (long)(uint32_t)m->shm_handle);
        }
        /* else: memory not SHM-backed — silent skip. No crash. Caller's
         * responsibility to bind an SHM-upgradable memory to the image
         * before present. */

        /* Store results[i] if requested. */
        if (pPresentInfo->pResults)
            pPresentInfo->pResults[i] = VK_SUCCESS;
    }
    return 0;
}
