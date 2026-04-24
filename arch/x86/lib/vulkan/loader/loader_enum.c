#include "loader.h"

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance,
                           uint32_t *pPhysicalDeviceCount,
                           VkPhysicalDevice *pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_instance *self = osito_instance_from(instance);

    /* First pass: sum up counts across ICDs. */
    uint32_t total = 0;
    uint32_t per_icd_count[OSITOK_VK_LOADER_MAX_ICDS] = {0};

    for (unsigned i = 0; i < self->icd_instance_count; i++) {
        struct osito_icd_inst *ci = &self->icd_instances[i];
        PFN_vkEnumeratePhysicalDevices enum_fn =
            (PFN_vkEnumeratePhysicalDevices)ci->icd->get_proc_addr(ci->handle,
                                                                   "vkEnumeratePhysicalDevices");
        if (!enum_fn) continue;
        uint32_t n = 0;
        VkResult rc = enum_fn(ci->handle, &n, NULL);
        if (rc == VK_SUCCESS) {
            per_icd_count[i] = n;
            total += n;
        }
    }

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = total;
        return VK_SUCCESS;
    }

    /* Second pass: actually collect handles, capped by app's buffer size.
     * Each ICD-returned handle gets wrapped in an osito_phys_device so
     * downstream trampolines can unwrap and dispatch to the owning ICD. */
    uint32_t cap = *pPhysicalDeviceCount;
    uint32_t written = 0;
    VkResult overall = VK_SUCCESS;

    for (unsigned i = 0; i < self->icd_instance_count && written < cap; i++) {
        struct osito_icd_inst *ci = &self->icd_instances[i];
        if (per_icd_count[i] == 0) continue;

        PFN_vkEnumeratePhysicalDevices enum_fn =
            (PFN_vkEnumeratePhysicalDevices)ci->icd->get_proc_addr(ci->handle,
                                                                   "vkEnumeratePhysicalDevices");
        if (!enum_fn) continue;

        uint32_t want = per_icd_count[i];
        uint32_t room = cap - written;
        uint32_t ask = want < room ? want : room;

        /* Use a small stack scratch for ICD handles, then wrap into
         * the app's buffer. Keep the scratch under the Q2 stack
         * guideline (≪ 200 KiB). 16 handles * 8 bytes = 128 B. */
        VkPhysicalDevice scratch[16];
        uint32_t pass = ask;
        if (pass > 16) pass = 16;
        VkResult rc = enum_fn(ci->handle, &pass, scratch);
        if (rc == VK_INCOMPLETE) overall = VK_INCOMPLETE;

        for (uint32_t j = 0; j < pass && written < cap; j++) {
            struct osito_phys_device *pw = malloc(sizeof(*pw));
            if (!pw) { overall = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            memset(pw, 0, sizeof(*pw));
            set_loader_magic_value(pw);
            pw->owner = ci;
            pw->real  = scratch[j];
            pPhysicalDevices[written++] = osito_phys_to(pw);
        }
    }

    *pPhysicalDeviceCount = written;
    if (written < total) overall = VK_INCOMPLETE;
    return overall;
}
