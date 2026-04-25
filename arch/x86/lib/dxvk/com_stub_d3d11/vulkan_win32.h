/* vulkan_win32.h -- OsitoK stub for DXVK W5.1.
 *
 * DXVK's vulkan_loader.h force-defines VK_USE_PLATFORM_WIN32_KHR=1 and
 * then includes <vulkan/vulkan.h> which in turn includes
 * <vulkan_win32.h> to get the Win32-specific Vulkan extensions
 * (VK_KHR_win32_surface, VK_KHR_external_memory_win32, etc.).
 *
 * On OsitoK we have no Win32 WSI, but DXVK only references these types
 * in external-memory / fence plumbing that DXBC doesn't reach. Declare
 * just enough so vulkan_core.h compiles under VK_USE_PLATFORM_WIN32_KHR.
 *
 * Paired with com_stub/IUnknown.h + osito_compat/dxvk_compat.h for
 * HANDLE / SECURITY_ATTRIBUTES / DWORD / HWND typedefs.
 */

#ifndef OSITOK_DXVK_VULKAN_WIN32_H
#define OSITOK_DXVK_VULKAN_WIN32_H

/* Native windows shim already provides HANDLE/GUID/DWORD/HINSTANCE/HWND
 * /HMONITOR + the SECURITY_ATTRIBUTES struct via windows_base.h, so we
 * do NOT pull in the com_stub/IUnknown.h variant here -- that would
 * give us two competing GUID typedefs in the same TU. */

/* SECURITY_ATTRIBUTES is defined by the DXVK native windows_base.h.
 * PSECURITY_ATTRIBUTES is not, so add it here. */
typedef struct SECURITY_ATTRIBUTES *PSECURITY_ATTRIBUTES_PTR;
#ifndef PSECURITY_ATTRIBUTES
#define PSECURITY_ATTRIBUTES PSECURITY_ATTRIBUTES_PTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan expects vulkan_core.h to have been included already; this file
 * is never pulled in standalone. We just provide the Win32 surface +
 * external-memory typedefs + struct forward-decls DXVK might touch. */

#ifndef VK_KHR_win32_surface
#define VK_KHR_win32_surface 1
#define VK_KHR_WIN32_SURFACE_SPEC_VERSION 6
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"

typedef VkFlags VkWin32SurfaceCreateFlagsKHR;

typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType              sType;
    const void*                  pNext;
    VkWin32SurfaceCreateFlagsKHR flags;
    HINSTANCE                    hinstance;
    HWND                         hwnd;
} VkWin32SurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateWin32SurfaceKHR)(
    VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface);

typedef VkBool32 (VKAPI_PTR *PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex);
#endif /* VK_KHR_win32_surface */

#ifndef VK_KHR_external_memory_win32
#define VK_KHR_external_memory_win32 1

typedef VkResult (VKAPI_PTR *PFN_vkGetMemoryWin32HandleKHR)(
    VkDevice device,
    const struct VkMemoryGetWin32HandleInfoKHR* pGetWin32HandleInfo,
    HANDLE* pHandle);
typedef VkResult (VKAPI_PTR *PFN_vkGetMemoryWin32HandlePropertiesKHR)(
    VkDevice device,
    VkExternalMemoryHandleTypeFlagBits handleType,
    HANDLE handle,
    struct VkMemoryWin32HandlePropertiesKHR* pMemoryWin32HandleProperties);

typedef struct VkImportMemoryWin32HandleInfoKHR {
    VkStructureType                       sType;
    const void*                           pNext;
    VkExternalMemoryHandleTypeFlagBits    handleType;
    HANDLE                                handle;
    LPCWSTR                               name;
} VkImportMemoryWin32HandleInfoKHR;

typedef struct VkExportMemoryWin32HandleInfoKHR {
    VkStructureType                    sType;
    const void*                        pNext;
    const PSECURITY_ATTRIBUTES         pAttributes;
    DWORD                              dwAccess;
    LPCWSTR                            name;
} VkExportMemoryWin32HandleInfoKHR;

typedef struct VkMemoryWin32HandlePropertiesKHR {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           memoryTypeBits;
} VkMemoryWin32HandlePropertiesKHR;

typedef struct VkMemoryGetWin32HandleInfoKHR {
    VkStructureType                       sType;
    const void*                           pNext;
    VkDeviceMemory                        memory;
    VkExternalMemoryHandleTypeFlagBits    handleType;
} VkMemoryGetWin32HandleInfoKHR;
#endif /* VK_KHR_external_memory_win32 */

#ifndef VK_KHR_win32_keyed_mutex
#define VK_KHR_win32_keyed_mutex 1

typedef struct VkWin32KeyedMutexAcquireReleaseInfoKHR {
    VkStructureType          sType;
    const void*              pNext;
    uint32_t                 acquireCount;
    const VkDeviceMemory*    pAcquireSyncs;
    const uint64_t*          pAcquireKeys;
    const uint32_t*          pAcquireTimeouts;
    uint32_t                 releaseCount;
    const VkDeviceMemory*    pReleaseSyncs;
    const uint64_t*          pReleaseKeys;
} VkWin32KeyedMutexAcquireReleaseInfoKHR;
#endif /* VK_KHR_win32_keyed_mutex */

#ifndef VK_KHR_external_semaphore_win32
#define VK_KHR_external_semaphore_win32 1

typedef VkResult (VKAPI_PTR *PFN_vkImportSemaphoreWin32HandleKHR)(
    VkDevice device,
    const struct VkImportSemaphoreWin32HandleInfoKHR* pImportSemaphoreWin32HandleInfo);
typedef VkResult (VKAPI_PTR *PFN_vkGetSemaphoreWin32HandleKHR)(
    VkDevice device,
    const struct VkSemaphoreGetWin32HandleInfoKHR* pGetWin32HandleInfo,
    HANDLE* pHandle);

typedef struct VkImportSemaphoreWin32HandleInfoKHR {
    VkStructureType                          sType;
    const void*                              pNext;
    VkSemaphore                              semaphore;
    VkSemaphoreImportFlags                   flags;
    VkExternalSemaphoreHandleTypeFlagBits    handleType;
    HANDLE                                   handle;
    LPCWSTR                                  name;
} VkImportSemaphoreWin32HandleInfoKHR;

typedef struct VkExportSemaphoreWin32HandleInfoKHR {
    VkStructureType               sType;
    const void*                   pNext;
    const PSECURITY_ATTRIBUTES    pAttributes;
    DWORD                         dwAccess;
    LPCWSTR                       name;
} VkExportSemaphoreWin32HandleInfoKHR;

typedef struct VkD3D12FenceSubmitInfoKHR {
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           waitSemaphoreValuesCount;
    const uint64_t*    pWaitSemaphoreValues;
    uint32_t           signalSemaphoreValuesCount;
    const uint64_t*    pSignalSemaphoreValues;
} VkD3D12FenceSubmitInfoKHR;

typedef struct VkSemaphoreGetWin32HandleInfoKHR {
    VkStructureType                          sType;
    const void*                              pNext;
    VkSemaphore                              semaphore;
    VkExternalSemaphoreHandleTypeFlagBits    handleType;
} VkSemaphoreGetWin32HandleInfoKHR;
#endif /* VK_KHR_external_semaphore_win32 */

#ifndef VK_KHR_external_fence_win32
#define VK_KHR_external_fence_win32 1

typedef VkResult (VKAPI_PTR *PFN_vkImportFenceWin32HandleKHR)(
    VkDevice device,
    const struct VkImportFenceWin32HandleInfoKHR* pImportFenceWin32HandleInfo);
typedef VkResult (VKAPI_PTR *PFN_vkGetFenceWin32HandleKHR)(
    VkDevice device,
    const struct VkFenceGetWin32HandleInfoKHR* pGetWin32HandleInfo,
    HANDLE* pHandle);

typedef struct VkImportFenceWin32HandleInfoKHR {
    VkStructureType                      sType;
    const void*                          pNext;
    VkFence                              fence;
    VkFenceImportFlags                   flags;
    VkExternalFenceHandleTypeFlagBits    handleType;
    HANDLE                               handle;
    LPCWSTR                              name;
} VkImportFenceWin32HandleInfoKHR;

typedef struct VkExportFenceWin32HandleInfoKHR {
    VkStructureType               sType;
    const void*                   pNext;
    const PSECURITY_ATTRIBUTES    pAttributes;
    DWORD                         dwAccess;
    LPCWSTR                       name;
} VkExportFenceWin32HandleInfoKHR;

typedef struct VkFenceGetWin32HandleInfoKHR {
    VkStructureType                      sType;
    const void*                          pNext;
    VkFence                              fence;
    VkExternalFenceHandleTypeFlagBits    handleType;
} VkFenceGetWin32HandleInfoKHR;
#endif /* VK_KHR_external_fence_win32 */

#ifndef VK_NV_external_memory_win32
#define VK_NV_external_memory_win32 1

typedef struct VkImportMemoryWin32HandleInfoNV {
    VkStructureType                      sType;
    const void*                          pNext;
    VkExternalMemoryHandleTypeFlagsNV    handleType;
    HANDLE                               handle;
} VkImportMemoryWin32HandleInfoNV;

typedef struct VkExportMemoryWin32HandleInfoNV {
    VkStructureType               sType;
    const void*                   pNext;
    const PSECURITY_ATTRIBUTES    pAttributes;
    DWORD                         dwAccess;
} VkExportMemoryWin32HandleInfoNV;
#endif /* VK_NV_external_memory_win32 */

#ifndef VK_NV_win32_keyed_mutex
#define VK_NV_win32_keyed_mutex 1

typedef struct VkWin32KeyedMutexAcquireReleaseInfoNV {
    VkStructureType          sType;
    const void*              pNext;
    uint32_t                 acquireCount;
    const VkDeviceMemory*    pAcquireSyncs;
    const uint64_t*          pAcquireKeys;
    const uint32_t*          pAcquireTimeoutMilliseconds;
    uint32_t                 releaseCount;
    const VkDeviceMemory*    pReleaseSyncs;
    const uint64_t*          pReleaseKeys;
} VkWin32KeyedMutexAcquireReleaseInfoNV;
#endif /* VK_NV_win32_keyed_mutex */

#ifndef VK_EXT_full_screen_exclusive
#define VK_EXT_full_screen_exclusive 1

typedef VkResult (VKAPI_PTR *PFN_vkAcquireFullScreenExclusiveModeEXT)(
    VkDevice device, VkSwapchainKHR swapchain);
typedef VkResult (VKAPI_PTR *PFN_vkReleaseFullScreenExclusiveModeEXT)(
    VkDevice device, VkSwapchainKHR swapchain);
typedef VkResult (VKAPI_PTR *PFN_vkGetDeviceGroupSurfacePresentModes2EXT)(
    VkDevice device,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkDeviceGroupPresentModeFlagsKHR* pModes);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes);

typedef enum VkFullScreenExclusiveEXT {
    VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT = 0,
    VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT = 1,
    VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT = 2,
    VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT = 3,
    VK_FULL_SCREEN_EXCLUSIVE_MAX_ENUM_EXT = 0x7FFFFFFF
} VkFullScreenExclusiveEXT;

typedef struct VkSurfaceFullScreenExclusiveInfoEXT {
    VkStructureType             sType;
    void*                       pNext;
    VkFullScreenExclusiveEXT    fullScreenExclusive;
} VkSurfaceFullScreenExclusiveInfoEXT;

typedef struct VkSurfaceCapabilitiesFullScreenExclusiveEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           fullScreenExclusiveSupported;
} VkSurfaceCapabilitiesFullScreenExclusiveEXT;

typedef struct VkSurfaceFullScreenExclusiveWin32InfoEXT {
    VkStructureType    sType;
    const void*        pNext;
    HMONITOR           hmonitor;
} VkSurfaceFullScreenExclusiveWin32InfoEXT;
#endif /* VK_EXT_full_screen_exclusive */

#ifdef __cplusplus
}
#endif

#endif /* OSITOK_DXVK_VULKAN_WIN32_H */
