/*
 * Encoder for vkCmdBeginRenderPass over venus.
 *
 * Request payload:
 *   dev_id             (u64)
 *   cb_id              (u64)
 *   rp_id              (u64)
 *   fb_id              (u64)
 *   renderArea.offsetX (i32)
 *   renderArea.offsetY (i32)
 *   renderArea.extentW (u32)
 *   renderArea.extentH (u32)
 *   clearValueCount    (u32)
 *   contents           (u32)
 *   clearValues[] u32*4 each (as uints — float bits are identical)
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

extern void *memcpy(void *, const void *, unsigned long);

#define VENUS_CLEAR_MAX 4u

int venus_cmd_encode_CmdBeginRenderPass(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint64_t rp_id, uint64_t fb_id,
        int32_t area_x, int32_t area_y, uint32_t area_w, uint32_t area_h,
        uint32_t clear_count, const VkClearValue *clear_values,
        uint32_t contents) {
    if (!w) return -22;
    if (clear_count > VENUS_CLEAR_MAX) clear_count = VENUS_CLEAR_MAX;

    uint32_t payload = 8u*4u + 4u*6u + clear_count * 16u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCmdBeginRenderPass,
            0u /* no reply */,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;  off += 8;
    *(uint64_t *)(p + off) = cb_id;   off += 8;
    *(uint64_t *)(p + off) = rp_id;   off += 8;
    *(uint64_t *)(p + off) = fb_id;   off += 8;
    *(int32_t  *)(p + off) = area_x;  off += 4;
    *(int32_t  *)(p + off) = area_y;  off += 4;
    *(uint32_t *)(p + off) = area_w;  off += 4;
    *(uint32_t *)(p + off) = area_h;  off += 4;
    *(uint32_t *)(p + off) = clear_count; off += 4;
    *(uint32_t *)(p + off) = contents;    off += 4;
    if (clear_count && clear_values) {
        memcpy(p + off, clear_values, clear_count * 16u);
    }

    return venus_wire_submit(w);
}
