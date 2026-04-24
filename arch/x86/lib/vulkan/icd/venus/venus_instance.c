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
    if (self->ctx_id > 0) {
        (void)__syscall1(VENUS_SYS_GPU_CTX_DESTROY, (long)(uint32_t)self->ctx_id);
        self->ctx_id = 0;
    }
    free(self);
}
