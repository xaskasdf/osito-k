#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkBeginCommandBuffer 90u

extern int printf(const char *, ...);

int venus_cmd_encode_BeginCommandBuffer(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t flags) {
    if (!w) return -22;
    (void)dev_id;

    uint8_t cmd[64];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkBeginCommandBuffer);
    vcw_wr_u32(&wr, 0);
    vcw_wr_u64(&wr, cb_id);
    vcw_wr_u64(&wr, 1);                                      /* pBeginInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u32(&wr, flags);
    vcw_wr_u64(&wr, 0);                                      /* pInheritanceInfo */
    if (wr.err) return wr.err;

    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VBCB] raw cb=%llu bytes=%u\n",
               (unsigned long long)cb_id, wr.off);
    }
    int rc = venus_wire_submit_raw(w, cmd, wr.off);
    return rc < 0 ? rc : VK_SUCCESS;
}
