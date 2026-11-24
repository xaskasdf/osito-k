#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCmdDraw 106u

int venus_cmd_encode_CmdDraw(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t vertex_count, uint32_t instance_count,
        uint32_t first_vertex, uint32_t first_instance) {
    if (!w) return -22;
    (void)dev_id;

    uint8_t cmd[32];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdDraw);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u32(&wr, vertex_count);
    vcw_wr_u32(&wr, instance_count);
    vcw_wr_u32(&wr, first_vertex);
    vcw_wr_u32(&wr, first_instance);
    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
