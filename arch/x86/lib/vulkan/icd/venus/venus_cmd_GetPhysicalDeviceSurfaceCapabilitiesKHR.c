/*
 * Encoder for vkGetPhysicalDeviceSurfaceCapabilitiesKHR. Guest-local:
 * pull width/height from the surface slot and return the stock caps.
 *
 * See master plan §W3b.5 T6.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull

int venus_cmd_encode_GetPhysicalDeviceSurfaceCapabilitiesKHR(
        struct venus_instance *inst, VkSurfaceKHR surface,
        VkSurfaceCapabilitiesKHR *pCaps) {
    if (!inst || !surface || !pCaps) return -22;
    int slot = (int)(((uint64_t)surface >> 48) & VENUS_H_SLOT_MASK_W3B5);
    if (slot < 0 || slot >= (int)VENUS_MAX_SURFACE_OBJECTS) return -22;
    struct venus_surface *s = &inst->surfaces[slot];
    if (!s->in_use) return -22;

    memset(pCaps, 0, sizeof(*pCaps));
    pCaps->minImageCount     = 2;
    pCaps->maxImageCount     = 3;
    pCaps->currentExtent.width  = s->width;
    pCaps->currentExtent.height = s->height;
    pCaps->minImageExtent.width  = s->width;
    pCaps->minImageExtent.height = s->height;
    pCaps->maxImageExtent.width  = s->width;
    pCaps->maxImageExtent.height = s->height;
    pCaps->maxImageArrayLayers   = 1;
    pCaps->supportedTransforms   = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->currentTransform      = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pCaps->supportedUsageFlags  =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return 0;
}
