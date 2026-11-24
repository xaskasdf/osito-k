#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkEndCommandBuffer 91u

extern int printf(const char *, ...);

int venus_cmd_encode_EndCommandBuffer(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id) {
    if (!w) return -22;
    (void)dev_id;

    uint8_t cmd[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkEndCommandBuffer);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    if (wr.err) return wr.err;

    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VECB] raw cb=%llu bytes=%u\n",
               (unsigned long long)cb_id, wr.off);
    }
    int rc = venus_wire_submit_raw(w, cmd, wr.off);
    return rc < 0 ? rc : VK_SUCCESS;
}
