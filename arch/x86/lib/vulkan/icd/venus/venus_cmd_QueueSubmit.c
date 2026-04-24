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

int venus_cmd_encode_QueueSubmit(
        struct venus_device *dev,
        uint32_t submitCount, const VkSubmitInfo *pSubmits,
        uint64_t fence_handle) {
    (void)submitCount; (void)pSubmits;
    if (!dev) return -22;

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
