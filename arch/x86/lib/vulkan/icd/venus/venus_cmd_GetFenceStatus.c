/*
 * Encoder for vkGetFenceStatus. Returns 0 (VK_SUCCESS) if signaled,
 * 3 (VK_NOT_READY) if not. Guest-local only.
 * See master plan §W3b.5 T3.
 */
#include "venus.h"

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

int venus_cmd_encode_GetFenceStatus(
        struct venus_device *dev, VkFence fence) {
    if (!dev || !fence) return -22;
    int slot = (int)(((uint64_t)fence >> 48) & VENUS_H_SLOT_MASK_W3B5);
    if (slot < 0 || slot >= (int)VENUS_MAX_FENCE_OBJECTS) return -22;
    struct venus_fence *f = &dev->fences[slot];
    if (!f->in_use) return -22;
    return f->signaled ? 0 : 3;  /* VK_SUCCESS : VK_NOT_READY */
}
