#include "loader.h"

extern int printf(const char *, ...);

VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance,
                           uint32_t *pPhysicalDeviceCount,
                           VkPhysicalDevice *pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct osito_instance *self = osito_instance_from(instance);
    printf("[VKLOADER] enum pdev: instance=%p icds=%u output=%p cap=%u\n",
           (void *)instance, self->icd_instance_count,
           (void *)pPhysicalDevices,
           pPhysicalDevices ? *pPhysicalDeviceCount : 0);

    /* First pass: sum up counts across ICDs. */
    uint32_t total = 0;
    uint32_t per_icd_count[OSITOK_VK_LOADER_MAX_ICDS] = {0};

    for (unsigned i = 0; i < self->icd_instance_count; i++) {
        struct osito_icd_inst *ci = &self->icd_instances[i];
        PFN_vkEnumeratePhysicalDevices enum_fn =
            (PFN_vkEnumeratePhysicalDevices)ci->icd->get_proc_addr(ci->handle,
                                                                   "vkEnumeratePhysicalDevices");
        printf("[VKLOADER] enum pdev: icd[%u]=%s child=%p proc=%p\n",
               i, ci->icd->name, (void *)ci->handle, (void *)enum_fn);
        if (!enum_fn) continue;
        uint32_t n = 0;
        VkResult rc = enum_fn(ci->handle, &n, NULL);
        printf("[VKLOADER] enum pdev: icd[%u] count rc=%d count=%u\n",
               i, (int)rc, n);
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

        /* Heap-allocate scratch to exact first-pass size — avoids the
         * 16-handle stack cap that silently truncated ICDs exposing more
         * than 16 physical devices. malloc failure here is graceful: we
         * skip this ICD and mark the overall result INCOMPLETE. */
        uint32_t want = per_icd_count[i];
        VkPhysicalDevice *raw = malloc((unsigned long)want * sizeof(VkPhysicalDevice));
        if (!raw) { overall = VK_INCOMPLETE; continue; }

        uint32_t got = want;
        VkResult rc = enum_fn(ci->handle, &got, raw);
        printf("[VKLOADER] enum pdev: icd[%u] fill rc=%d want=%u got=%u\n",
               i, (int)rc, want, got);
        if (rc == VK_INCOMPLETE) overall = VK_INCOMPLETE;

        for (uint32_t j = 0; j < got && written < cap; j++) {
            struct osito_phys_device *pw = malloc(sizeof(*pw));
            if (!pw) { overall = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            memset(pw, 0, sizeof(*pw));
            set_loader_magic_value(pw);
            pw->owner = ci;
            pw->real  = raw[j];
            VkPhysicalDevice wrapped = osito_phys_to(pw);
            pPhysicalDevices[written] = wrapped;
            printf("[VKLOADER] enum pdev: slot=%u raw=%p wrapper=%p out=%p readback=%p\n",
                   written, (void *)raw[j], (void *)pw, (void *)wrapped,
                   (void *)pPhysicalDevices[written]);
            written++;
        }
        free(raw);
    }

    *pPhysicalDeviceCount = written;
    if (written < total) overall = VK_INCOMPLETE;
    printf("[VKLOADER] enum pdev: return rc=%d written=%u total=%u\n",
           (int)overall, written, total);
    return overall;
}
