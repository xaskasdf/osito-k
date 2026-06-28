#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkBindImageMemory 29u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_BindImageMemory(
        struct venus_wire *w, uint64_t dev_id, uint64_t image_id,
        uint64_t mem_id, uint64_t memory_offset) {
    if (!w) return -22;

    uint8_t cmd[64];
    uint8_t reply[8];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkBindImageMemory);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, image_id);
    vcw_wr_u64(&wr, mem_id);
    vcw_wr_u64(&wr, memory_offset);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VBI] reply cmd=%u vk=%u img=%llu mem=%llu off=%llu\n",
               reply_cmd, vk_result, (unsigned long long)image_id,
               (unsigned long long)mem_id, (unsigned long long)memory_offset);
    }
    if (reply_cmd != VN_CMD_TYPE_vkBindImageMemory)
        return -5;
    return (int)vk_result;
}
