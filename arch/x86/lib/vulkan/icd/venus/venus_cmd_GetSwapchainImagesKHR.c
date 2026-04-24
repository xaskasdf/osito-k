/*
 * Encoder for vkGetSwapchainImagesKHR. Guest-local: return the
 * pre-allocated image slots as VkImage handles (image marker tag,
 * same layout as venus_w3b5_objects.c uses for other objects).
 *
 * See master plan §W3b.5 T4.
 */
#include "venus.h"

/* Markers match venus_w3b4_objects.c. */
#define VENUS_H_MARKER_IMAGE      0xE000000000000000ull
#define VENUS_H_SLOT_MASK_W3B     0x0FFFull
#define VENUS_H_PTR_MASK_W3B      0x0000FFFFFFFFFFFFull

int venus_cmd_encode_GetSwapchainImagesKHR(
        struct venus_device *dev, int sc_slot,
        uint32_t *pCount, VkImage *pImages) {
    if (!dev || !pCount) return -22;
    if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) return -22;
    struct venus_swapchain *sc = &dev->swapchains[sc_slot];
    if (!sc->in_use) return -22;

    uint32_t count = sc->image_count;

    if (!pImages) {
        *pCount = count;
        return 0;
    }

    uint32_t n = (*pCount < count) ? *pCount : count;
    for (uint32_t i = 0; i < n; i++) {
        int islot = sc->image_slots[i];
        if (islot < 0) { pImages[i] = VK_NULL_HANDLE; continue; }
        uint64_t h = ((uint64_t)(uint32_t)islot & VENUS_H_SLOT_MASK_W3B) << 48
                   | ((uint64_t)(uintptr_t)dev & VENUS_H_PTR_MASK_W3B)
                   | VENUS_H_MARKER_IMAGE;
        pImages[i] = (VkImage)h;
    }
    if (*pCount < count) {
        *pCount = n;
        return 5;  /* VK_INCOMPLETE */
    }
    *pCount = n;
    return 0;
}
