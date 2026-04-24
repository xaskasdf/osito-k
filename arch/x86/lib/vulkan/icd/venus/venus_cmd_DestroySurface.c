/*
 * Guest-local "destroy" for VkSurfaceKHR. No wire traffic — frees the
 * slot in venus_instance->surfaces[]. See plan §W3b.5 T1.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

int venus_cmd_encode_DestroySurface(struct venus_instance *inst,
                                    uint64_t handle) {
    if (!inst) return -22;
    int slot = (int)(((uint64_t)handle >> 48) & VENUS_H_SLOT_MASK_W3B5);
    if (slot < 0 || slot >= (int)VENUS_MAX_SURFACE_OBJECTS) return -22;
    struct venus_surface *s = &inst->surfaces[slot];
    if (!s->in_use) return 0;  /* idempotent */
    memset(s, 0, sizeof(*s));
    return 0;
}
