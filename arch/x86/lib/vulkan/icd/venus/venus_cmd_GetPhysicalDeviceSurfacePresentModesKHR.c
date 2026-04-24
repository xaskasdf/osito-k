/*
 * Encoder for vkGetPhysicalDeviceSurfacePresentModesKHR. Guest-local:
 * we report FIFO only. The OsitoK compositor doesn't expose any
 * queuing semantics other than "draw on SYS_GUI_FLIP", which we treat
 * as FIFO (the flip is serialized relative to the next compositor
 * refresh).
 *
 * See master plan §W3b.5 T6.
 */
#include "venus.h"

int venus_cmd_encode_GetPhysicalDeviceSurfacePresentModesKHR(
        struct venus_instance *inst, VkSurfaceKHR surface,
        uint32_t *pModeCount, VkPresentModeKHR *pModes) {
    (void)inst; (void)surface;
    if (!pModeCount) return -22;
    const uint32_t kCount = 1u;
    if (!pModes) { *pModeCount = kCount; return 0; }
    uint32_t n = (*pModeCount < kCount) ? *pModeCount : kCount;
    if (n >= 1) pModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    if (*pModeCount < kCount) { *pModeCount = n; return 5; }
    *pModeCount = n;
    return 0;
}
