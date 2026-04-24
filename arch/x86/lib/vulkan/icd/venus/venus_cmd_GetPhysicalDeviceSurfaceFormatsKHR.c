/*
 * Encoder for vkGetPhysicalDeviceSurfaceFormatsKHR. Guest-local: we
 * report a single format — B8G8R8A8_UNORM with SRGB_NONLINEAR — which
 * matches the OsitoK compositor pixel layout (BGRA, opaque).
 *
 * See master plan §W3b.5 T6.
 */
#include "venus.h"

int venus_cmd_encode_GetPhysicalDeviceSurfaceFormatsKHR(
        struct venus_instance *inst, VkSurfaceKHR surface,
        uint32_t *pFormatCount, VkSurfaceFormatKHR *pFormats) {
    (void)inst; (void)surface;
    if (!pFormatCount) return -22;
    const uint32_t kCount = 1u;
    if (!pFormats) {
        *pFormatCount = kCount;
        return 0;
    }
    uint32_t n = (*pFormatCount < kCount) ? *pFormatCount : kCount;
    if (n >= 1) {
        pFormats[0].format     = VK_FORMAT_B8G8R8A8_UNORM;
        pFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    if (*pFormatCount < kCount) { *pFormatCount = n; return 5; /* VK_INCOMPLETE */ }
    *pFormatCount = n;
    return 0;
}
