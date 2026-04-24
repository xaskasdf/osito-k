#include "loader.h"

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkInstance *pInstance) {
    (void)pCreateInfo; (void)pAllocator; (void)pInstance;
    return VK_ERROR_INITIALIZATION_FAILED;  /* filled in T4 */
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance,
                  const VkAllocationCallbacks *pAllocator) {
    (void)instance; (void)pAllocator;
    /* filled in T4 */
}
