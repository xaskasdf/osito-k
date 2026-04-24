#include "venus.h"

extern int strcmp(const char *, const char *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!name) return NULL;

    /* Instance-level entry points. */
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)venus_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)venus_DestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)venus_EnumeratePhysicalDevices;
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)venus_GetPhysicalDeviceProperties;

    /* Device-scoped entry points — loader resolves via GIPA with instance
     * arg in the Khronos convention, so we must still answer here. */
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)venus_CreateDevice;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)venus_DestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)venus_GetDeviceQueue;

    return NULL;
}
