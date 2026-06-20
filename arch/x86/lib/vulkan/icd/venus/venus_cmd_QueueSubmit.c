/*
 * Encoder for vkQueueSubmit over venus.
 *
 * Guest-local in W3b.5: when no wire, we simply mark all signal fences
 * + signal semaphores as "signaled" and return VK_SUCCESS. Submitted
 * command buffers were already no-ops in W3b.4 without a wire, so from
 * the app's POV submit → idle is effectively instant.
 *
 * When the wire IS live, we do NOT forward anything in W3b.5 (that's
 * W3b.6). Returning VK_SUCCESS without the host round-trip is fine
 * because the host's view of the device stays consistent as long as
 * all commands were guest-local no-ops.
 *
 * The fence/semaphore lookup uses the same (handle >> 48) & 0x0FFF
 * decoder used by every other W3b.* slot type — see venus_w3b5_objects.c
 * for the helpers.
 *
 * See master plan §W3b.5 T2.
 */
#include "venus.h"
#include "venus_wire.h"
#include "venus_proto_core.h"

extern int printf(const char *, ...);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
static uint32_t w49_logged_copy_exec;
static uint32_t w49_logged_copy_b2i_exec;
static uint32_t w49_logged_sampled_draw_copy;

static int w49_image_has_color(struct venus_memory *m,
                               struct venus_image *img,
                               uint32_t w, uint32_t h) {
    if (!m || !img || !m->local_ptr || w == 0 || h == 0) return 0;
    uint64_t off = img->bound_offset;
    if (off >= m->size) return 0;
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    uint64_t max_pixels = (m->size - off) / 4u;
    if (pixels > max_pixels) pixels = max_pixels;
    const uint32_t *fb = (const uint32_t *)((const uint8_t *)m->local_ptr + off);
    for (uint64_t k = 0; k < pixels; k++) {
        if (fb[k] != 0)
            return 1;
    }
    return 0;
}

/* W4.8/W4.9 — fill image backing with a 32-bit BGRA color. Swapchain
 * images are SHM-backed; intermediate render targets are malloc-backed. */
static void w48_fill_image(struct venus_memory *m,
                           struct venus_image  *img,
                           uint32_t bgra) {
    if (!m || !m->local_ptr) return;
    uint32_t w = m->shm_width  ? m->shm_width  : (img ? img->width  : 0u);
    uint32_t h = m->shm_height ? m->shm_height : (img ? img->height : 0u);
    if (w == 0 || h == 0) return;
    uint64_t off = img ? img->bound_offset : 0u;
    if (off >= m->size) return;
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    uint64_t max_pixels = (m->size - off) / 4u;
    if (pixels > max_pixels) pixels = max_pixels;
    uint32_t *fb = (uint32_t *)((uint8_t *)m->local_ptr + off);
    for (uint64_t k = 0; k < pixels; k++) fb[k] = bgra;
}

static void w49_execute_recorded_copy_image(struct venus_device *dev,
                                            VkCommandBuffer cb_h) {
    if (!dev || !cb_h) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use || !vcb->recorded_has_copy_image) return;

    int src_slot = vcb->recorded_copy_src_image_slot;
    int dst_slot = vcb->recorded_copy_dst_image_slot;
    if (src_slot < 0 || src_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    struct venus_image *src = &dev->images[src_slot];
    struct venus_image *dst = &dev->images[dst_slot];
    if (!src->in_use || !dst->in_use) return;

    int src_mslot = src->bound_mem_slot;
    int dst_mslot = dst->bound_mem_slot;
    if (src_mslot < 0 || src_mslot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    if (dst_mslot < 0 || dst_mslot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    struct venus_memory *src_m = &dev->memories[src_mslot];
    struct venus_memory *dst_m = &dev->memories[dst_mslot];
    if (!src_m->in_use || !dst_m->in_use) return;
    if (!src_m->local_ptr || !dst_m->local_ptr) return;
    if (src->bound_offset >= src_m->size || dst->bound_offset >= dst_m->size)
        return;

    uint32_t sw = src_m->shm_width  ? src_m->shm_width  : src->width;
    uint32_t sh = src_m->shm_height ? src_m->shm_height : src->height;
    uint32_t dw = dst_m->shm_width  ? dst_m->shm_width  : dst->width;
    uint32_t dh = dst_m->shm_height ? dst_m->shm_height : dst->height;
    uint32_t w = sw < dw ? sw : dw;
    uint32_t h = sh < dh ? sh : dh;
    if (w == 0 || h == 0) return;

    if (!w49_logged_copy_exec) {
        w49_logged_copy_exec = 1u;
        printf("[VQ2] CPU image copy src=%d dst=%d %ux%u\n",
               src_slot, dst_slot, w, h);
    }

    uint64_t src_avail = src_m->size - src->bound_offset;
    uint64_t dst_avail = dst_m->size - dst->bound_offset;
    uint64_t src_stride = (uint64_t)sw * 4u;
    uint64_t dst_stride = (uint64_t)dw * 4u;
    uint64_t row_bytes = (uint64_t)w * 4u;
    uint8_t *src_base = (uint8_t *)src_m->local_ptr + src->bound_offset;
    uint8_t *dst_base = (uint8_t *)dst_m->local_ptr + dst->bound_offset;

    for (uint32_t y = 0; y < h; y++) {
        uint64_t src_off = (uint64_t)y * src_stride;
        uint64_t dst_off = (uint64_t)y * dst_stride;
        if (src_off + row_bytes > src_avail) break;
        if (dst_off + row_bytes > dst_avail) break;
        const uint32_t *sp = (const uint32_t *)(src_base + src_off);
        uint32_t *dp = (uint32_t *)(dst_base + dst_off);
        for (uint32_t x = 0; x < w; x++) dp[x] = sp[x];
    }
}

static void w49_execute_recorded_copy_buffer_to_image(struct venus_device *dev,
                                                      VkCommandBuffer cb_h) {
    if (!dev || !cb_h) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use || !vcb->recorded_has_copy_buffer_to_image) return;

    int src_slot = vcb->recorded_copy_src_buffer_slot;
    int dst_slot = vcb->recorded_copy_buffer_dst_image_slot;
    if (src_slot < 0 || src_slot >= (int)VENUS_MAX_BUF_OBJECTS) return;
    if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    struct venus_buffer *src = &dev->buffers[src_slot];
    struct venus_image *dst = &dev->images[dst_slot];
    if (!src->in_use || !dst->in_use) return;

    if (src->bound_mem_slot < 0 || src->bound_mem_slot >= (int)VENUS_MAX_MEM_OBJECTS)
        return;
    if (dst->bound_mem_slot < 0 || dst->bound_mem_slot >= (int)VENUS_MAX_MEM_OBJECTS)
        return;
    struct venus_memory *src_m = &dev->memories[src->bound_mem_slot];
    struct venus_memory *dst_m = &dev->memories[dst->bound_mem_slot];
    if (!src_m->in_use || !dst_m->in_use) return;
    if (!src_m->local_ptr || !dst_m->local_ptr) return;

    uint32_t dw = dst_m->shm_width ? dst_m->shm_width : dst->width;
    uint32_t dh = dst_m->shm_height ? dst_m->shm_height : dst->height;
    uint32_t w = vcb->recorded_copy_buffer_width;
    uint32_t h = vcb->recorded_copy_buffer_height;
    if (w == 0 || w > dw) w = dw;
    if (h == 0 || h > dh) h = dh;
    if (w == 0 || h == 0) return;

    uint32_t row_pixels = vcb->recorded_copy_buffer_row_length;
    if (row_pixels == 0 || row_pixels < w) row_pixels = w;
    uint64_t src_stride = (uint64_t)row_pixels * 4u;
    uint64_t dst_stride = (uint64_t)dw * 4u;
    uint64_t row_bytes = (uint64_t)w * 4u;
    uint64_t src_base_off = src->bound_offset + vcb->recorded_copy_buffer_offset;
    uint64_t dst_base_off = dst->bound_offset;
    if (src_base_off >= src_m->size || dst_base_off >= dst_m->size) return;
    uint64_t src_avail = src_m->size - src_base_off;
    uint64_t dst_avail = dst_m->size - dst_base_off;
    uint8_t *src_base = (uint8_t *)src_m->local_ptr + src_base_off;
    uint8_t *dst_base = (uint8_t *)dst_m->local_ptr + dst_base_off;

    if (!w49_logged_copy_b2i_exec) {
        w49_logged_copy_b2i_exec = 1u;
        printf("[VQ2] CPU buffer->image copy srcbuf=%d dstimg=%d %ux%u\n",
               src_slot, dst_slot, w, h);
    }

    for (uint32_t y = 0; y < h; y++) {
        uint64_t src_off = (uint64_t)y * src_stride;
        uint64_t dst_off = (uint64_t)y * dst_stride;
        if (src_off + row_bytes > src_avail) break;
        if (dst_off + row_bytes > dst_avail) break;
        const uint32_t *sp = (const uint32_t *)(src_base + src_off);
        uint32_t *dp = (uint32_t *)(dst_base + dst_off);
        for (uint32_t x = 0; x < w; x++) dp[x] = sp[x];
    }
}

static void w49_execute_recorded_sampled_draw_copy(struct venus_device *dev,
                                                   VkCommandBuffer cb_h) {
    if (!dev || !cb_h) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use || !vcb->drew_flag) return;

    int src_slot = vcb->recorded_sampled_image_slot;
    int dst_slot = vcb->last_drawn_image_slot;
    if (src_slot < 0 || src_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (dst_slot < 0 || dst_slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    if (src_slot == dst_slot) return;
    struct venus_image *src = &dev->images[src_slot];
    struct venus_image *dst = &dev->images[dst_slot];
    if (!src->in_use || !dst->in_use) return;

    int src_mslot = src->bound_mem_slot;
    int dst_mslot = dst->bound_mem_slot;
    if (src_mslot < 0 || src_mslot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    if (dst_mslot < 0 || dst_mslot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    struct venus_memory *src_m = &dev->memories[src_mslot];
    struct venus_memory *dst_m = &dev->memories[dst_mslot];
    if (!src_m->in_use || !dst_m->in_use) return;
    if (!src_m->local_ptr || !dst_m->local_ptr) return;
    if (src->bound_offset >= src_m->size || dst->bound_offset >= dst_m->size)
        return;

    uint32_t sw = src_m->shm_width ? src_m->shm_width : src->width;
    uint32_t sh = src_m->shm_height ? src_m->shm_height : src->height;
    uint32_t dw = dst_m->shm_width ? dst_m->shm_width : dst->width;
    uint32_t dh = dst_m->shm_height ? dst_m->shm_height : dst->height;
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return;

    uint64_t src_stride = (uint64_t)sw * 4u;
    uint64_t dst_stride = (uint64_t)dw * 4u;
    uint64_t src_avail = src_m->size - src->bound_offset;
    uint64_t dst_avail = dst_m->size - dst->bound_offset;
    uint8_t *src_base = (uint8_t *)src_m->local_ptr + src->bound_offset;
    uint8_t *dst_base = (uint8_t *)dst_m->local_ptr + dst->bound_offset;

    int src_has_color = w49_image_has_color(src_m, src, sw, sh);
    if (w49_logged_sampled_draw_copy < 16u) {
        w49_logged_sampled_draw_copy++;
        printf("[VQ2] CPU sampled draw copy srcimg=%d dstimg=%d %ux%u->%ux%u src_color=%d\n",
               src_slot, dst_slot, sw, sh, dw, dh, src_has_color);
    }

    for (uint32_t y = 0; y < dh; y++) {
        uint32_t sy = (uint32_t)(((uint64_t)y * sh) / dh);
        uint64_t src_off = (uint64_t)sy * src_stride;
        uint64_t dst_off = (uint64_t)y * dst_stride;
        if (src_off + (uint64_t)sw * 4u > src_avail) break;
        if (dst_off + (uint64_t)dw * 4u > dst_avail) break;
        const uint32_t *sp = (const uint32_t *)(src_base + src_off);
        uint32_t *dp = (uint32_t *)(dst_base + dst_off);
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t sx = (uint32_t)(((uint64_t)x * sw) / dw);
            dp[x] = sp[sx];
        }
    }
}

static void w48_execute_recorded_clear(struct venus_device *dev,
                                       VkCommandBuffer cb_h) {
    if (!dev || !cb_h) return;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use || !vcb->recorded_has_clear) return;

    int islot = vcb->recorded_clear_image_slot;
    if (islot < 0) islot = vcb->last_drawn_image_slot;
    if (islot < 0 || islot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    struct venus_image *img = &dev->images[islot];
    if (!img->in_use) return;

    int mslot = img->bound_mem_slot;
    if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    struct venus_memory *m = &dev->memories[mslot];
    if (!m->in_use) return;

    w48_fill_image(m, img, vcb->recorded_clear_color);
}

static void w3b5_signal_semaphore(struct venus_device *dev,
                                  VkSemaphore semaphore) {
    if (!dev || !semaphore) return;
    int slot = (int)(((uint64_t)semaphore >> 48) & VENUS_H_SLOT_MASK_W3B5);
    if (slot >= 0 && slot < (int)VENUS_MAX_SEMA_OBJECTS &&
        dev->semaphores[slot].in_use)
        dev->semaphores[slot].signaled = 1;
}

static void w3b5_signal_fence(struct venus_device *dev,
                              uint64_t fence_handle) {
    if (!dev || !fence_handle) return;
    int fslot = (int)((fence_handle >> 48) & VENUS_H_SLOT_MASK_W3B5);
    if (fslot >= 0 && fslot < (int)VENUS_MAX_FENCE_OBJECTS &&
        dev->fences[fslot].in_use)
        dev->fences[fslot].signaled = 1;
}

int venus_cmd_encode_QueueSubmit(
        struct venus_device *dev,
        uint32_t submitCount, const VkSubmitInfo *pSubmits,
        uint64_t fence_handle) {
    if (!dev) return -22;

    /* W4.8 — execute recorded clears. For each submitted cmd buffer that
     * has a recorded clear color, find the target image's bound memory
     * slot. If it's SHM-backed, fill the buffer with the clear color.
     * Honor recorded_clear_image_slot first (set by CmdClearColorImage
     * AND CmdBeginRenderPass with LOAD_OP_CLEAR), then fall back to
     * last_drawn_image_slot. */
    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo *si = &pSubmits[i];
            if (!si->pCommandBuffers) continue;
            for (uint32_t j = 0; j < si->commandBufferCount; j++) {
                w48_execute_recorded_clear(dev, si->pCommandBuffers[j]);
                w49_execute_recorded_copy_image(dev, si->pCommandBuffers[j]);
                w49_execute_recorded_copy_buffer_to_image(
                        dev, si->pCommandBuffers[j]);
                w49_execute_recorded_sampled_draw_copy(
                        dev, si->pCommandBuffers[j]);
            }
        }
    }

    /* Signal every signal semaphore referenced in each submit. */
    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo *si = &pSubmits[i];
            for (uint32_t j = 0; j < si->signalSemaphoreCount; j++) {
                w3b5_signal_semaphore(dev, si->pSignalSemaphores[j]);
            }
        }
    }

    /* Signal the fence if one was passed. */
    w3b5_signal_fence(dev, fence_handle);
    return 0;
}

int venus_cmd_encode_QueueSubmit2(
        struct venus_device *dev,
        uint32_t submitCount, const VkSubmitInfo2 *pSubmits,
        uint64_t fence_handle) {
    if (!dev) return -22;

    /* DXVK uses vkQueueSubmit2. Mirror the Submit1 guest-local behavior so
     * fences/semaphores and SHM clears stay coherent for synchronization2. */
    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo2 *si = &pSubmits[i];
            if (!si->pCommandBufferInfos) continue;
            for (uint32_t j = 0; j < si->commandBufferInfoCount; j++) {
                VkCommandBuffer cb = si->pCommandBufferInfos[j].commandBuffer;
                w48_execute_recorded_clear(dev, cb);
                w49_execute_recorded_copy_image(dev, cb);
                w49_execute_recorded_copy_buffer_to_image(dev, cb);
                w49_execute_recorded_sampled_draw_copy(dev, cb);
            }
        }
    }

    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo2 *si = &pSubmits[i];
            if (!si->pSignalSemaphoreInfos) continue;
            for (uint32_t j = 0; j < si->signalSemaphoreInfoCount; j++)
                w3b5_signal_semaphore(dev,
                        si->pSignalSemaphoreInfos[j].semaphore);
        }
    }

    w3b5_signal_fence(dev, fence_handle);
    return 0;
}
