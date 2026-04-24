#include "loader.h"

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkInstance *pInstance) {
    (void)pAllocator;  /* Wave 2 ignores custom allocators. */
    if (!pCreateInfo || !pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    struct osito_instance *self = malloc(sizeof(*self));
    if (!self) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(self, 0, sizeof(*self));
    set_loader_magic_value(self);

    /* Create an instance on each registered ICD. Any ICD that fails is
     * skipped (rationale: a partially-available backend shouldn't kill
     * the loader). */
    for (unsigned i = 0; i < osito_icd_count; i++) {
        const struct osito_icd_entry *e = &osito_icd_table[i];

        PFN_vkCreateInstance create =
            (PFN_vkCreateInstance)e->get_proc_addr(VK_NULL_HANDLE,
                                                   "vkCreateInstance");
        if (!create) continue;

        VkInstance child = VK_NULL_HANDLE;
        VkResult rc = create(pCreateInfo, pAllocator, &child);
        if (rc == VK_SUCCESS && child && self->icd_instance_count < OSITOK_VK_LOADER_MAX_ICDS) {
            self->icd_instances[self->icd_instance_count].icd = e;
            self->icd_instances[self->icd_instance_count].handle = child;
            self->icd_instance_count++;
        }
    }

    if (self->icd_instance_count == 0) {
        free(self);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    *pInstance = osito_instance_to(self);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    if (!instance) return;
    struct osito_instance *self = osito_instance_from(instance);

    for (unsigned i = 0; i < self->icd_instance_count; i++) {
        struct osito_icd_inst *ci = &self->icd_instances[i];
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)ci->icd->get_proc_addr(ci->handle,
                                                          "vkDestroyInstance");
        if (destroy) destroy(ci->handle, pAllocator);
        ci->icd = NULL;
        ci->handle = VK_NULL_HANDLE;
    }
    self->icd_instance_count = 0;
    free(self);
}
