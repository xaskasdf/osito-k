#include "loader.h"

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance, const VkAllocationCallbacks *);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance, uint32_t *, VkPhysicalDevice *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);

/* W3a — device + queue + physical-device-properties forwards. */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice, const VkDeviceCreateInfo *,
    const VkAllocationCallbacks *, VkDevice *);
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice, const VkAllocationCallbacks *);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice, uint32_t, uint32_t, VkQueue *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice, VkPhysicalDeviceProperties *);

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

    /* W3a additions — device lifecycle + physical-device properties. */
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)vkCreateDevice;
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)vkDestroyDevice;
    if (strcmp(pName, "vkGetDeviceQueue") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties;

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

/* ---------------- W3a trampolines -----------------------------------------
 *
 * Known limitation (plan gotcha #5): "first ICD wins" dispatch. W3b will
 * replace this with proper handle-owner tracking (each physical device /
 * device handle remembers which ICD owns it). For W3a it is sufficient
 * because only one ICD (venus) reports devices when VIRGL is up, and
 * nvk-stub's device-level entries return NULL anyway. */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice,
               const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkDevice *pDevice) {
    /* Walk ICDs in table order; the first that answers with something
     * other than VK_ERROR_INITIALIZATION_FAILED wins. The physical-device
     * handle came from that ICD's enumerate, so its CreateDevice is the
     * right one. */
    for (unsigned i = 0; i < osito_icd_count; i++) {
        const struct osito_icd_entry *e = &osito_icd_table[i];
        PFN_vkCreateDevice fn =
            (PFN_vkCreateDevice)e->get_proc_addr(VK_NULL_HANDLE, "vkCreateDevice");
        if (!fn) continue;
        VkResult rc = fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
        if (rc != VK_ERROR_INITIALIZATION_FAILED) return rc;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    /* Each ICD's device handle starts with VK_LOADER_DATA — we can't
     * disambiguate without more state. For W3a, dispatch to the first ICD
     * that resolves vkDestroyDevice (venus is registered first and is the
     * only ICD that actually creates devices in this wave). */
    for (unsigned i = 0; i < osito_icd_count; i++) {
        const struct osito_icd_entry *e = &osito_icd_table[i];
        PFN_vkDestroyDevice fn =
            (PFN_vkDestroyDevice)e->get_proc_addr(VK_NULL_HANDLE, "vkDestroyDevice");
        if (fn) { fn(device, pAllocator); return; }
    }
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                 uint32_t queueIndex, VkQueue *pQueue) {
    for (unsigned i = 0; i < osito_icd_count; i++) {
        const struct osito_icd_entry *e = &osito_icd_table[i];
        PFN_vkGetDeviceQueue fn =
            (PFN_vkGetDeviceQueue)e->get_proc_addr(VK_NULL_HANDLE, "vkGetDeviceQueue");
        if (fn) { fn(device, queueFamilyIndex, queueIndex, pQueue); return; }
    }
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceProperties *pProperties) {
    for (unsigned i = 0; i < osito_icd_count; i++) {
        const struct osito_icd_entry *e = &osito_icd_table[i];
        PFN_vkGetPhysicalDeviceProperties fn =
            (PFN_vkGetPhysicalDeviceProperties)e->get_proc_addr(
                VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
        if (fn) { fn(physicalDevice, pProperties); return; }
    }
}
