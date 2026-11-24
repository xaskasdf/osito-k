#ifndef OSITOK_VK_NVK_STUB_H
#define OSITOK_VK_NVK_STUB_H

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* Per-instance state for nvk-stub. Currently just the magic + caps snapshot. */
struct nvk_stub_instance {
    VK_LOADER_DATA loader_data;
    uint32_t       caps;     /* SYS_GPU_CAPS result at create time */
};

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkInstance *pInstance);

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyInstance(VkInstance instance,
                         const VkAllocationCallbacks *pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_EnumeratePhysicalDevices(VkInstance instance,
                                  uint32_t *pPhysicalDeviceCount,
                                  VkPhysicalDevice *pPhysicalDevices);

#endif
