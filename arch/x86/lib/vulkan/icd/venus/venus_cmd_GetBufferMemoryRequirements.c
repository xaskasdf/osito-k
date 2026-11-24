#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkGetBufferMemoryRequirements 30u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_GetBufferMemoryRequirements(
        struct venus_wire *w, uint64_t dev_id, uint64_t buf_id,
        VkMemoryRequirements *out) {
    if (!w || !out) return -22;

    uint8_t cmd[48];
    uint8_t reply[32];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkGetBufferMemoryRequirements);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, buf_id);
    vcw_wr_u64(&wr, 1);                                      /* pReqs */
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint64_t present   = *(uint64_t *)(reply + 4);
    if (reply_cmd != VN_CMD_TYPE_vkGetBufferMemoryRequirements || !present)
        return -5;

    out->size           = *(uint64_t *)(reply + 12);
    out->alignment      = *(uint64_t *)(reply + 20);
    out->memoryTypeBits = *(uint32_t *)(reply + 28);

    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VGBMR] buf=%llu size=%llu align=%llu bits=0x%x bytes=%u\n",
               (unsigned long long)buf_id,
               (unsigned long long)out->size,
               (unsigned long long)out->alignment,
               out->memoryTypeBits, wr.off);
    }
    return 0;
}
