/*
 * Encoder for vkCreatePipelineLayout over venus.
 *
 * W3b.4 subset: empty layout only (no descriptor sets, no push constants).
 * A fuller encoder arrives in W3b.6.
 *
 * Request payload:
 *   dev_id                   (u64)
 *   flags                    (u32)
 *   setLayoutCount           (u32)   must be 0
 *   pushConstantRangeCount   (u32)   must be 0
 *   pad                      (u32)
 *   pAllocator_present       (u32)  always 0
 *   pLayout_present          (u32)  always 1
 *
 * Reply:
 *   VkResult                 (u32)
 *   pad                      (u32)
 *   host VkPipelineLayout id (u64)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_CreatePipelineLayout(
        struct venus_wire *w, uint64_t dev_id,
        const VkPipelineLayoutCreateInfo *pCreateInfo,
        uint64_t *out_layout_id) {
    if (!w || !pCreateInfo || !out_layout_id) return -22;

    /* W3b.4 subset gate. */
    if (pCreateInfo->setLayoutCount != 0 ||
        pCreateInfo->pushConstantRangeCount != 0)
        return 3; /* VK_ERROR_INITIALIZATION_FAILED */

    uint32_t payload = 8u + 4u + 4u + 4u + 4u + 4u + 4u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreatePipelineLayout,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    uint32_t off = 0;
    *(uint64_t *)(p + off) = dev_id;                     off += 8;
    *(uint32_t *)(p + off) = pCreateInfo->flags;         off += 4;
    *(uint32_t *)(p + off) = 0u;                         off += 4;
    *(uint32_t *)(p + off) = 0u;                         off += 4;
    *(uint32_t *)(p + off) = 0u;                         off += 4;
    *(uint32_t *)(p + off) = 0u;                         off += 4;
    *(uint32_t *)(p + off) = 1u;                         off += 4;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_layout_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
