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

/* W3b.2 — additional phys-dev queries. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice, VkPhysicalDeviceFeatures *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);

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

    /* W3b.2 additions — three more phys-dev queries. */
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures;
    if (strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties;

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

/* ---------------- W3b.2 trampolines ---------------------------------------
 *
 * Each VkPhysicalDevice returned by vkEnumeratePhysicalDevices is wrapped
 * in an osito_phys_device (loader.h). Trampolines unwrap to the owning
 * ICD and dispatch against that ICD's real handle — fixing the W3a
 * "first ICD wins" gotcha.
 *
 * VkDevice follows the same wrapping in vkCreateDevice below. */

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice,
               const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkDevice *pDevice) {
    if (!physicalDevice || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_phys_device *pw = osito_phys_from(physicalDevice);
    struct osito_icd_inst    *ci = pw->owner;
    if (!ci) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkCreateDevice fn = (PFN_vkCreateDevice)
        ci->icd->get_proc_addr(ci->handle, "vkCreateDevice");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkDevice icd_dev = VK_NULL_HANDLE;
    VkResult rc = fn(pw->real, pCreateInfo, pAllocator, &icd_dev);
    if (rc != VK_SUCCESS || !icd_dev) return rc;

    struct osito_device *dw = malloc(sizeof(*dw));
    if (!dw) {
        /* Best-effort destroy of the orphan ICD device. */
        PFN_vkDestroyDevice drop = (PFN_vkDestroyDevice)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyDevice");
        if (drop) drop(icd_dev, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(dw, 0, sizeof(*dw));
    set_loader_magic_value(dw);
    dw->owner = ci;
    dw->real  = icd_dev;

    *pDevice = osito_device_to(dw);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    if (!device) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    if (ci) {
        PFN_vkDestroyDevice fn = (PFN_vkDestroyDevice)
            ci->icd->get_proc_addr(ci->handle, "vkDestroyDevice");
        if (fn) fn(dw->real, pAllocator);
    }
    free(dw);
}

VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                 uint32_t queueIndex, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    struct osito_device    *dw = osito_device_from(device);
    struct osito_icd_inst  *ci = dw->owner;
    if (!ci) return;
    PFN_vkGetDeviceQueue fn = (PFN_vkGetDeviceQueue)
        ci->icd->get_proc_addr(ci->handle, "vkGetDeviceQueue");
    if (fn) fn(dw->real, queueFamilyIndex, queueIndex, pQueue);
}

/* Tiny unwrap helper for the phys-dev trampolines below. */
static inline int osito_unwrap_phys(VkPhysicalDevice h,
                                    struct osito_icd_inst **ci_out,
                                    VkPhysicalDevice *real_out) {
    if (!h) return 0;
    struct osito_phys_device *pw = osito_phys_from(h);
    if (!pw || !pw->owner) return 0;
    *ci_out   = pw->owner;
    *real_out = pw->real;
    return 1;
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceProperties *pProperties) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceProperties fn = (PFN_vkGetPhysicalDeviceProperties)
        ci->icd->get_proc_addr(ci->handle, "vkGetPhysicalDeviceProperties");
    if (fn) fn(real, pProperties);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                            VkPhysicalDeviceFeatures *pFeatures) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceFeatures fn = (PFN_vkGetPhysicalDeviceFeatures)
        ci->icd->get_proc_addr(ci->handle, "vkGetPhysicalDeviceFeatures");
    if (fn) fn(real, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice physicalDevice,
        uint32_t *pCount, VkQueueFamilyProperties *pFamilies) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (fn) fn(real, pCount, pFamilies);
}

VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties *pMem) {
    struct osito_icd_inst *ci = 0; VkPhysicalDevice real = 0;
    if (!osito_unwrap_phys(physicalDevice, &ci, &real)) return;
    PFN_vkGetPhysicalDeviceMemoryProperties fn =
        (PFN_vkGetPhysicalDeviceMemoryProperties)ci->icd->get_proc_addr(
            ci->handle, "vkGetPhysicalDeviceMemoryProperties");
    if (fn) fn(real, pMem);
}
