#include "venus_wire.h"
#include "venus_cmd_writer.h"

#define VN_CMD_TYPE_vkCmdEndRendering 214u

int venus_cmd_encode_CmdEndRendering(struct venus_wire *w, uint64_t cb_id) {
    if (!w) return -22;

    uint8_t cmd[32];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdEndRendering);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);

    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
