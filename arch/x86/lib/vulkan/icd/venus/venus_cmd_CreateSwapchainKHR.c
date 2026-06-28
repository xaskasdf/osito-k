/*
 * Encoder for vkCreateSwapchainKHR. Allocate a swapchain slot, create
 * per-image object slots, and attach two kinds of backing:
 *
 * - a host VkImage/VkDeviceMemory pair, so image views used by Venus
 *   command buffers resolve to real renderer-side objects;
 * - a CPU-visible SHM backbuffer, so vkQueuePresentKHR can still copy
 *   frame data into the compositor surface SHM and flip the surface.
 *
 * Swapchain images do not create compositor windows themselves.
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
extern long  __syscall2(long, long, long);
extern int venus_cmd_encode_DestroySwapchainKHR(struct venus_device *, int);
extern int venus_cmd_encode_CreateImage(struct venus_wire *, uint64_t,
        const VkImageCreateInfo *, uint64_t *);
extern int venus_cmd_encode_GetImageMemoryRequirements(struct venus_wire *,
        uint64_t, uint64_t, VkMemoryRequirements *);
extern int venus_cmd_encode_AllocateMemory(struct venus_wire *, uint64_t,
        uint64_t, uint32_t, uint64_t *);
extern int venus_cmd_encode_BindImageMemory(struct venus_wire *, uint64_t,
        uint64_t, uint64_t, uint64_t);
extern int venus_cmd_encode_DestroyImage(struct venus_wire *, uint64_t, uint64_t);
extern int venus_cmd_encode_FreeMemory(struct venus_wire *, uint64_t, uint64_t);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define SYS_SHM_CREATE            500L
#define SYS_SHM_MAP               501L
#define SYS_SHM_DESTROY           503L
#define SHM_FLAG_CPU_WRITE        (1L << 0)
#define SHM_FLAG_CPU_READ         (1L << 1)
#define SHM_IMAGE_FLAGS           (SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ)

static uint32_t venus_sc_log_image_diag;
static uint32_t venus_sc_recycle_log_count;

static uint32_t venus_sc_pick_memory_type(uint32_t bits) {
    if (bits & (1u << 1)) return 1u;
    for (uint32_t i = 0; i < 32u; i++) {
        if (bits & (1u << i)) return i;
    }
    return 0u;
}

static void venus_sc_make_host_backing(
        struct venus_device *dev,
        const VkSwapchainCreateInfoKHR *pCreateInfo,
        struct venus_swapchain *sc,
        struct venus_image *img,
        struct venus_memory *m,
        uint32_t image_index,
        int img_slot,
        int mem_slot) {
    if (!dev || !pCreateInfo || !sc || !img || !m) return;
    if (!dev->parent || !dev->parent->wire || dev->host_handle == 0) return;

    VkImageCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = (VkFormat)sc->format;
    ci.extent.width = sc->width;
    ci.extent.height = sc->height;
    ci.extent.depth = 1u;
    ci.mipLevels = 1u;
    ci.arrayLayers = 1u;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = pCreateInfo->imageUsage |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = pCreateInfo->imageSharingMode;
    ci.queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount;
    ci.pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    uint64_t image_host = 0;
    int rc = venus_cmd_encode_CreateImage(dev->parent->wire, dev->host_handle,
                                          &ci, &image_host);
    if (rc != VK_SUCCESS || image_host == 0) {
        printf("[VSCH] create host image failed idx=%u img=%d rc=%d\n",
               image_index, img_slot, rc);
        return;
    }

    VkMemoryRequirements reqs;
    memset(&reqs, 0, sizeof(reqs));
    rc = venus_cmd_encode_GetImageMemoryRequirements(dev->parent->wire,
            dev->host_handle, image_host, &reqs);
    if (rc != 0 || reqs.size == 0 || reqs.memoryTypeBits == 0) {
        printf("[VSCH] req failed idx=%u img=%d host=%llu rc=%d size=%llu bits=0x%x\n",
               image_index, img_slot, (unsigned long long)image_host, rc,
               (unsigned long long)reqs.size, reqs.memoryTypeBits);
        (void)venus_cmd_encode_DestroyImage(dev->parent->wire,
                                            dev->host_handle, image_host);
        return;
    }

    uint64_t mem_host = 0;
    uint32_t type_index = venus_sc_pick_memory_type(reqs.memoryTypeBits);
    rc = venus_cmd_encode_AllocateMemory(dev->parent->wire, dev->host_handle,
                                         reqs.size, type_index, &mem_host);
    if (rc != VK_SUCCESS || mem_host == 0) {
        printf("[VSCH] alloc failed idx=%u mem=%d hostimg=%llu rc=%d size=%llu type=%u bits=0x%x\n",
               image_index, mem_slot, (unsigned long long)image_host, rc,
               (unsigned long long)reqs.size, type_index, reqs.memoryTypeBits);
        (void)venus_cmd_encode_DestroyImage(dev->parent->wire,
                                            dev->host_handle, image_host);
        return;
    }

    rc = venus_cmd_encode_BindImageMemory(dev->parent->wire, dev->host_handle,
                                          image_host, mem_host, 0);
    if (rc != VK_SUCCESS) {
        printf("[VSCH] bind failed idx=%u img=%d mem=%d img_host=%llu mem_host=%llu rc=%d\n",
               image_index, img_slot, mem_slot,
               (unsigned long long)image_host,
               (unsigned long long)mem_host, rc);
        (void)venus_cmd_encode_FreeMemory(dev->parent->wire,
                                          dev->host_handle, mem_host);
        (void)venus_cmd_encode_DestroyImage(dev->parent->wire,
                                            dev->host_handle, image_host);
        return;
    }

    img->host_id = image_host;
    m->host_id = mem_host;
    m->type_index = type_index;
    if (venus_sc_log_image_diag < 32u) {
        printf("[VSCH] image idx=%u img=%d host=%llu mem=%d memhost=%llu req=%llu bits=0x%x type=%u\n",
               image_index, img_slot, (unsigned long long)img->host_id,
               mem_slot, (unsigned long long)m->host_id,
               (unsigned long long)reqs.size, reqs.memoryTypeBits, type_index);
    }
}

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

    int surface_slot = -1;
    {
        int ssl = (int)(((uint64_t)pCreateInfo->surface >> 48) & VENUS_H_SLOT_MASK_W3B5);
        surface_slot = (ssl >= 0 && ssl < (int)VENUS_MAX_SURFACE_OBJECTS) ? ssl : -1;
    }

    if (pCreateInfo->oldSwapchain) {
        int old_slot = (int)(((uint64_t)pCreateInfo->oldSwapchain >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (old_slot >= 0 && old_slot < (int)VENUS_MAX_SWAPCHAIN_OBJECTS &&
            dev->swapchains[old_slot].in_use) {
            if (venus_sc_recycle_log_count < 24u) {
                venus_sc_recycle_log_count++;
                printf("[VSCR] old handle slot=%d surface=%d\n",
                       old_slot, dev->swapchains[old_slot].surface_slot);
            }
            (void)venus_cmd_encode_DestroySwapchainKHR(dev, old_slot);
        }
    }

    /* DXVK may pass oldSwapchain=NULL while recreating. This WSI backend only
     * supports one live swapchain per compositor surface, so retire stale
     * swapchains for the same surface before reserving a new slot. */
    if (surface_slot >= 0) {
        for (uint32_t i = 0; i < VENUS_MAX_SWAPCHAIN_OBJECTS; i++) {
            if (dev->swapchains[i].in_use &&
                dev->swapchains[i].surface_slot == surface_slot) {
                if (venus_sc_recycle_log_count < 24u) {
                    venus_sc_recycle_log_count++;
                    printf("[VSCR] recycle slot=%u surface=%d\n",
                           i, surface_slot);
                }
                (void)venus_cmd_encode_DestroySwapchainKHR(dev, (int)i);
            }
        }
    }

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
    sc->surface_slot = surface_slot;
    sc->image_count   = image_count;
    sc->current_index = 0;
    sc->width         = pCreateInfo->imageExtent.width;
    sc->height        = pCreateInfo->imageExtent.height;
    sc->format        = (uint32_t)pCreateInfo->imageFormat;

    /* Allocate image slots (one per swapchain image). The image remains
     * swapchain-owned, but it needs host backing because DXVK renders into
     * swapchain image views directly via dynamic rendering. */
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

        /* W4.8: pre-bind a CPU-visible SHM backbuffer per swapchain image.
         * Do not create scanout surfaces here; the WSI compositor surface
         * is the only visible target. */
        int mem_slot = -1;
        for (uint32_t j = 0; j < VENUS_MAX_MEM_OBJECTS; j++) {
            if (!dev->memories[j].in_use) { mem_slot = (int)j; break; }
        }
        if (mem_slot >= 0) {
            uint64_t bytes = (uint64_t)sc->width * (uint64_t)sc->height * 4u;
            long shm = __syscall2(SYS_SHM_CREATE, (long)bytes, SHM_IMAGE_FLAGS);
            if (shm > 0) {
                long mapped = __syscall1(SYS_SHM_MAP, shm);
                if (mapped != 0) {
                    struct venus_memory *m = &dev->memories[mem_slot];
                    memset(m, 0, sizeof(*m));
                    m->in_use        = 1;
                    m->size          = bytes;
                    m->local_ptr     = (void *)(uintptr_t)mapped;
                    m->is_shm_backed = 1;
                    m->shm_handle    = (uint32_t)shm;
                    m->shm_width     = sc->width;
                    m->shm_height    = sc->height;
                    img->bound_mem_slot = mem_slot;
                    img->bound_offset   = 0;
                    sc->memory_slots[i] = mem_slot;
                    venus_sc_make_host_backing(dev, pCreateInfo, sc,
                                               img, m, i, img_slot, mem_slot);
                    if (venus_sc_log_image_diag < 16u) {
                        venus_sc_log_image_diag++;
                        printf("[VSCD] image idx=%u img=%d imghost=%llu mem=%d memhost=%llu shm=%u %ux%u\n",
                               i, img_slot, (unsigned long long)img->host_id,
                               mem_slot, (unsigned long long)m->host_id,
                               m->shm_handle, sc->width, sc->height);
                    }
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
