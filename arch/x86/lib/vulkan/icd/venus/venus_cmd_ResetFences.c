/*
 * Encoder for vkResetFences. Clear the `signaled` field on each fence
 * slot in the input array. Guest-local only.
 * See master plan §W3b.5 T3.
 */
#include "venus.h"

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

int venus_cmd_encode_ResetFences(
        struct venus_device *dev,
        uint32_t count, const VkFence *pFences) {
    if (!dev) return -22;
    if (count == 0) return 0;
    if (!pFences) return -22;
    for (uint32_t i = 0; i < count; i++) {
        int slot = (int)(((uint64_t)pFences[i] >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (slot < 0 || slot >= (int)VENUS_MAX_FENCE_OBJECTS) continue;
        if (dev->fences[slot].in_use)
            dev->fences[slot].signaled = 0;
    }
    return 0;
}
