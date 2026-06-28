/*
 * Encoder for vkDestroySwapchainKHR. Swapchain images and their memory
 * are implementation-owned in this WSI path. Free the SHM mapping used
 * by the compositor and any host VkImage/VkDeviceMemory backing created
 * for Venus rendering.
 *
 * See master plan §W3b.5 T4.
 */
#include "venus.h"

extern void *memset(void *, int, unsigned long);
extern int   printf(const char *, ...);
extern long  __syscall1(long, long);
extern int venus_cmd_encode_DestroyImage(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_FreeMemory(struct venus_wire *, uint64_t, uint64_t);

#define SYS_SHM_UNMAP    502L
#define SYS_SHM_DESTROY  503L

static uint32_t venus_destroy_sc_log_count;

int venus_cmd_encode_DestroySwapchainKHR(struct venus_device *dev, int sc_slot) {
    if (!dev) return -22;
    if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) return -22;
    struct venus_swapchain *sc = &dev->swapchains[sc_slot];
    if (!sc->in_use) return 0;
    if (venus_destroy_sc_log_count < 24u) {
        venus_destroy_sc_log_count++;
        printf("[VSCDST] slot=%d surface=%d images=%u\n",
               sc_slot, sc->surface_slot, sc->image_count);
    }

    for (uint32_t i = 0; i < sc->image_count; i++) {
        int s = sc->image_slots[i];
        if (s >= 0 && s < (int)VENUS_MAX_IMAGE_OBJECTS) {
            struct venus_image *img = &dev->images[s];
            if (img->in_use) {
                if (dev->parent && dev->parent->wire && img->host_id != 0)
                    (void)venus_cmd_encode_DestroyImage(dev->parent->wire,
                                                        dev->host_handle,
                                                        img->host_id);
                memset(img, 0, sizeof(*img));
            }
        }

        int mslot = sc->memory_slots[i];
        uint64_t mem_host = 0;
        if (mslot >= 0 && mslot < (int)VENUS_MAX_MEM_OBJECTS) {
            struct venus_memory *m = &dev->memories[mslot];
            mem_host = m->host_id;
            if (m->in_use && m->is_shm_backed) {
                if (m->local_ptr)
                    (void)__syscall1(SYS_SHM_UNMAP, (long)m->shm_handle);
                if (m->shm_handle)
                    (void)__syscall1(SYS_SHM_DESTROY, (long)m->shm_handle);
            }
            if (dev->parent && dev->parent->wire && mem_host != 0)
                (void)venus_cmd_encode_FreeMemory(dev->parent->wire,
                                                  dev->host_handle, mem_host);
            memset(m, 0, sizeof(*m));
        }
    }
    memset(sc, 0, sizeof(*sc));
    return 0;
}
