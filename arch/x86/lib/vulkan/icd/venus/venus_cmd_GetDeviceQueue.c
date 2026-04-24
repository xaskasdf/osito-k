/*
 * Encoder for vkGetDeviceQueue over venus.
 *
 * Guest-local slot allocator: we never forward queue acquisition over
 * the wire in W3b.5. Real Mesa venus does forward, but the reply is
 * just a host-side id — useless to us until we pipe submits through
 * the wire (deferred to W3b.6). The handle returned from this encoder
 * is a dispatchable VkQueue pointer into venus_device->queues[slot].
 *
 * Assignment rule: the (queue_family_index, queue_index) pair maps
 * 1:1 to a slot; duplicate Get calls for the same pair return the
 * same slot. See master plan §W3b.5 T2.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_GetDeviceQueue(
        struct venus_device *dev,
        uint32_t queue_family_index, uint32_t queue_index,
        struct venus_queue **out_q) {
    if (!dev || !out_q) return -22;

    /* Return existing slot if the pair was already requested. */
    for (uint32_t i = 0; i < VENUS_MAX_QUEUE_OBJECTS; i++) {
        struct venus_queue *q = &dev->queues[i];
        if (q->in_use && q->queue_family_index == queue_family_index &&
            q->queue_index == queue_index) {
            *out_q = q;
            return 0;
        }
    }
    /* Allocate. */
    for (uint32_t i = 0; i < VENUS_MAX_QUEUE_OBJECTS; i++) {
        struct venus_queue *q = &dev->queues[i];
        if (!q->in_use) {
            memset(q, 0, sizeof(*q));
            set_loader_magic_value(&q->loader_data);
            q->owner = dev;
            q->queue_family_index = queue_family_index;
            q->queue_index = queue_index;
            q->in_use = 1;
            *out_q = q;
            return 0;
        }
    }
    return -12;
}
