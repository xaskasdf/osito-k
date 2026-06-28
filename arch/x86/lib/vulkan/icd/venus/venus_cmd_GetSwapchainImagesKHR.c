/*
 * Encoder for vkGetSwapchainImagesKHR. Guest-local: return the
 * pre-allocated image slots as VkImage handles (image marker tag,
 * same layout as venus_w3b5_objects.c uses for other objects).
 *
 * See master plan §W3b.5 T4.
 */
#include "venus.h"

extern int printf(const char *, ...);

/* Markers match venus_w3b4_objects.c. */
#define VENUS_H_MARKER_IMAGE      0xE000000000000000ull
#define VENUS_H_SLOT_MASK_W3B     0x0FFFull
#define VENUS_H_PTR_MASK_W3B      0x0000FFFFFFFFFFFFull

static uint32_t venus_gswi_log_count;

int venus_cmd_encode_GetSwapchainImagesKHR(
        struct venus_device *dev, int sc_slot,
        uint32_t *pCount, VkImage *pImages) {
    if (venus_gswi_log_count < 12u) {
        venus_gswi_log_count++;
        printf("[VSGIE] begin dev=%p slot=%d pCount=%p in=%u imgs=%p\n",
               (void *)dev, sc_slot, (void *)pCount,
               pCount ? *pCount : 0u, (void *)pImages);
    }
    if (!dev || !pCount) {
        printf("[VSGIE] invalid base dev=%p pCount=%p\n",
               (void *)dev, (void *)pCount);
        return -22;
    }
    if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) {
        printf("[VSGIE] invalid slot=%d max=%u\n",
               sc_slot, (unsigned)VENUS_MAX_SWAPCHAIN_OBJECTS);
        return -22;
    }
    struct venus_swapchain *sc = &dev->swapchains[sc_slot];
    if (!sc->in_use) {
        printf("[VSGIE] inactive slot=%d\n", sc_slot);
        return -22;
    }

    uint32_t count = sc->image_count;

    if (!pImages) {
        *pCount = count;
        if (venus_gswi_log_count < 12u) {
            venus_gswi_log_count++;
            printf("[VSGIE] count out=%u\n", count);
        }
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
        if (venus_gswi_log_count < 12u) {
            venus_gswi_log_count++;
            printf("[VSGIE] incomplete out=%u count=%u first=0x%llx\n",
                   n, count, n ? (unsigned long long)(uintptr_t)pImages[0] : 0ull);
        }
        return 5;  /* VK_INCOMPLETE */
    }
    *pCount = n;
    if (venus_gswi_log_count < 12u) {
        venus_gswi_log_count++;
        printf("[VSGIE] list out=%u first=0x%llx\n",
               n, n ? (unsigned long long)(uintptr_t)pImages[0] : 0ull);
    }
    return 0;
}
