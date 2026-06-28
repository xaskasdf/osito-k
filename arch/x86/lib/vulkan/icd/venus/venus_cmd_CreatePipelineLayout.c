#include "venus_wire.h"
#include "venus_cmd_writer.h"
#include "venus.h"

#define VN_CMD_TYPE_vkCreatePipelineLayout 68u
#define VN_CMD_GENERATE_REPLY 1u

extern int printf(const char *, ...);

int venus_cmd_encode_CreatePipelineLayout(
        struct venus_wire *w, uint64_t dev_id,
        const VkPipelineLayoutCreateInfo *pCreateInfo,
        const uint64_t *set_layout_ids,
        uint32_t set_layout_count,
        uint64_t *out_layout_id) {
    if (!w || !pCreateInfo || !out_layout_id) return -22;
    if (set_layout_count != pCreateInfo->setLayoutCount) return -22;
    if (pCreateInfo->setLayoutCount && !set_layout_ids) return -22;

    extern uint64_t venus_wire_alloc_object_id(struct venus_wire *);

    uint8_t cmd[4096];
    uint8_t reply[24];
    struct venus_cmd_writer wr = { cmd, 0, sizeof(cmd), 0 };
    uint64_t layout_id = venus_wire_alloc_object_id(w);
    if (!layout_id) return -12;

    vcw_wr_i32(&wr, (int32_t)VN_CMD_TYPE_vkCreatePipelineLayout);
    vcw_wr_u32(&wr, VN_CMD_GENERATE_REPLY);
    vcw_wr_u64(&wr, dev_id);

    vcw_wr_u64(&wr, 1); /* pCreateInfo */
    vcw_wr_i32(&wr, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    vcw_wr_u64(&wr, 0); /* pNext */
    vcw_wr_u32(&wr, pCreateInfo->flags);
    vcw_wr_u32(&wr, pCreateInfo->setLayoutCount);
    vcw_wr_array_size(&wr, pCreateInfo->setLayoutCount);
    for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++)
        vcw_wr_u64(&wr, set_layout_ids[i]);

    vcw_wr_u32(&wr, pCreateInfo->pushConstantRangeCount);
    vcw_wr_array_size(&wr, pCreateInfo->pushConstantRangeCount);
    for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++) {
        const VkPushConstantRange *r = &pCreateInfo->pPushConstantRanges[i];
        vcw_wr_u32(&wr, r->stageFlags);
        vcw_wr_u32(&wr, r->offset);
        vcw_wr_u32(&wr, r->size);
    }

    vcw_wr_u64(&wr, 0); /* pAllocator */
    vcw_wr_u64(&wr, 1); /* pPipelineLayout */
    vcw_wr_u64(&wr, layout_id);
    if (wr.err) return wr.err;

    for (uint32_t i = 0; i < sizeof(reply); i++)
        reply[i] = 0;
    int rc = venus_wire_submit_reply(w, cmd, wr.off, reply, sizeof(reply));
    if (rc < (int)sizeof(reply)) return rc < 0 ? rc : -5;

    uint32_t reply_cmd = *(uint32_t *)(reply + 0);
    uint32_t vk_result = *(uint32_t *)(reply + 4);
    uint64_t present = *(uint64_t *)(reply + 8);
    uint64_t layout_reply = *(uint64_t *)(reply + 16);
    static uint32_t log_count;
    if (log_count < 32u) {
        log_count++;
        printf("[VPLAYOUT] reply cmd=%u vk=%u present=%llu layout=%llu guest=%llu sets=%u push=%u bytes=%u\n",
               reply_cmd, vk_result, (unsigned long long)present,
               (unsigned long long)layout_reply,
               (unsigned long long)layout_id,
               pCreateInfo->setLayoutCount,
               pCreateInfo->pushConstantRangeCount, wr.off);
    }
    if (reply_cmd != VN_CMD_TYPE_vkCreatePipelineLayout)
        return -5;
    if (vk_result != VK_SUCCESS)
        return (int)vk_result;
    if (!present || !layout_reply)
        return -5;

    *out_layout_id = layout_id;
    return VK_SUCCESS;
}
