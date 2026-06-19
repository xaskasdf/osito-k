/*
 * Encoder for vkCreateSwapchainKHR. Guest-local: allocate swapchain
 * slot, allocate image slots for each of the minImageCount images,
 * tag each image `is_swapchain_owned = 1`. The image has no bound
 * memory yet — the app is expected to `vkBindImageMemory` a
 * host-visible memory slot to each image AFTER vkGetSwapchainImagesKHR
 * + its own vkAllocateMemory. At bind time the memory slot is
 * upgraded to SHM-backed (see venus_cmd_BindImageMemory.c W3b.5
 * additions).
 *
 * Width/height/format come from pCreateInfo; we don't re-query the
 * surface because the surface already stored them at CreateSurface
 * time. If the caller specified a different imageExtent, we honor
 * the caller's value (Vulkan spec: use the caller's value as long
 * as it's within surface caps; caps were returned as the caller's
 * extent anyway in our guest-local
 * vkGetPhysicalDeviceSurfaceCapabilitiesKHR impl).
 *
 * See master plan §W3b.5 T4.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern void *memset(void *, int, unsigned long);
extern int   printf(const char *, ...);
extern long  __syscall1(long, long);
extern long  __syscall3(long, long, long, long);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define SYS_SHM_MAP               501L
#define SYS_SHM_DESTROY           503L
#define SYS_SHM_MKSURFACE         506L
#define SHM_FMT_BGRA              0x41524742L

int venus_cmd_encode_CreateSwapchainKHR(
        struct venus_device *dev,
        const VkSwapchainCreateInfoKHR *pCreateInfo,
        int *out_slot) {
    if (!dev || !pCreateInfo || !out_slot) {
        printf("[VSCE] invalid args dev=%p ci=%p out=%p\n", dev, (void *)pCreateInfo, (void *)out_slot);
        return -22;
    }

    /* Clamp minImageCount to our capacity. */
    uint32_t image_count = pCreateInfo->minImageCount;
    if (image_count > VENUS_MAX_SWAPCHAIN_IMAGES) image_count = VENUS_MAX_SWAPCHAIN_IMAGES;
    if (image_count == 0) image_count = 2;  /* defensive default */

    /* Allocate a swapchain slot. */
    int sc_slot = -1;
    for (uint32_t i = 0; i < VENUS_MAX_SWAPCHAIN_OBJECTS; i++) {
        if (!dev->swapchains[i].in_use) { sc_slot = (int)i; break; }
    }
    if (sc_slot < 0) {
        printf("[VSCE] no swapchain slots\n");
        return -12;
    }

    struct venus_swapchain *sc = &dev->swapchains[sc_slot];
    memset(sc, 0, sizeof(*sc));
    for (uint32_t i = 0; i < VENUS_MAX_SWAPCHAIN_IMAGES; i++) {
        sc->image_slots[i]  = -1;
        sc->memory_slots[i] = -1;
    }
    sc->in_use = 1;
    /* Resolve surface slot from pCreateInfo->surface. */
    {
        int ssl = (int)(((uint64_t)pCreateInfo->surface >> 48) & VENUS_H_SLOT_MASK_W3B5);
        sc->surface_slot = (ssl >= 0 && ssl < (int)VENUS_MAX_SURFACE_OBJECTS) ? ssl : -1;
    }
    sc->image_count   = image_count;
    sc->current_index = 0;
    sc->width         = pCreateInfo->imageExtent.width;
    sc->height        = pCreateInfo->imageExtent.height;
    sc->format        = (uint32_t)pCreateInfo->imageFormat;

    /* Allocate image slots (one per swapchain image). We DON'T call the
     * existing CreateImage encoder because swapchain-owned images live
     * entirely guest-local in W3b.5 (no host id). */
    for (uint32_t i = 0; i < image_count; i++) {
        int img_slot = -1;
        for (uint32_t j = 0; j < VENUS_MAX_IMAGE_OBJECTS; j++) {
            if (!dev->images[j].in_use) { img_slot = (int)j; break; }
        }
        if (img_slot < 0) {
            printf("[VSCE] no image slot at image %u/%u\n", i, image_count);
            /* Roll back on failure. */
            for (uint32_t k = 0; k < i; k++) {
                int s = sc->image_slots[k];
                if (s >= 0) memset(&dev->images[s], 0, sizeof(dev->images[s]));
            }
            memset(sc, 0, sizeof(*sc));
            return -12;
        }
        struct venus_image *img = &dev->images[img_slot];
        memset(img, 0, sizeof(*img));
        img->in_use             = 1;
        img->width              = sc->width;
        img->height             = sc->height;
        img->format             = sc->format;
        img->bound_mem_slot     = -1;
        img->usage              = pCreateInfo->imageUsage;
        img->is_swapchain_owned = 1;
        sc->image_slots[i] = img_slot;

        /* W4.8: pre-bind a SHM-backed memory slot per swapchain image so
         * Mesa+Zink callers don't have to vkBindImageMemory swapchain
         * images explicitly (the WSI standard says swapchain images come
         * pre-bound). vkCmdClearColorImage + vkQueueSubmit can then fill
         * the SHM directly and vkQueuePresentKHR flips it. */
        int mem_slot = -1;
        for (uint32_t j = 0; j < VENUS_MAX_MEM_OBJECTS; j++) {
            if (!dev->memories[j].in_use) { mem_slot = (int)j; break; }
        }
        if (mem_slot >= 0) {
            long shm = __syscall3(SYS_SHM_MKSURFACE,
                                  (long)sc->width, (long)sc->height,
                                  SHM_FMT_BGRA);
            if (shm > 0) {
                long mapped = __syscall1(SYS_SHM_MAP, shm);
                if (mapped != 0) {
                    struct venus_memory *m = &dev->memories[mem_slot];
                    memset(m, 0, sizeof(*m));
                    m->in_use        = 1;
                    m->size          = (uint64_t)sc->width * sc->height * 4u;
                    m->local_ptr     = (void *)(uintptr_t)mapped;
                    m->is_shm_backed = 1;
                    m->shm_handle    = (uint32_t)shm;
                    m->shm_width     = sc->width;
                    m->shm_height    = sc->height;
                    img->bound_mem_slot = mem_slot;
                    img->bound_offset   = 0;
                    sc->memory_slots[i] = mem_slot;
                } else {
                    printf("[VSCE] shm map failed image=%u handle=%ld\n", i, shm);
                    (void)__syscall1(SYS_SHM_DESTROY, shm);
                }
            }
        } else {
            printf("[VSCE] no memory slot for image %u/%u\n", i, image_count);
        }
    }

    *out_slot = sc_slot;
    printf("[VSCE] done slot=%d images=%u extent=%ux%u fmt=%u\n",
           sc_slot, image_count, sc->width, sc->height, sc->format);
    return 0;
}
