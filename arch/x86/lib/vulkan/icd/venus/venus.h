/*
 * venus.h — userland venus ICD for OsitoK.
 *
 * Wave 3a: skeleton only. No protocol wire encoding yet. Validates the
 * kernel handshake path (SYS_GPU_CAPS / CTX_CREATE / CTX_DESTROY) and
 * hosts the Vulkan dispatch for instance+device lifecycles.
 */
#ifndef OSITOK_VK_VENUS_H
#define OSITOK_VK_VENUS_H

#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

/* Matches kernel ABI from arch/x86/include/sys/gpu_syscalls.h. We do NOT
 * include that header here because it's kernel-tree; duplicate the minimum
 * constants instead. */
#define VENUS_SYS_GPU_CAPS         600L
#define VENUS_SYS_GPU_CTX_CREATE   601L
#define VENUS_SYS_GPU_CTX_DESTROY  602L
#define VENUS_GPU_CAP_VENUS_READY  (1u << 0)
#define VENUS_GPU_CTX_VENUS        0x00u

struct venus_instance {
    VK_LOADER_DATA loader_data;
    uint32_t       caps;          /* SYS_GPU_CAPS snapshot */
    int32_t        ctx_id;        /* kernel GPU ctx, 0 if none */
};

struct venus_device {
    VK_LOADER_DATA loader_data;
    struct venus_instance *parent;
    /* Single implicit queue for W3a — real queue families land in W3b. */
    VK_LOADER_DATA queue_loader_data;
};

/* Entry points. */
VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *,
                     VkInstance *);

VKAPI_ATTR void VKAPI_CALL
venus_DestroyInstance(VkInstance, const VkAllocationCallbacks *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_EnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                   const VkAllocationCallbacks *, VkDevice *);

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDevice(VkDevice, const VkAllocationCallbacks *);

VKAPI_ATTR void VKAPI_CALL
venus_GetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance, const char *);

#endif
