#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VENUS_VP_MAX 4u
#define VN_CMD_TYPE_vkCmdSetViewport 94u

static void vcw_wr_f32(struct venus_cmd_writer *wr, float v) {
    vcw_wr_bytes(wr, &v, 4);
}

int venus_cmd_encode_CmdSetViewport(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t first_viewport, uint32_t viewport_count,
        const VkViewport *viewports) {
    if (!w) return -22;
    (void)dev_id;
    if (viewport_count > VENUS_VP_MAX) viewport_count = VENUS_VP_MAX;
    if (!viewports) viewport_count = 0;

    uint8_t cmd[128];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdSetViewport);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u32(&wr, first_viewport);
    vcw_wr_u32(&wr, viewport_count);
    vcw_wr_array_size(&wr, viewport_count);
    for (uint32_t i = 0; i < viewport_count; i++) {
        vcw_wr_f32(&wr, viewports[i].x);
        vcw_wr_f32(&wr, viewports[i].y);
        vcw_wr_f32(&wr, viewports[i].width);
        vcw_wr_f32(&wr, viewports[i].height);
        vcw_wr_f32(&wr, viewports[i].minDepth);
        vcw_wr_f32(&wr, viewports[i].maxDepth);
    }

    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
