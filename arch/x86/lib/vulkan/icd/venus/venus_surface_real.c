#include "venus_real.h"

extern void *malloc(unsigned long);
extern void free(void *);

VKAPI_ATTR VkResult VKAPI_CALL
venus_real_CreateOsitokCompositorSurfaceKHR(
    VkInstance instance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *create_info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    if (!instance || !create_info || !surface || allocator ||
        create_info->sType !=
            VK_STRUCTURE_TYPE_OSITOK_COMPOSITOR_SURFACE_CREATE_INFO ||
        create_info->pNext || create_info->flags)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_surface_real *self = malloc(sizeof(*self));
    if (!self)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    self->instance = (struct venus_instance_real *)instance;
    self->window_id = create_info->shmHandle;
    *surface = (VkSurfaceKHR)(uintptr_t)self;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_real_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                             const VkAllocationCallbacks *allocator)
{
    (void)instance;
    (void)allocator;
    free((void *)(uintptr_t)surface);
}
