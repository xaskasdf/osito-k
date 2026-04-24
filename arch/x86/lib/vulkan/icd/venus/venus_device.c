#include "venus.h"

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDevice *pDevice) {
    (void)pCreateInfo; (void)pAllocator;
    if (!physicalDevice || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_instance *parent = (struct venus_instance *)physicalDevice;
    if (!(parent->caps & VENUS_GPU_CAP_VENUS_READY) || parent->ctx_id <= 0)
        return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_device *dev = malloc(sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(dev, 0, sizeof(*dev));
    set_loader_magic_value(dev);
    dev->parent = parent;
    set_loader_magic_value(&dev->queue_loader_data);

    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!device) return;
    struct venus_device *dev = (struct venus_device *)device;
    /* The kernel ctx is owned by the parent instance, not per device.
     * Nothing to call back to the kernel on device destroy in W3a. */
    free(dev);
}

VKAPI_ATTR void VKAPI_CALL
venus_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                     uint32_t queueIndex, VkQueue *pQueue) {
    (void)queueFamilyIndex; (void)queueIndex;
    if (!device || !pQueue) return;
    struct venus_device *dev = (struct venus_device *)device;
    /* Point at the embedded queue_loader_data so the handle has magic. */
    *pQueue = (VkQueue)&dev->queue_loader_data;
}
