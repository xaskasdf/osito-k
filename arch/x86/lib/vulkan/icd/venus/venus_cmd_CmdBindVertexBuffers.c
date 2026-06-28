#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VENUS_VB_MAX_BINDINGS 8u
#define VN_CMD_TYPE_vkCmdBindVertexBuffers 105u

int venus_cmd_encode_CmdBindVertexBuffers(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t first_binding, uint32_t binding_count,
        const uint64_t *buffer_ids, const uint64_t *offsets) {
    if (!w) return -22;
    (void)dev_id;
    if (binding_count > VENUS_VB_MAX_BINDINGS)
        binding_count = VENUS_VB_MAX_BINDINGS;
    if (!buffer_ids) binding_count = 0;

    uint8_t cmd[192];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCmdBindVertexBuffers);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u32(&wr, first_binding);
    vcw_wr_u32(&wr, binding_count);
    vcw_wr_array_size(&wr, binding_count);
    for (uint32_t i = 0; i < binding_count; i++)
        vcw_wr_u64(&wr, buffer_ids[i]);
    vcw_wr_array_size(&wr, offsets ? binding_count : 0);
    for (uint32_t i = 0; offsets && i < binding_count; i++)
        vcw_wr_u64(&wr, offsets[i]);

    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
