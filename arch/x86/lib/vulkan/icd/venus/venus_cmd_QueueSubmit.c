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
#include "venus_cmd_writer.h"

extern int printf(const char *, ...);
extern void *malloc(unsigned long);
extern void free(void *);

#define VENUS_H_SLOT_MASK_W3B5    0x0FFFull
#define VN_CMD_TYPE_vkQueueSubmit 18u
#define VN_CMD_TYPE_vkQueueSubmit2 206u
#define VN_CMD_TYPE_vkCopyImageToMemoryMESA 297u
#define VN_CMD_GENERATE_REPLY 1u
#define VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_MESA_W49 1000384008
#define W49_HOST_COPY_REPLY_HEADER_BYTES 16u
static uint32_t w49_logged_copy_exec;
static uint32_t w49_logged_copy_b2i_exec;
static uint32_t w49_logged_sampled_draw_copy;
static uint32_t w49_logged_sampled_draw_fallback;
static uint32_t w49_logged_sampled_draw_host_only;
static uint32_t w49_logged_host_copy_ok;
static uint32_t w49_logged_host_copy_fail;
static uint32_t w49_fallback_frame;
static uint32_t w49_logged_submit_diag;
static uint32_t w49_logged_present_readback;
static uint32_t w49_host_readback_failures;
static uint32_t w49_host_readback_disabled;
static uint32_t w49_host_copy_failures;
static uint32_t w49_host_copy_disabled;
static uint32_t w49_logged_host_copy_disabled;
static uint8_t *w49_host_copy_reply;
static uint64_t w49_host_copy_reply_cap;

static void w49_log_submit_cmd(struct venus_device *dev, VkCommandBuffer cb_h) {
    if (!dev || !cb_h || w49_logged_submit_diag >= 24u) return;

    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use) return;

    int dst_slot = vcb->last_drawn_image_slot;
    int src_slot = vcb->recorded_sampled_image_slot;
    int dst_mslot = -1;
    int src_mslot = -1;
    uint64_t dst_host = 0;
    uint64_t src_host = 0;
    uint32_t dst_shm = 0;
    uint32_t src_shm = 0;

    if (dst_slot >= 0 && dst_slot < (int)VENUS_MAX_IMAGE_OBJECTS) {
        struct venus_image *dst = &dev->images[dst_slot];
        if (dst->in_use) {
            dst_host = dst->host_id;
            dst_mslot = dst->bound_mem_slot;
            if (dst_mslot >= 0 && dst_mslot < (int)VENUS_MAX_MEM_OBJECTS)
                dst_shm = dev->memories[dst_mslot].is_shm_backed;
        }
    }
    if (src_slot >= 0 && src_slot < (int)VENUS_MAX_IMAGE_OBJECTS) {
        struct venus_image *src = &dev->images[src_slot];
        if (src->in_use) {
            src_host = src->host_id;
            src_mslot = src->bound_mem_slot;
            if (src_mslot >= 0 && src_mslot < (int)VENUS_MAX_MEM_OBJECTS)
                src_shm = dev->memories[src_mslot].is_shm_backed;
        }
    }

    w49_logged_submit_diag++;
    printf("[VQ2D] devhost=%llu cbhost=%llu drew=%u dst=%d dsthost=%llu dstmem=%d dstshm=%u src=%d srchost=%llu srcmem=%d srcshm=%u\n",
           (unsigned long long)dev->host_handle,
           (unsigned long long)vcb->host_id,
           vcb->drew_flag, dst_slot, (unsigned long long)dst_host,
           dst_mslot, dst_shm, src_slot, (unsigned long long)src_host,
           src_mslot, src_shm);
}

static int w49_image_has_visible_color(struct venus_memory *m,
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
        if ((fb[k] & 0x00ffffffu) != 0)
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

static void w49_fill_present_fallback(struct venus_memory *m,
                                      struct venus_image *img,
                                      uint32_t frame) {
    if (!m || !img || !m->local_ptr) return;
    uint32_t w = m->shm_width ? m->shm_width : img->width;
    uint32_t h = m->shm_height ? m->shm_height : img->height;
    if (w == 0 || h == 0 || img->bound_offset >= m->size) return;

    uint64_t avail = m->size - img->bound_offset;
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    if (pixels > avail / 4u) pixels = avail / 4u;

    uint32_t *fb = (uint32_t *)((uint8_t *)m->local_ptr + img->bound_offset);
    for (uint64_t p = 0; p < pixels; p++) {
        uint32_t x = (uint32_t)(p % w);
        uint32_t y = (uint32_t)(p / w);
        uint32_t band = ((x / 64u) + (y / 64u) + frame) & 3u;
        uint32_t r = (uint32_t)((x + frame * 7u) & 0xffu);
        uint32_t g = (uint32_t)((y + frame * 5u) & 0xffu);
        uint32_t b = (band == 0u) ? 0xffu : (band == 1u) ? 0x40u : 0x90u;
        fb[p] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
}

static uint8_t *w49_get_host_copy_reply_buf(uint64_t size) {
    if (size == 0) return 0;
    if (w49_host_copy_reply && w49_host_copy_reply_cap >= size)
        return w49_host_copy_reply;

    if (w49_host_copy_reply)
        free(w49_host_copy_reply);

    w49_host_copy_reply = (uint8_t *)malloc((unsigned long)size);
    w49_host_copy_reply_cap = w49_host_copy_reply ? size : 0;
    return w49_host_copy_reply;
}

static int w49_copy_host_image_to_shm(struct venus_device *dev,
                                      struct venus_image *src,
                                      struct venus_memory *dst_m,
                                      struct venus_image *dst,
                                      uint32_t w,
                                      uint32_t h,
                                      VkImageLayout src_layout) {
    if (!dev || !src || !dst_m || !dst) return -22;
    if (w49_host_copy_disabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!dev->parent || !dev->parent->wire || !dev->host_handle) return -22;
    if (!src->host_id || !dst_m->local_ptr) return -22;
    if (w == 0 || h == 0) return -22;

    uint64_t row_bytes = (uint64_t)w * 4u;
    uint64_t data_size = row_bytes * (uint64_t)h;
    uint64_t reply_size = (uint64_t)W49_HOST_COPY_REPLY_HEADER_BYTES + data_size;
    if (reply_size > (uint64_t)VENUS_RING_REPLY_BYTES) return -12;
    if (reply_size > 0xffffffffu) return -12;

    uint8_t *reply = w49_get_host_copy_reply_buf(reply_size);
    if (!reply) return -12;
    for (uint64_t i = 0; i < reply_size; i++)
        reply[i] = 0;

    uint8_t cmd[160];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCopyImageToMemoryMESA);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev->host_handle);
    vcw_wr_u64(&wr, 1); /* pCopyImageToMemoryInfo */
    vcw_wr_i32(&wr, (int32_t)VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_MESA_W49);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u32(&wr, 0); /* flags */
    vcw_wr_u64(&wr, src->host_id);
    vcw_wr_i32(&wr, (int32_t)src_layout);
    vcw_wr_u32(&wr, 0); /* memoryRowLength: tightly packed */
    vcw_wr_u32(&wr, 0); /* memoryImageHeight: tightly packed */
    vcw_wr_u32(&wr, VK_IMAGE_ASPECT_COLOR_BIT);
    vcw_wr_u32(&wr, 0); /* mipLevel */
    vcw_wr_u32(&wr, 0); /* baseArrayLayer */
    vcw_wr_u32(&wr, 1); /* layerCount */
    vcw_wr_i32(&wr, 0); /* imageOffset.x */
    vcw_wr_i32(&wr, 0); /* imageOffset.y */
    vcw_wr_i32(&wr, 0); /* imageOffset.z */
    vcw_wr_u32(&wr, w);
    vcw_wr_u32(&wr, h);
    vcw_wr_u32(&wr, 1);
    vcw_wr_u64(&wr, data_size);
    vcw_wr_array_size(&wr, data_size); /* pData reply blob */
    if (wr.err) return wr.err;

    int got = venus_wire_submit_reply(dev->parent->wire, cmd, wr.off,
                                      reply, (uint32_t)reply_size);
    if (got < (int)W49_HOST_COPY_REPLY_HEADER_BYTES) {
        if (w49_logged_host_copy_fail < 16u) {
            w49_logged_host_copy_fail++;
            printf("[VQ2] host copy reply short rc=%d bytes=%llu\n",
                   got, (unsigned long long)reply_size);
        }
        w49_host_copy_failures++;
        if (got == -110 || w49_host_copy_failures >= 4u) {
            w49_host_copy_disabled = 1u;
            if (w49_logged_host_copy_disabled < 4u) {
                w49_logged_host_copy_disabled++;
                printf("[VQ2] host copy disabled rc=%d failures=%u\n",
                       got, w49_host_copy_failures);
            }
        }
        return got < 0 ? got : -5;
    }

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    int32_t vk_result = *(int32_t *)(reply + 4);
    uint64_t array_size = *(uint64_t *)(reply + 8);
    if (reply_cmd != VN_CMD_TYPE_vkCopyImageToMemoryMESA ||
        vk_result != VK_SUCCESS || array_size < data_size) {
        if (w49_logged_host_copy_fail < 16u) {
            w49_logged_host_copy_fail++;
            printf("[VQ2] host copy failed cmd=%u vk=%d array=%llu need=%llu\n",
                   reply_cmd, vk_result,
                   (unsigned long long)array_size,
                   (unsigned long long)data_size);
        }
        w49_host_copy_failures++;
        if (vk_result == VK_ERROR_FEATURE_NOT_PRESENT ||
            w49_host_copy_failures >= 4u) {
            w49_host_copy_disabled = 1u;
            if (w49_logged_host_copy_disabled < 4u) {
                w49_logged_host_copy_disabled++;
                printf("[VQ2] host copy disabled vk=%d failures=%u\n",
                       vk_result, w49_host_copy_failures);
            }
        }
        return vk_result ? vk_result : -5;
    }

    uint32_t dw = dst_m->shm_width ? dst_m->shm_width : dst->width;
    uint64_t dst_stride = (uint64_t)dw * 4u;
    uint64_t dst_avail = dst_m->size - dst->bound_offset;
    uint8_t *dst_base = (uint8_t *)dst_m->local_ptr + dst->bound_offset;
    const uint8_t *src_base = reply + W49_HOST_COPY_REPLY_HEADER_BYTES;
    for (uint32_t y = 0; y < h; y++) {
        uint64_t dst_off = (uint64_t)y * dst_stride;
        uint64_t src_off = (uint64_t)y * row_bytes;
        if (dst_off + row_bytes > dst_avail) break;
        for (uint64_t x = 0; x < row_bytes; x++)
            dst_base[dst_off + x] = src_base[src_off + x];
    }

    if (w49_logged_host_copy_ok < 16u) {
        w49_logged_host_copy_ok++;
        printf("[VQ2] host copy image src=%llu dstimg=%d %ux%u bytes=%llu\n",
               (unsigned long long)src->host_id,
               (int)(dst - dev->images), w, h,
               (unsigned long long)data_size);
    }
    w49_host_copy_failures = 0;
    return 0;
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

    int src_has_color = w49_image_has_visible_color(src_m, src, sw, sh);
    if (w49_logged_sampled_draw_copy < 16u) {
        w49_logged_sampled_draw_copy++;
        printf("[VQ2] CPU sampled draw copy srcimg=%d dstimg=%d %ux%u->%ux%u src_visible=%d\n",
               src_slot, dst_slot, sw, sh, dw, dh, src_has_color);
    }

    if (!src_has_color && dst_m->is_shm_backed) {
        if (src->host_id != 0 || src_m->host_id != 0) {
            uint32_t cw = sw < dw ? sw : dw;
            uint32_t ch = sh < dh ? sh : dh;
            if (w49_copy_host_image_to_shm(dev, src, dst_m, dst, cw, ch,
                                           VK_IMAGE_LAYOUT_GENERAL) == 0)
                return;
            if (w49_logged_sampled_draw_host_only < 16u) {
                w49_logged_sampled_draw_host_only++;
                printf("[VQ2] sampled draw host-only srcimg=%d srchost=%llu srcmemhost=%llu dstimg=%d; skip CPU pattern\n",
                       src_slot, (unsigned long long)src->host_id,
                       (unsigned long long)src_m->host_id, dst_slot);
            }
            return;
        }
        uint32_t frame = ++w49_fallback_frame;
        w49_fill_present_fallback(dst_m, dst, frame);
        if (w49_logged_sampled_draw_fallback < 16u) {
            w49_logged_sampled_draw_fallback++;
            printf("[VQ2] sampled draw fallback pattern dstimg=%d frame=%u\n",
                   dst_slot, frame);
        }
        return;
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

static void w49_readback_present_target(struct venus_device *dev,
                                        VkCommandBuffer cb_h) {
    if (!dev || !cb_h || w49_host_readback_disabled) return;

    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use || !vcb->drew_flag) return;

    int slot = vcb->last_drawn_image_slot;
    if (slot < 0 || slot >= (int)VENUS_MAX_IMAGE_OBJECTS) return;
    struct venus_image *img = &dev->images[slot];
    if (!img->in_use || !img->is_swapchain_owned || !img->host_id) return;

    int mslot = img->bound_mem_slot;
    if (mslot < 0 || mslot >= (int)VENUS_MAX_MEM_OBJECTS) return;
    struct venus_memory *m = &dev->memories[mslot];
    if (!m->in_use || !m->is_shm_backed || !m->local_ptr) return;

    uint32_t w = m->shm_width ? m->shm_width : img->width;
    uint32_t h = m->shm_height ? m->shm_height : img->height;
    if (img->width && img->width < w) w = img->width;
    if (img->height && img->height < h) h = img->height;
    if (w == 0 || h == 0) return;

    int rc = w49_copy_host_image_to_shm(dev, img, m, img, w, h,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (rc == 0) {
        if (w49_logged_present_readback < 32u) {
            w49_logged_present_readback++;
            printf("[VQ2] present readback image=%d host=%llu %ux%u\n",
                   slot, (unsigned long long)img->host_id, w, h);
        }
        w49_host_readback_failures = 0;
        return;
    }

    if (w49_logged_present_readback < 32u) {
        w49_logged_present_readback++;
        printf("[VQ2] present readback failed image=%d host=%llu rc=%d\n",
               slot, (unsigned long long)img->host_id, rc);
    }

    w49_host_readback_failures++;
    if (rc == VK_ERROR_FEATURE_NOT_PRESENT || rc == -110 ||
        w49_host_readback_failures >= 4u) {
        w49_host_readback_disabled = 1u;
        printf("[VQ2] present readback disabled rc=%d failures=%u\n",
               rc, w49_host_readback_failures);
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

static uint64_t wq_cmd_buffer_host(VkCommandBuffer cb_h) {
    if (!cb_h) return 0;
    struct venus_cmd_buffer *vcb = (struct venus_cmd_buffer *)cb_h;
    if (!vcb->in_use) return 0;
    return vcb->host_id;
}

static uint32_t wq_submit_cmd_count(const VkSubmitInfo *si) {
    uint32_t n = 0;
    if (!si || !si->pCommandBuffers) return 0;
    for (uint32_t i = 0; i < si->commandBufferCount; i++) {
        if (wq_cmd_buffer_host(si->pCommandBuffers[i]))
            n++;
    }
    return n;
}

static uint32_t wq_submit2_cmd_count(const VkSubmitInfo2 *si) {
    uint32_t n = 0;
    if (!si || !si->pCommandBufferInfos) return 0;
    for (uint32_t i = 0; i < si->commandBufferInfoCount; i++) {
        if (wq_cmd_buffer_host(si->pCommandBufferInfos[i].commandBuffer))
            n++;
    }
    return n;
}

static int wq_submit_real(struct venus_device *dev, struct venus_queue *queue,
                          uint32_t submitCount,
                          const VkSubmitInfo *pSubmits) {
    if (!dev || !queue || !dev->parent || !dev->parent->wire)
        return 1;
    if (!queue->host_id)
        return 1;

    uint8_t cmd[8192];
    uint8_t reply[8];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkQueueSubmit);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, queue->host_id);
    vcw_wr_u32(&wr, submitCount);
    if (pSubmits && submitCount) {
        vcw_wr_array_size(&wr, submitCount);
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo *si = &pSubmits[i];
            uint32_t cmd_count = wq_submit_cmd_count(si);
            vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_SUBMIT_INFO);
            vcw_wr_u64(&wr, 0); /* pNext */
            vcw_wr_u32(&wr, 0); /* waitSemaphoreCount */
            vcw_wr_array_size(&wr, 0); /* pWaitSemaphores */
            vcw_wr_array_size(&wr, 0); /* pWaitDstStageMask */
            vcw_wr_u32(&wr, cmd_count);
            vcw_wr_array_size(&wr, cmd_count);
            for (uint32_t j = 0; j < si->commandBufferCount; j++) {
                uint64_t cb = wq_cmd_buffer_host(si->pCommandBuffers[j]);
                if (cb) vcw_wr_u64(&wr, cb);
            }
            vcw_wr_u32(&wr, 0); /* signalSemaphoreCount */
            vcw_wr_array_size(&wr, 0); /* pSignalSemaphores */
        }
    } else {
        vcw_wr_array_size(&wr, 0);
    }
    vcw_wr_u64(&wr, 0); /* fence: guest fences are local today */
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(dev->parent->wire, cmd, wr.off,
                                     reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;
    if (*(uint32_t *)(reply + 0) != VN_CMD_TYPE_vkQueueSubmit)
        return -5;
    return (int)*(int32_t *)(reply + 4);
}

static int wq_submit2_real(struct venus_device *dev, struct venus_queue *queue,
                           uint32_t submitCount,
                           const VkSubmitInfo2 *pSubmits) {
    if (!dev || !queue || !dev->parent || !dev->parent->wire)
        return 1;
    if (!queue->host_id)
        return 1;

    uint8_t cmd[8192];
    uint8_t reply[8];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkQueueSubmit2);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, queue->host_id);
    vcw_wr_u32(&wr, submitCount);
    if (pSubmits && submitCount) {
        vcw_wr_array_size(&wr, submitCount);
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo2 *si = &pSubmits[i];
            uint32_t cmd_count = wq_submit2_cmd_count(si);
            vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
            vcw_wr_u64(&wr, 0); /* pNext */
            vcw_wr_u32(&wr, si->flags);
            vcw_wr_u32(&wr, 0); /* waitSemaphoreInfoCount */
            vcw_wr_array_size(&wr, 0); /* pWaitSemaphoreInfos */
            vcw_wr_u32(&wr, cmd_count);
            vcw_wr_array_size(&wr, cmd_count);
            for (uint32_t j = 0; j < si->commandBufferInfoCount; j++) {
                const VkCommandBufferSubmitInfo *cbi =
                    &si->pCommandBufferInfos[j];
                uint64_t cb = wq_cmd_buffer_host(cbi->commandBuffer);
                if (!cb) continue;
                vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
                vcw_wr_u64(&wr, 0); /* pNext */
                vcw_wr_u64(&wr, cb);
                vcw_wr_u32(&wr, cbi->deviceMask);
            }
            vcw_wr_u32(&wr, 0); /* signalSemaphoreInfoCount */
            vcw_wr_array_size(&wr, 0); /* pSignalSemaphoreInfos */
        }
    } else {
        vcw_wr_array_size(&wr, 0);
    }
    vcw_wr_u64(&wr, 0); /* fence: guest fences are local today */
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(dev->parent->wire, cmd, wr.off,
                                     reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;
    if (*(uint32_t *)(reply + 0) != VN_CMD_TYPE_vkQueueSubmit2)
        return -5;
    return (int)*(int32_t *)(reply + 4);
}

int venus_cmd_encode_QueueSubmit(
        struct venus_device *dev,
        struct venus_queue *queue,
        uint32_t submitCount, const VkSubmitInfo *pSubmits,
        uint64_t fence_handle) {
    if (!dev) return -22;

    int real_rc = wq_submit_real(dev, queue, submitCount, pSubmits);
    int real_submitted = (real_rc == VK_SUCCESS);

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
                VkCommandBuffer cb = si->pCommandBuffers[j];
                w49_log_submit_cmd(dev, cb);
                if (real_submitted) {
                    w49_readback_present_target(dev, cb);
                } else {
                    w48_execute_recorded_clear(dev, cb);
                    w49_execute_recorded_copy_image(dev, cb);
                    w49_execute_recorded_copy_buffer_to_image(dev, cb);
                    w49_execute_recorded_sampled_draw_copy(dev, cb);
                }
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
    return real_rc <= 0 ? real_rc : 0;
}

int venus_cmd_encode_QueueSubmit2(
        struct venus_device *dev,
        struct venus_queue *queue,
        uint32_t submitCount, const VkSubmitInfo2 *pSubmits,
        uint64_t fence_handle) {
    if (!dev) return -22;

    int real_rc = wq_submit2_real(dev, queue, submitCount, pSubmits);
    int real_submitted = (real_rc == VK_SUCCESS);

    /* DXVK uses vkQueueSubmit2. Mirror the Submit1 guest-local behavior so
     * fences/semaphores and SHM clears stay coherent for synchronization2. */
    if (pSubmits) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo2 *si = &pSubmits[i];
            if (!si->pCommandBufferInfos) continue;
            for (uint32_t j = 0; j < si->commandBufferInfoCount; j++) {
                VkCommandBuffer cb = si->pCommandBufferInfos[j].commandBuffer;
                w49_log_submit_cmd(dev, cb);
                if (real_submitted) {
                    w49_readback_present_target(dev, cb);
                } else {
                    w48_execute_recorded_clear(dev, cb);
                    w49_execute_recorded_copy_image(dev, cb);
                    w49_execute_recorded_copy_buffer_to_image(dev, cb);
                    w49_execute_recorded_sampled_draw_copy(dev, cb);
                }
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
    return real_rc <= 0 ? real_rc : 0;
}
