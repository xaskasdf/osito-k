#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkFreeCommandBuffers 89u
#define VENUS_CB_FREE_MAX 32u

int venus_cmd_encode_FreeCommandBuffers(
        struct venus_wire *w, uint64_t dev_id, uint64_t pool_id,
        uint32_t count, const uint64_t *ids) {
    if (!w || count == 0 || count > VENUS_CB_FREE_MAX) return -22;

    uint8_t cmd[64u + VENUS_CB_FREE_MAX * 8u];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkFreeCommandBuffers);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, pool_id);
    vcw_wr_u32(&wr, count);
    vcw_wr_array_size(&wr, count);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t v = ids ? ids[i] : 0ull;
        vcw_wr_u64(&wr, v);
    }
    if (wr.err) return wr.err;

    return venus_wire_submit_raw(w, cmd, wr.off);
}
