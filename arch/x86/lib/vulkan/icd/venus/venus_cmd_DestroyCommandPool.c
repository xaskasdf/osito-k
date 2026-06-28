#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkDestroyCommandPool 86u

int venus_cmd_encode_DestroyCommandPool(struct venus_wire *w,
                                        uint64_t dev_id, uint64_t pool_id) {
    if (!w) return -22;

    uint8_t cmd[40];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkDestroyCommandPool);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, pool_id);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    if (wr.err) return wr.err;

    return venus_wire_submit_raw(w, cmd, wr.off);
}
