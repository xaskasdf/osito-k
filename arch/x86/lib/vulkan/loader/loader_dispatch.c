#include "loader.h"

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance, uint32_t *, VkPhysicalDevice *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);

PFN_vkVoidFunction
osito_loader_get_instance_proc_addr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;

    /* Global entry points (no instance needed). */
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;

    /* Instance-scoped entry points. */
    if (strcmp(pName, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)vkDestroyInstance;
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;

    /* Unknown — fall through to the first ICD that resolves it. Matches
     * the spec's language that unknown queries may return NULL when no
     * extension is enabled, but a forward is a friendlier default for
     * debugging. */
    if (instance) {
        struct osito_instance *self = osito_instance_from(instance);
        for (unsigned i = 0; i < self->icd_instance_count; i++) {
            struct osito_icd_inst *ci = &self->icd_instances[i];
            PFN_vkVoidFunction fn = ci->icd->get_proc_addr(ci->handle, pName);
            if (fn) return fn;
        }
    }
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return osito_loader_get_instance_proc_addr(instance, pName);
}
