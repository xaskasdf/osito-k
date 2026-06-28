#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkDestroyPipeline 67u

int venus_cmd_encode_DestroyPipeline(struct venus_wire *w,
                                     uint64_t dev_id, uint64_t pipeline_id) {
    if (!w || !pipeline_id) return -22;

    uint8_t cmd[28];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkDestroyPipeline);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, pipeline_id);
    vcw_wr_u64(&wr, 0); /* pAllocator */
    if (wr.err) return wr.err;

    return venus_wire_submit_raw(w, cmd, wr.off);
}
