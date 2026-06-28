#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreateCommandPool 85u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_CreateCommandPool(
        struct venus_wire *w, uint64_t dev_id,
        const VkCommandPoolCreateInfo *pCreateInfo,
        uint64_t *out_pool_id) {
    if (!w || !pCreateInfo || !out_pool_id) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[96];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t pool_id = venus_wire_alloc_object_id(w);
    if (!pool_id) return -12;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreateCommandPool);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);
    vcw_wr_u64(&wr, 1);                                      /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    vcw_wr_u64(&wr, 0);                                      /* pNext */
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_u32(&wr, pCreateInfo->queueFamilyIndex);
    vcw_wr_u64(&wr, 0);                                      /* pAllocator */
    vcw_wr_u64(&wr, 1);                                      /* pCommandPool */
    vcw_wr_u64(&wr, pool_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present   = *(uint64_t *)(reply + 8);
    uint64_t pool_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 16u) {
        log_count++;
        printf("[VCP] reply cmd=%u vk=%u present=%llu pool=%llu guest=%llu bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)pool_reply,
               (unsigned long long)pool_id, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreateCommandPool)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !pool_reply)
        return -5;

    *out_pool_id = pool_id;
    return VK_SUCCESS;
}
