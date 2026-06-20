/*
 * vulkan_ositok.h — OsitoK-specific Vulkan extension declarations.
 *
 * Currently reserves the structure and enum numbers for
 * VK_OSITOK_compositor_surface. Body wired in Wave 3 (WSI).
 */
#ifndef VULKAN_OSITOK_H_
#define VULKAN_OSITOK_H_ 1

#include "vulkan_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_OSITOK_compositor_surface 1
#define VK_OSITOK_COMPOSITOR_SURFACE_SPEC_VERSION 1
#define VK_OSITOK_COMPOSITOR_SURFACE_EXTENSION_NAME "VK_OSITOK_compositor_surface"

/* Placeholder structure type — real value will be registered with Khronos
 * if this ever ships outside OsitoK. For now we use a private ID in the
 * reserved range (>= 1000000000 per Vulkan ext convention). */
#define VK_STRUCTURE_TYPE_OSITOK_COMPOSITOR_SURFACE_CREATE_INFO ((VkStructureType)1000710000)

typedef struct VkOsitoCompositorSurfaceCreateInfoOSITOK {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        shmHandle;   /* compositor target SHM handle */
} VkOsitoCompositorSurfaceCreateInfoOSITOK;

typedef VkResult (VKAPI_PTR *PFN_vkCreateOsitoCompositorSurfaceOSITOK)(
    VkInstance                                       instance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK*  pCreateInfo,
    const VkAllocationCallbacks*                     pAllocator,
    VkSurfaceKHR*                                    pSurface);

/* Wave 3b.5 — entry point declaration. */
#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkCreateOsitokCompositorSurfaceKHR(
    VkInstance instance,
    const VkOsitoCompositorSurfaceCreateInfoOSITOK *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSurfaceKHR *pSurface);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_OSITOK_H_ */
