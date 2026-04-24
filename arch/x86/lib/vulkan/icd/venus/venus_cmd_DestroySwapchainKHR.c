/*
 * Encoder for vkDestroySwapchainKHR. Guest-local: destroy each owned
 * image (frees the image slot; the app is responsible for freeing the
 * memory it allocated and bound, per Vulkan spec).
 *
 * Note: we do NOT touch the bound memory slots here. Per Vulkan
 * WSI convention, the memory backing swapchain images is managed by
 * the implementation — BUT in W3b.5 we let the app allocate + bind
 * the memory itself for simplicity (no vkGetMemoryRequirements for
 * swapchain images returns separate answers; we reuse the regular
 * vkGetImageMemoryRequirements path). The app is expected to call
 * vkFreeMemory on the memory handles it created. If the memory
 * happens to be SHM-backed (because it was bound to a swapchain
 * image), vkFreeMemory cleans up via its standard path.
 *
 * See master plan §W3b.5 T4.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

int venus_cmd_encode_DestroySwapchainKHR(struct venus_device *dev, int sc_slot) {
    if (!dev) return -22;
    if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) return -22;
    struct venus_swapchain *sc = &dev->swapchains[sc_slot];
    if (!sc->in_use) return 0;

    for (uint32_t i = 0; i < VENUS_MAX_SWAPCHAIN_IMAGES; i++) {
        int s = sc->image_slots[i];
        if (s < 0 || s >= (int)VENUS_MAX_IMAGE_OBJECTS) continue;
        struct venus_image *img = &dev->images[s];
        if (!img->in_use) continue;
        memset(img, 0, sizeof(*img));
    }
    memset(sc, 0, sizeof(*sc));
    return 0;
}
