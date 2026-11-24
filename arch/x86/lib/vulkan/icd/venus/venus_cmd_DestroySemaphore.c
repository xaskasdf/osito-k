/*
 * Encoder for vkDestroySemaphore. Guest-local: clear the slot.
 * See master plan §W3b.5 T3.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_DestroySemaphore(struct venus_device *dev, int slot) {
    if (!dev) return -22;
    if (slot < 0 || slot >= (int)VENUS_MAX_SEMA_OBJECTS) return -22;
    struct venus_semaphore *s = &dev->semaphores[slot];
    if (!s->in_use) return 0;
    memset(s, 0, sizeof(*s));
    return 0;
}
