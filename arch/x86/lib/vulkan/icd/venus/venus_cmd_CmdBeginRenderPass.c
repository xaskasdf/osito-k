#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VENUS_CLEAR_MAX 4u
#define VN_CMD_TYPE_vkCmdBeginRenderPass 133u

static void vcw_wr_clear_value(struct venus_cmd_writer *wr,
                               const VkClearValue *clear) {
    vcw_wr_u32(wr, 0); /* VkClearValue union tag: color; preserves raw bytes. */
    vcw_wr_u32(wr, clear->color.uint32[0]);
    vcw_wr_u32(wr, clear->color.uint32[1]);
    vcw_wr_u32(wr, clear->color.uint32[2]);
    vcw_wr_u32(wr, clear->color.uint32[3]);
}

int venus_cmd_encode_CmdBeginRenderPass(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint64_t rp_id, uint64_t fb_id,
        int32_t area_x, int32_t area_y, uint32_t area_w, uint32_t area_h,
        uint32_t clear_count, const VkClearValue *clear_values,
        uint32_t contents) {
    if (!w) return -22;
    (void)dev_id;
    if (clear_count > VENUS_CLEAR_MAX) clear_count = VENUS_CLEAR_MAX;
    if (!clear_values) clear_count = 0;

    uint8_t cmd[192];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdBeginRenderPass);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u64(&wr, 1); /* pRenderPassBegin */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u64(&wr, rp_id);
    vcw_wr_u64(&wr, fb_id);
    vcw_wr_i32(&wr, area_x);
    vcw_wr_i32(&wr, area_y);
    vcw_wr_u32(&wr, area_w);
    vcw_wr_u32(&wr, area_h);
    vcw_wr_u32(&wr, clear_count);
    vcw_wr_array_size(&wr, clear_count);
    for (uint32_t i = 0; i < clear_count; i++)
        vcw_wr_clear_value(&wr, &clear_values[i]);
    vcw_wr_i32(&wr, (int32_t)contents);

    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
