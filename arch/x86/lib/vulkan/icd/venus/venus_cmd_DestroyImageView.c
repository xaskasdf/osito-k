/*
 * Encoder for vkDestroyImageView over venus.
 *
 * Request payload:
 *   dev_id             (u64)
 *   view_id            (u64)
 *   pAllocator_present (u32)  always 0
 *   pad                (u32)
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkDestroyImageView 58u

int venus_cmd_encode_DestroyImageView(struct venus_wire *w,
                                      uint64_t dev_id, uint64_t view_id) {
    if (!w) return -22;

    uint8_t cmd[40];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkDestroyImageView);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, view_id);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    if (wr.err) return wr.err;

    return venus_wire_submit_raw(w, cmd, wr.off);
}
