#include "venus.h"

/* OsitoK libc — 6-argument fixed-arity syscall gates. */
extern long __syscall0(long);
extern long __syscall1(long, long);
extern long __syscall2(long, long, long);

extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);

VKAPI_ATTR VkResult VKAPI_CALL
venus_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance) {
    (void)pCreateInfo; (void)pAllocator;
    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    struct venus_instance *self = malloc(sizeof(*self));
    if (!self) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(self, 0, sizeof(*self));
    set_loader_magic_value(self);

    /* Probe kernel capability. Caps is u32 passed by reference. */
    uint32_t caps = 0;
    (void)__syscall1(VENUS_SYS_GPU_CAPS, (long)&caps);
    self->caps = caps;

    /* If venus not ready in the host, we still create an instance — the
     * loader will see us enumerate zero devices, same as nvk-stub. This
     * keeps vkCreateInstance succeeding even on hosts without virgl. */
    if (caps & VENUS_GPU_CAP_VENUS_READY) {
        long rc = __syscall1(VENUS_SYS_GPU_CTX_CREATE, (long)VENUS_GPU_CTX_VENUS);
        if (rc > 0) {
            self->ctx_id = (int32_t)rc;

            /* Open the wire and issue the real CreateInstance call (W3b.1). */
            extern struct venus_wire *venus_wire_open(int32_t);
            extern int venus_cmd_encode_CreateInstance(struct venus_wire *,
                                                       const VkInstanceCreateInfo *,
                                                       uint64_t *);
            self->wire = venus_wire_open(self->ctx_id);
            if (self->wire) {
                uint64_t host_handle = 0;
                int r = venus_cmd_encode_CreateInstance(self->wire, pCreateInfo,
                                                        &host_handle);
                if (r == 0 /* VK_SUCCESS */ && host_handle != 0) {
                    self->host_handle = host_handle;
                } else {
                    /* Host rejected or wire stalled — fall back to guest-local
                     * instance (no rendering but enumeration still returns 0). */
                    self->caps &= ~VENUS_GPU_CAP_VENUS_READY;
                }
            } else {
                self->caps &= ~VENUS_GPU_CAP_VENUS_READY;
            }
        } else {
            /* Kernel reported ready but context creation failed — treat as
             * not-ready for this instance. No reason to fail the whole call. */
            self->caps &= ~VENUS_GPU_CAP_VENUS_READY;
        }
    }

    *pInstance = (VkInstance)self;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
venus_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    if (!instance) return;
    struct venus_instance *self = (struct venus_instance *)instance;
    if (self->wire) {
        extern void venus_wire_close(struct venus_wire *);
        venus_wire_close(self->wire);
        self->wire = 0;
    }
    if (self->ctx_id > 0) {
        (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, (long)(uint32_t)self->ctx_id);
        self->ctx_id = 0;
    }
    free(self);
}

/* Single-device model for W3a. The physical-device handle IS the instance
 * pointer; loader dispatch follows `VK_LOADER_DATA` magic either way. */

VKAPI_ATTR VkResult VKAPI_CALL
venus_EnumeratePhysicalDevices(VkInstance instance,
                               uint32_t *pPhysicalDeviceCount,
                               VkPhysicalDevice *pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    struct venus_instance *self = (struct venus_instance *)instance;

    uint32_t count = (self->caps & VENUS_GPU_CAP_VENUS_READY) ? 1u : 0u;

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount < count) {
        *pPhysicalDeviceCount = count;
        return VK_INCOMPLETE;
    }
    if (count > 0) {
        /* Reuse the instance pointer as the physical-device handle.
         * Dispatchable-object magic works either way because both structs
         * begin with VK_LOADER_DATA. */
        pPhysicalDevices[0] = (VkPhysicalDevice)self;
    }
    *pPhysicalDeviceCount = count;
    return VK_SUCCESS;
}

/* Hard-coded reasonable defaults. W3b's venus protocol will pull real
 * values from the host. */
VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceProperties *pProperties) {
    (void)physicalDevice;
    if (!pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion       = VK_API_VERSION_1_4;
    pProperties->driverVersion    = VK_MAKE_VERSION(0, 3, 0);
    pProperties->vendorID         = 0x1AF4;  /* Red Hat / virtio */
    pProperties->deviceID         = 0x1050;  /* virtio-gpu */
    pProperties->deviceType       = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    /* deviceName: fixed string 'OsitoK venus virtio-gpu' (NUL-terminated). */
    static const char name[] = "OsitoK venus virtio-gpu";
    unsigned long i;
    for (i = 0; i < sizeof(name) && i < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1; i++)
        pProperties->deviceName[i] = name[i];
    pProperties->deviceName[i] = '\0';
    /* Limits and sparse properties zeroed — W3b fills them. */
}
