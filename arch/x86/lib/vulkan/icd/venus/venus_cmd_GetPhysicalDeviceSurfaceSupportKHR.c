/*
 * Encoder for vkGetPhysicalDeviceSurfaceSupportKHR. Guest-local:
 * our single implicit queue family (family 0) always supports the
 * single presentation path we expose. Return VK_TRUE.
 *
 * See master plan §W3b.5 T6.
 */
#include "venus.h"

int venus_cmd_encode_GetPhysicalDeviceSurfaceSupportKHR(
        struct venus_instance *inst,
        uint32_t queueFamilyIndex, VkSurfaceKHR surface,
        VkBool32 *pSupported) {
    (void)inst; (void)queueFamilyIndex; (void)surface;
    if (!pSupported) return -22;
    *pSupported = VK_TRUE;
    return 0;
}
