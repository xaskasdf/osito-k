/*
 * NVK-stub ICD — userland shim for the future NVK native backend.
 *
 * Wave 1 kernel leaves GPU_CAP_NVK_READY clear, so this ICD reports
 * zero physical devices. The loader must still create its instance
 * and dispatch queries cleanly — that's what we validate in W2.
 *
 * Plan deviation: OsitoK libc exposes __syscall1/__syscall2/... rather
 * than a variadic syscall() wrapper; we use __syscall1 directly.
 */
#include "nvk_stub.h"
#include "../../loader/loader.h"

/* OsitoK syscall ABI — see arch/x86/include/sys/gpu_syscalls.h */
#define SYS_GPU_CAPS          600
#define GPU_CAP_NVK_READY     (1u << 1)

/* OsitoK libc syscall primitive. Provided by syscall.S. */
extern long __syscall1(long nr, long a1);

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkInstance *pInstance) {
    (void)pCreateInfo; (void)pAllocator;

    struct nvk_stub_instance *inst = malloc(sizeof(*inst));
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(inst, 0, sizeof(*inst));

    uint32_t caps = 0;
    (void)__syscall1(SYS_GPU_CAPS, (long)&caps);
    inst->caps = caps;

    /* Set the ICD magic dispatchable-object marker per vk_icd.h. */
    set_loader_magic_value(inst);

    *pInstance = (VkInstance)inst;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_stub_DestroyInstance(VkInstance instance,
                         const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (instance) free((void *)instance);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_stub_EnumeratePhysicalDevices(VkInstance instance,
                                  uint32_t *pPhysicalDeviceCount,
                                  VkPhysicalDevice *pPhysicalDevices) {
    struct nvk_stub_instance *inst = (struct nvk_stub_instance *)instance;

    /* Report zero devices unless the native NVK backend is wired. */
    uint32_t count = (inst->caps & GPU_CAP_NVK_READY) ? 1 : 0;

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount < count) {
        *pPhysicalDeviceCount = count;
        return VK_INCOMPLETE;
    }
    /* Wave 2 never takes this branch (count == 0). Wave 3/Phase-2 fills it. */
    *pPhysicalDeviceCount = count;
    return VK_SUCCESS;
}

/* vk_icdGetInstanceProcAddr — the one symbol the loader calls. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_PTR
nvk_stub_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!name) return NULL;

    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)nvk_stub_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)nvk_stub_DestroyInstance;
    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)nvk_stub_EnumeratePhysicalDevices;
    return NULL;
}
