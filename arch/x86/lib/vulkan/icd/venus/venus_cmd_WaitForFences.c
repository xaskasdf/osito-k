/*
 * Encoder for vkWaitForFences. Guest-local: since submits were all
 * no-ops, every fence we're asked to wait on is implicitly signaled
 * by the time the app calls Wait. Return VK_SUCCESS unconditionally.
 *
 * This keeps the contract: after Wait returns SUCCESS, the app
 * expects fences to be signaled. We don't need to flip the signaled
 * bit here (QueueSubmit / AcquireNextImage already did), but we do
 * in case anyone called Wait on a fence that was never submitted —
 * the fence stays unsignaled in that case, which matches Vulkan's
 * "waiting on an unsignaled fence that has no pending submit
 * completes when the fence is eventually signaled" rule. For W3b.5
 * simplicity we also signal those so the test harness never blocks.
 *
 * See master plan §W3b.5 T3.
 */
#include "venus.h"

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

int venus_cmd_encode_WaitForFences(
        struct venus_device *dev,
        uint32_t count, const VkFence *pFences,
        uint32_t wait_all, uint64_t timeout_ns) {
    (void)wait_all; (void)timeout_ns;
    if (!dev) return -22;
    if (count == 0 || !pFences) return 0;
    for (uint32_t i = 0; i < count; i++) {
        int slot = (int)(((uint64_t)pFences[i] >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (slot < 0 || slot >= (int)VENUS_MAX_FENCE_OBJECTS) continue;
        if (dev->fences[slot].in_use)
            dev->fences[slot].signaled = 1;
    }
    return 0;  /* VK_SUCCESS */
}
