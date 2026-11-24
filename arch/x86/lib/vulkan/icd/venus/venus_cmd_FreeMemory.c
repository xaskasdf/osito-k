#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkFreeMemory 22u

int venus_cmd_encode_FreeMemory(struct venus_wire *w,
                                uint64_t dev_id, uint64_t mem_id) {
    if (!w) return -22;

    uint8_t cmd[48];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkFreeMemory);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, mem_id);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    if (wr.err) return wr.err;
    return venus_wire_submit_raw(w, cmd, wr.off);
}
