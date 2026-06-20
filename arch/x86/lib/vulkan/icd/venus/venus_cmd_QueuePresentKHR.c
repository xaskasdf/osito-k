/*
 * Encoder for vkQueuePresentKHR.
 *
 * This is the real integration point — it bridges Vulkan swapchain
 * presentation to the OsitoK compositor via SYS_GUI_FLIP (507).
 *
 * For each (swapchain, image_index) pair in pPresentInfo:
 *   1. Look up the swapchain slot.
 *   2. Look up the image slot at image_index.
 *   3. Look up the memory bound to the image (venus_memory slot from
 *      img->bound_mem_slot set in vkBindImageMemory).
 *   4. (W3b.6) Find a cmd buffer that drew into this image's framebuffer,
 *      and if it recorded a CmdDraw + a vertex buffer with vertexCount>=3,
 *      run the CPU-fallback rasterizer on the SHM-mapped framebuffer.
 *   5. If that memory slot contains pixels, copy it into the compositor
 *      surface SHM and issue syscall(SYS_GUI_FLIP=507, surface_shm).
 *   6. If NOT SHM-backed or still empty, silently return VK_SUCCESS.
 *
 * The app picks one flip per vkQueuePresentKHR call. Semaphores are
 * waited on guest-local (no-op, since no real GPU work is pending).
 *
 * See master plan §W3b.5 T5 + W3b.6 §G3 (CPU rasterizer integration).
 */
#include "venus.h"

extern long __syscall1(long, long);
extern int printf(const char *, ...);
extern void vsr_paint_triangle(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                               const float *verts, uint32_t stride_floats,
                               uint32_t bgra_color);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define SYS_GUI_FLIP              507L

/* Magenta in BGRA byte order: B=FF, G=00, R=FF, A=FF.
 * Little-endian u32: bytes [B,G,R,A] = [FF,00,FF,FF] -> 0xFFFF00FF. */
#define VENUS_W3B6_MAGENTA   0xFFFF00FFu

static uint32_t venus_present_logged_raster;
static uint32_t venus_present_logged_empty;
static uint32_t venus_present_logged_no_target;

static int present_surface_has_color(const uint32_t *fb,
                                     uint32_t w, uint32_t h) {
    if (!fb || w == 0 || h == 0) return 0;
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    for (uint64_t k = 0; k < pixels; k++) {
        if (fb[k] != 0)
            return 1;
    }
    return 0;
}

/* Find a command buffer in this device whose last render pass drew into
 * the given image slot. Returns NULL if none. */
static struct venus_cmd_buffer *find_cb_drew_image(struct venus_device *dev,
                                                   int img_slot) {
    if (!dev || img_slot < 0) return 0;
    for (uint32_t i = 0; i < VENUS_MAX_CMD_BUFFER_OBJECTS; i++) {
        struct venus_cmd_buffer *vcb = &dev->cmd_buffers[i];
        if (!vcb->in_use) continue;
        if (!vcb->drew_flag) continue;
        if (vcb->last_drawn_image_slot != img_slot) continue;
        if (vcb->recorded_vertex_count < 3u) continue;
        return vcb;
    }
    return 0;
}

static struct venus_surface *present_target_surface(struct venus_device *dev,
                                                    struct venus_swapchain *sc) {
    if (!dev || !dev->parent || !sc) return 0;
    if (sc->surface_slot < 0 ||
        sc->surface_slot >= (int)VENUS_MAX_SURFACE_OBJECTS)
        return 0;
    struct venus_surface *s = &dev->parent->surfaces[sc->surface_slot];
    if (!s->in_use || !s->shm_handle || !s->target_ptr) return 0;
    return s;
}

static void copy_present_to_surface(struct venus_surface *s,
                                    struct venus_memory *m,
                                    struct venus_image *img) {
    if (!s || !m || !img || !s->target_ptr || !m->local_ptr) return;

    uint32_t sw = m->shm_width  ? m->shm_width  : img->width;
    uint32_t sh = m->shm_height ? m->shm_height : img->height;
    uint32_t dw = s->width ? s->width : sw;
    uint32_t dh = s->height ? s->height : sh;
    if (!sw || !sh || !dw || !dh) return;

    uint32_t cw = (sw < dw) ? sw : dw;
    uint32_t ch = (sh < dh) ? sh : dh;
    uint32_t *dst = (uint32_t *)s->target_ptr;
    const uint32_t *src = (const uint32_t *)((const uint8_t *)m->local_ptr +
                                             img->bound_offset);

    uint64_t dpixels = (uint64_t)dw * (uint64_t)dh;
    for (uint64_t p = 0; p < dpixels; p++)
        dst[p] = 0;

    for (uint32_t y = 0; y < ch; y++) {
        uint32_t *drow = dst + (uint64_t)y * dw;
        const uint32_t *srow = src + (uint64_t)y * sw;
        for (uint32_t x = 0; x < cw; x++)
            drow[x] = srow[x];
    }
}

int venus_cmd_encode_QueuePresentKHR(
        struct venus_device *dev,
        const VkPresentInfoKHR *pPresentInfo) {
    if (!dev || !pPresentInfo) return -22;

    /* Wait semaphores: guest-local — clear the "signaled" bit to
     * model consumption. No actual wait (there's no host GPU work). */
    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
        VkSemaphore sh = pPresentInfo->pWaitSemaphores[i];
        if (!sh) continue;
        int slot = (int)(((uint64_t)sh >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (slot >= 0 && slot < (int)VENUS_MAX_SEMA_OBJECTS &&
            dev->semaphores[slot].in_use)
            dev->semaphores[slot].signaled = 0;
    }

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        VkSwapchainKHR sch = pPresentInfo->pSwapchains[i];
        uint32_t idx = pPresentInfo->pImageIndices[i];
        int sc_slot = (int)(((uint64_t)sch >> 48) & VENUS_H_SLOT_MASK_W3B5);
        if (sc_slot < 0 || sc_slot >= (int)VENUS_MAX_SWAPCHAIN_OBJECTS) continue;
        struct venus_swapchain *sc = &dev->swapchains[sc_slot];
        if (!sc->in_use) continue;
        if (idx >= sc->image_count) continue;
        int img_slot = sc->image_slots[idx];
        if (img_slot < 0 || img_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) continue;
        struct venus_image *img = &dev->images[img_slot];
        if (!img->in_use) continue;
        int mslot = img->bound_mem_slot;
        if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) continue;
        struct venus_memory *m = &dev->memories[mslot];
        if (!m->in_use) continue;

        /* W3b.6 — CPU-fallback rasterizer. Requires SHM-backed dest +
         * a recorded draw + a bound vertex buffer with mapped pointer. */
        if (m->is_shm_backed && m->local_ptr) {
            struct venus_cmd_buffer *vcb = find_cb_drew_image(dev, img_slot);
            if (vcb && vcb->recorded_vb_slot >= 0 &&
                vcb->recorded_vb_slot < (int32_t)VENUS_MAX_BUF_OBJECTS) {
                if (!venus_present_logged_raster) {
                    venus_present_logged_raster = 1u;
                    printf("[VPRES] CPU raster candidate image=%d vb=%d verts=%u\n",
                           img_slot, vcb->recorded_vb_slot,
                           vcb->recorded_vertex_count);
                }
                struct venus_buffer *vb = &dev->buffers[vcb->recorded_vb_slot];
                if (vb->in_use && vb->bound_mem_slot >= 0 &&
                    vb->bound_mem_slot < (int)VENUS_MAX_MEM_OBJECTS) {
                    struct venus_memory *vbm = &dev->memories[vb->bound_mem_slot];
                    if (vbm->in_use && vbm->local_ptr) {
                        const uint8_t *base = (const uint8_t *)vbm->local_ptr
                                            + vb->bound_offset
                                            + vcb->recorded_vb_offset;
                        uint32_t stride = vcb->recorded_vb_stride;
                        if (stride < 12u) stride = 12u;
                        uint32_t stride_floats = stride / 4u;
                        const float *verts = (const float *)base;
                        verts += vcb->recorded_first_vertex * stride_floats;
                        vsr_paint_triangle(
                                (uint32_t *)m->local_ptr,
                                m->shm_width  ? m->shm_width  : img->width,
                                m->shm_height ? m->shm_height : img->height,
                                verts, stride_floats,
                                VENUS_W3B6_MAGENTA);
                    }
                }
            }

            uint32_t w = m->shm_width  ? m->shm_width  : img->width;
            uint32_t h = m->shm_height ? m->shm_height : img->height;
            const uint32_t *present_src =
                    (const uint32_t *)((const uint8_t *)m->local_ptr +
                                       img->bound_offset);
            if (!present_surface_has_color(present_src, w, h)) {
                if (!venus_present_logged_empty) {
                    venus_present_logged_empty = 1u;
                    printf("[VPRES] empty swapchain image=%d -> skip flip\n",
                           img_slot);
                }
                if (pPresentInfo->pResults)
                    pPresentInfo->pResults[i] = VK_SUCCESS;
                continue;
            }
        }

        if (m->is_shm_backed && m->local_ptr) {
            struct venus_surface *surf = present_target_surface(dev, sc);
            if (surf) {
                copy_present_to_surface(surf, m, img);
                (void)__syscall1(SYS_GUI_FLIP, (long)(uint32_t)surf->shm_handle);
            } else if (!venus_present_logged_no_target) {
                venus_present_logged_no_target = 1u;
                printf("[VPRES] no compositor target for swapchain=%d surface=%d\n",
                       sc_slot, sc->surface_slot);
            }
        }
        /* else: memory not SHM-backed — silent skip. No crash. */

        /* Store results[i] if requested. */
        if (pPresentInfo->pResults)
            pPresentInfo->pResults[i] = VK_SUCCESS;
    }
    return 0;
}
