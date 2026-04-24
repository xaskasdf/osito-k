/*
 * Encoder for vkCreateSemaphore. Guest-local slot allocator.
 * Semaphores start unsignaled per spec (no SEMAPHORE_CREATE flag
 * analog to FENCE_CREATE_SIGNALED_BIT exists in core Vulkan 1.0).
 *
 * See master plan §W3b.5 T3.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_CreateSemaphore(
        struct venus_device *dev,
        const VkSemaphoreCreateInfo *pCreateInfo,
        int *out_slot) {
    (void)pCreateInfo;
    if (!dev || !out_slot) return -22;
    for (uint32_t i = 0; i < VENUS_MAX_SEMA_OBJECTS; i++) {
        struct venus_semaphore *s = &dev->semaphores[i];
        if (!s->in_use) {
            memset(s, 0, sizeof(*s));
            s->in_use   = 1;
            s->signaled = 0;
            *out_slot = (int)i;
            return 0;
        }
    }
    return -12;
}
