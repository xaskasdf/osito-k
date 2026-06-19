/*
 * Encoder-less "surface" creation for the OsitoK compositor.
 *
 * VK_OSITOK_compositor_surface is an OsitoK-proprietary WSI extension.
 * Surfaces are purely guest-local bookkeeping: no wire round-trip, no
 * host Vulkan surface object. The compositor window is the actual
 * presentation target; on `vkQueuePresentKHR` we call `SYS_GUI_FLIP`
 * against the SHM handle bound to the swapchain's current image.
 *
 * This file provides the encoder-shaped API used by the ICD layer so
 * the calling convention mirrors W3b.4 encoders even though there is
 * no wire traffic. The real implementation lives here too (rather than
 * in venus_w3b5_objects.c) to keep the per-op TU layout consistent.
 *
 * Allocates a slot in the venus_instance->surfaces[] table and returns
 * a tagged VkSurfaceKHR handle encoding (slot << 48 | ptr & mask |
 * marker). Destroy clears the slot.
 *
 * See master plan §W3b.5 T1.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern void *memset(void *, int, unsigned long);

/* Surface uses marker 0 so `(handle >> 48) & 0x0FFF` returns the
 * original slot. Other W3b.5 objects use the free 0x1..0x3 high nibbles. */
#define VENUS_H_MARKER_SURFACE    0x0000000000000000ull
#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define VENUS_H_PTR_MASK_W3B5     0x0000FFFFFFFFFFFFull

static int surface_slot_alloc(struct venus_instance *inst) {
    for (uint32_t i = 0; i < VENUS_MAX_SURFACE_OBJECTS; i++) {
        if (!inst->surfaces[i].in_use) {
            inst->surfaces[i].in_use = 1;
            return (int)i;
        }
    }
    return -1;
}

int venus_cmd_encode_CreateOsitokCompositorSurface(
        struct venus_instance *inst,
        uint32_t window_id, uint32_t width, uint32_t height,
        uint64_t *out_handle) {
    if (!inst || !out_handle) return -22;

    int slot = surface_slot_alloc(inst);
    if (slot < 0) return -12;  /* -ENOMEM */

    struct venus_surface *s = &inst->surfaces[slot];
    s->window_id = window_id;
    s->width     = width;
    s->height    = height;

    uint64_t h = ((uint64_t)(uint32_t)slot & VENUS_H_SLOT_MASK_W3B5) << 48
               | ((uint64_t)(uintptr_t)inst & VENUS_H_PTR_MASK_W3B5)
               | VENUS_H_MARKER_SURFACE;
    *out_handle = h;
    return 0;
}
