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

/* Guest-local fallback used when the wire is absent or a round-trip
 * fails. Matches the W3a hardcoded identity so apps that don't have a
 * real virgl host still see a predictable device. */
static void venus_props_fallback(VkPhysicalDeviceProperties *pProperties) {
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion       = VK_API_VERSION_1_4;
    pProperties->driverVersion    = VK_MAKE_VERSION(0, 3, 0);
    pProperties->vendorID         = 0x1AF4;  /* Red Hat / virtio */
    pProperties->deviceID         = 0x1050;  /* virtio-gpu */
    pProperties->deviceType       = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    static const char name[] = "OsitoK venus virtio-gpu";
    unsigned long i;
    for (i = 0; i < sizeof(name) && i < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1; i++)
        pProperties->deviceName[i] = name[i];
    pProperties->deviceName[i] = '\0';
}

/* W3b.2: real host query, fallback on any wire error. */
extern int venus_cmd_encode_GetPhysicalDeviceProperties(
        struct venus_wire *, uint64_t, VkPhysicalDeviceProperties *);
extern int venus_cmd_encode_GetPhysicalDeviceFeatures(
        struct venus_wire *, uint64_t, VkPhysicalDeviceFeatures *);
extern int venus_cmd_encode_GetPhysicalDeviceQueueFamilyProperties(
        struct venus_wire *, uint64_t, uint32_t *,
        VkQueueFamilyProperties *);
extern int venus_cmd_encode_GetPhysicalDeviceMemoryProperties(
        struct venus_wire *, uint64_t, VkPhysicalDeviceMemoryProperties *);

/* TODO(w3b.3): in the real venus protocol, physical-device handles are
 * distinct from instance handles and are obtained via a separate
 * enumeration handshake. W3a reuses the instance pointer as the
 * VkPhysicalDevice token; here we correspondingly pass the host
 * instance id as `pd_id`, which a real host will likely reject. Until
 * W3b.3 adds proper physical-device-id bookkeeping, expect these wire
 * calls to fail on a virgl host and the guest-local fallback path to
 * fire. The fallback produces spec-legal zero/stub values, so callers
 * don't observe UB. */
VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceProperties *pProperties) {
    if (!pProperties) return;
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire) {
        venus_props_fallback(pProperties);
        return;
    }
    int rc = venus_cmd_encode_GetPhysicalDeviceProperties(
            self->wire, self->host_handle, pProperties);
    if (rc != 0) venus_props_fallback(pProperties);
}

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceFeatures *pFeatures) {
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire) return;   /* zero-features fallback */
    int rc = venus_cmd_encode_GetPhysicalDeviceFeatures(
            self->wire, self->host_handle, pFeatures);
    if (rc != 0) memset(pFeatures, 0, sizeof(*pFeatures));
}

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                             uint32_t *pCount,
                                             VkQueueFamilyProperties *pFamilies) {
    if (!pCount) return;
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire) { *pCount = 0; return; }
    int rc = venus_cmd_encode_GetPhysicalDeviceQueueFamilyProperties(
            self->wire, self->host_handle, pCount, pFamilies);
    if (rc != 0) *pCount = 0;
}

VKAPI_ATTR void VKAPI_CALL
venus_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceMemoryProperties *pMem) {
    if (!pMem) return;
    memset(pMem, 0, sizeof(*pMem));
    struct venus_instance *self = (struct venus_instance *)physicalDevice;
    if (!self || !self->wire) return;   /* count=0/count=0 fallback */
    int rc = venus_cmd_encode_GetPhysicalDeviceMemoryProperties(
            self->wire, self->host_handle, pMem);
    if (rc != 0) memset(pMem, 0, sizeof(*pMem));
}
