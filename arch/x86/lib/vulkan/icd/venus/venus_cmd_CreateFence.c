/*
 * Encoder for vkCreateFence over venus. Guest-local slot allocator;
 * the returned VkFence handle encodes the slot + owning device ptr.
 *
 * Fence signaled state: starts at `flags & VK_FENCE_CREATE_SIGNALED_BIT`
 * per spec. WaitForFences in W3b.5 returns SUCCESS immediately, but
 * GetFenceStatus honors the bit so apps that check state before
 * their first submit see consistent results.
 *
 * See master plan §W3b.5 T3.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_CreateFence(
        struct venus_device *dev,
        const VkFenceCreateInfo *pCreateInfo,
        int *out_slot) {
    if (!dev || !pCreateInfo || !out_slot) return -22;

    for (uint32_t i = 0; i < VENUS_MAX_FENCE_OBJECTS; i++) {
        struct venus_fence *f = &dev->fences[i];
        if (!f->in_use) {
            memset(f, 0, sizeof(*f));
            f->in_use   = 1;
            f->signaled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) ? 1u : 0u;
            *out_slot = (int)i;
            return 0;
        }
    }
    return -12;
}
