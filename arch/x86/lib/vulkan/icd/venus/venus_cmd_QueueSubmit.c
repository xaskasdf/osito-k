/*
 * Encoder for vkQueueSubmit over venus.
 *
 * Guest-local in W3b.5: when no wire, we simply mark all signal fences
 * + signal semaphores as "signaled" and return VK_SUCCESS. Submitted
 * command buffers were already no-ops in W3b.4 without a wire, so from
 * the app's POV submit → idle is effectively instant.
 *
 * When the wire IS live, we do NOT forward anything in W3b.5 (that's
 * W3b.6). Returning VK_SUCCESS without the host round-trip is fine
 * because the host's view of the device stays consistent as long as
 * all commands were guest-local no-ops.
 *
 * The fence/semaphore lookup uses the same (handle >> 48) & 0x0FFF
 * decoder used by every other W3b.* slot type — see venus_w3b5_objects.c
 * for the helpers.
 *
 * See master plan §W3b.5 T2.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

/* W4.8 — fill an SHM-backed memory buffer with a 32-bit BGRA color. */
static void w48_fill_shm(struct venus_memory *m,
                         struct venus_image  *img,
                         uint32_t bgra) {
    if (!m || !m->is_shm_backed || !m->local_ptr) return;
    uint32_t w = m->shm_width  ? m->shm_width  : (img ? img->width  : 0u);
    uint32_t h = m->shm_height ? m->shm_height : (img ? img->height : 0u);
    if (w == 0 || h == 0) return;
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    uint32_t *fb = (uint32_t *)m->local_ptr;
    for (uint64_t k = 0; k < pixels; k++) fb[k] = bgra;
}

int venus_cmd_encode_QueueSubmit(
        struct venus_device *dev,
        uint32_t submitCount, const VkSubmitInfo *pSubmits,
        uint64_t fence_handle) {
    if (!dev) return -22;

    /* W4.8 — execute recorded clears. For each submitted cmd buffer that
     * has a recorded clear color, find the target image's bound memory
     * slot. If it's SHM-backed, fill the buffer with the clear color.
     * Honor recorded_clear_image_slot first (set by CmdClearColorImage
     * AND CmdBeginRenderPass with LOAD_OP_CLEAR), then fall back to
     * last_drawn_image_slot. */
    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo *si = &pSubmits[i];
            if (!si->pCommandBuffers) continue;
            for (uint32_t j = 0; j < si->commandBufferCount; j++) {
                VkCommandBuffer cb_h = si->pCommandBuffers[j];
                if (!cb_h) continue;
                struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
                if (!vcb->in_use || !vcb->recorded_has_clear) continue;
                int islot = vcb->recorded_clear_image_slot;
                if (islot < 0) islot = vcb->last_drawn_image_slot;
                if (islot < 0 || islot >= (int)VENUS_MAX_IMAGE_OBJECTS) continue;
                struct venus_image *img = &dev->images[islot];
                if (!img->in_use) continue;
                int mslot = img->bound_mem_slot;
                if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) continue;
                struct venus_memory *m = &dev->memories[mslot];
                if (!m->in_use) continue;
                w48_fill_shm(m, img, vcb->recorded_clear_color);
            }
        }
    }

    /* Signal every signal semaphore referenced in each submit. */
    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo *si = &pSubmits[i];
            for (uint32_t j = 0; j < si->signalSemaphoreCount; j++) {
                VkSemaphore sh = si->pSignalSemaphores[j];
                if (!sh) continue;
                int slot = (int)(((uint64_t)sh >> 48) & VENUS_H_SLOT_MASK_W3B5);
                if (slot < 0 || slot >= (int)VENUS_MAX_SEMA_OBJECTS) continue;
                if (dev->semaphores[slot].in_use)
                    dev->semaphores[slot].signaled = 1;
            }
        }
    }

    /* Signal the fence if one was passed. */
    if (fence_handle) {
        int fslot = (int)(((uint64_t)fence_handle >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (fslot >= 0 && fslot < (int)VENUS_MAX_FENCE_OBJECTS &&
            dev->fences[fslot].in_use) {
            dev->fences[fslot].signaled = 1;
        }
    }
    return 0;
}
