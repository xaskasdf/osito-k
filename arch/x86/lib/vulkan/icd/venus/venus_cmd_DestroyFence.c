/*
 * Encoder for vkDestroyFence. Guest-local: clear the slot.
 * See master plan §W3b.5 T3.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_DestroyFence(struct venus_device *dev, int slot) {
    if (!dev) return -22;
    if (slot < 0 || slot >= (int)VENUS_MAX_FENCE_OBJECTS) return -22;
    struct venus_fence *f = &dev->fences[slot];
    if (!f->in_use) return 0;
    memset(f, 0, sizeof(*f));
    return 0;
}
