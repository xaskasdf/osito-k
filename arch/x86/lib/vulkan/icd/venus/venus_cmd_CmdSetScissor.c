#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VENUS_SC_MAX 4u
#define VN_CMD_TYPE_vkCmdSetScissor 95u

int venus_cmd_encode_CmdSetScissor(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t first_scissor, uint32_t scissor_count,
        const VkRect2D *scissors) {
    if (!w) return -22;
    (void)dev_id;
    if (scissor_count > VENUS_SC_MAX) scissor_count = VENUS_SC_MAX;
    if (!scissors) scissor_count = 0;

    uint8_t cmd[96];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdSetScissor);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u32(&wr, first_scissor);
    vcw_wr_u32(&wr, scissor_count);
    vcw_wr_array_size(&wr, scissor_count);
    for (uint32_t i = 0; i < scissor_count; i++) {
        vcw_wr_i32(&wr, scissors[i].offset.x);
        vcw_wr_i32(&wr, scissors[i].offset.y);
        vcw_wr_u32(&wr, scissors[i].extent.width);
        vcw_wr_u32(&wr, scissors[i].extent.height);
    }

    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
