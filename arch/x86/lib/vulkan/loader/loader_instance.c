#include "loader.h"

extern int strcmp(const char *, const char *);

/* Hardware detection — ask the kernel which GPU backend(s) it has up.
 *
 * SYS_GPU_CAPS (601) returns a bitmask:
 *   bit 0  GPU_CAP_VENUS_READY  — virtio-gpu 3D online (QEMU/cloud)
 *   bit 1  GPU_CAP_NVK_READY    — bare-metal NVIDIA NVK backend online
 *
 * We use this to order the ICD scan so the right backend is tried FIRST
 * and its physical devices land at index 0 in vkEnumeratePhysicalDevices
 * (Mesa Zink picks pdev[0]).  If both are up (unusual — host-virtio +
 * passthrough) we still prefer NVK because it backs a real GPU.  If the
 * syscall fails (very early boot, kernel without GPU caps) we fall back
 * to the static table order.
 */
#define SYS_GPU_CAPS         600
#define GPU_CAP_VENUS_READY  (1u << 0)
#define GPU_CAP_NVK_READY    (1u << 1)

extern long __syscall1(long n, long a);

static unsigned osito_query_gpu_caps(void)
{
    unsigned caps = 0;
    long rc = __syscall1(SYS_GPU_CAPS, (long)(unsigned long)&caps);
    if (rc < 0) return 0;
    return caps;
}

/* Return the ICD entry whose name matches, or NULL. */
static const struct osito_icd_entry *osito_icd_find(const char *name)
{
    for (unsigned i = 0; i < osito_icd_count; i++) {
        if (strcmp(osito_icd_table[i].name, name) == 0)
            return &osito_icd_table[i];
    }
    return NULL;
}

/* Build a probe order based on detected hardware. Writes pointers into
 * `order[]` (length must be >= osito_icd_count) and returns the number
 * of entries written. Backends that are not hardware-ready are not probed. */
static unsigned osito_icd_probe_order(const struct osito_icd_entry **order)
{
    unsigned caps = osito_query_gpu_caps();
    unsigned n = 0;

    /* Register exactly one validated backend. */
    if (caps & GPU_CAP_VENUS_READY) {
        const struct osito_icd_entry *e = osito_icd_find("venus");
        if (e) { order[n++] = e; return n; }
    }
    return n;
}

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

    /* HW-detect once and build the probe order. */
    const struct osito_icd_entry *order[8];
    unsigned order_n = osito_icd_probe_order(order);

    /* Create an instance on each registered ICD. Any ICD that fails is
     * skipped (rationale: a partially-available backend shouldn't kill
     * the loader). */
    for (unsigned i = 0; i < order_n; i++) {
        const struct osito_icd_entry *e = order[i];

        PFN_vkCreateInstance create =
            (PFN_vkCreateInstance)e->get_proc_addr(VK_NULL_HANDLE,
                                                   "vkCreateInstance");
        if (!create) continue;

        VkInstance child = VK_NULL_HANDLE;
        VkResult rc = create(pCreateInfo, NULL, &child);  /* Wave 2 ignores custom allocators. */
        if (rc != VK_SUCCESS || !child) continue;

        if (self->icd_instance_count >= OSITOK_VK_LOADER_MAX_ICDS) {
            /* Table full — destroy the orphan instead of leaking it. */
            PFN_vkDestroyInstance orphan_destroy =
                (PFN_vkDestroyInstance)e->get_proc_addr(child, "vkDestroyInstance");
            if (orphan_destroy) orphan_destroy(child, NULL);
            continue;
        }

        self->icd_instances[self->icd_instance_count].icd = e;
        self->icd_instances[self->icd_instance_count].handle = child;
        self->icd_instance_count++;
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
    (void)pAllocator;  /* Wave 2 ignores custom allocators. */
    if (!instance) return;
    struct osito_instance *self = osito_instance_from(instance);

    /*
     * W3b.2 NOTE: device wrappers are not registered with the instance, so
     * vkDestroyInstance cannot free outstanding VkDevice wrappers. Worse,
     * such devices stay LIVE in the owning ICD — vkDestroyInstance walks
     * the ICDs and tells each to destroy its own instance, but ICD-side
     * devices already borrowed out remain. App must destroy devices before
     * instances (Vulkan spec already requires this). Full wrapper tracking
     * + orphan teardown lands in W3b.3 when queue handle lifetime becomes
     * load-bearing for present/submit.
     */
    for (unsigned i = 0; i < self->icd_instance_count; i++) {
        struct osito_icd_inst *ci = &self->icd_instances[i];
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)ci->icd->get_proc_addr(ci->handle,
                                                          "vkDestroyInstance");
        if (destroy) destroy(ci->handle, NULL);
        ci->icd = NULL;
        ci->handle = VK_NULL_HANDLE;
    }
    self->icd_instance_count = 0;
    free(self);
}
