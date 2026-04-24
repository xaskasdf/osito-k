#include "loader.h"

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance,
                           uint32_t *pPhysicalDeviceCount,
                           VkPhysicalDevice *pPhysicalDevices) {
    (void)instance;
    if (pPhysicalDevices) (void)pPhysicalDevices;
    if (pPhysicalDeviceCount) *pPhysicalDeviceCount = 0;
    return VK_SUCCESS;  /* filled in T5 */
}
