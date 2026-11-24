#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkAllocateCommandBuffers 88u
#define VN_CMD_GENERATE_REPLY 1u
#define VENUS_CB_ALLOC_MAX 32u

extern int printf(const char *, ...);

int venus_cmd_encode_AllocateCommandBuffers(
        struct venus_wire *w, uint64_t dev_id, uint64_t pool_id,
        uint32_t level, uint32_t count,
        uint64_t *out_ids) {
    if (!w || !out_ids || count == 0 || count > VENUS_CB_ALLOC_MAX) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[128u + VENUS_CB_ALLOC_MAX * 8u];
    uint8_t reply[16u + VENUS_CB_ALLOC_MAX * 8u];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };

    uint64_t ids[VENUS_CB_ALLOC_MAX];
    for (uint32_t i = 0; i < count; i++) {
        ids[i] = venus_wire_alloc_object_id(w);
        if (!ids[i]) return -12;
    }

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkAllocateCommandBuffers);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1);                                      /* pAllocateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u64(&wr, pool_id);
    vcw_wr_i32(&wr, (int32_t)level);
    vcw_wr_u32(&wr, count);

    vcw_wr_array_size(&wr, count);                           /* pCommandBuffers */
    for (uint32_t i = 0; i < count; i++) {
        vcw_wr_u64(&wr, ids[i]);
    }
    if (wr.err) return wr.err;

    uint32_t reply_sz = 16u + count * 8u;
    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, reply_sz);
    if (rc < (int)reply_sz) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t reply_count = *(uint64_t *)(reply + 8);
    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VACB] reply cmd=%u vk=%u count=%llu first=%llu bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)reply_count,
               (unsigned long long)(count ? *(uint64_t *)(reply + 16) : 0ull),
               wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkAllocateCommandBuffers)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (reply_count != count)
        return -5;

    for (uint32_t i = 0; i < count; i++)
        out_ids[i] = *(uint64_t *)(reply + 16u + i * 8u);
    return VK_SUCCESS;
}
