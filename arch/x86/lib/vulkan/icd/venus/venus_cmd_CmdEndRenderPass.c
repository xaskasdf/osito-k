#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCmdEndRenderPass 135u

int venus_cmd_encode_CmdEndRenderPass(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id) {
    if (!w) return -22;
    (void)dev_id;

    uint8_t cmd[16];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdEndRenderPass);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
