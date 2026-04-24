/*
 * Encoder for vkCreateCommandPool over venus.
 *
 * Request payload:
 *   dev_id               (u64)
 *   flags                (u32)
 *   queueFamilyIndex     (u32)
 *   pAllocator_present   (u32)  always 0
 *   pCmdPool_present     (u32)  always 1
 *
 * Reply:
 *   VkResult             (u32)
 *   pad                  (u32)
 *   host VkCommandPool id(u64)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_CreateCommandPool(
        struct venus_wire *w, uint64_t dev_id,
        const VkCommandPoolCreateInfo *pCreateInfo,
        uint64_t *out_pool_id) {
    if (!w || !pCreateInfo || !out_pool_id) return -22;

    uint32_t payload = 8u + 4u + 4u + 4u + 4u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCreateCommandPool,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint32_t *)(p + 8)  = pCreateInfo->flags;
    *(uint32_t *)(p + 12) = pCreateInfo->queueFamilyIndex;
    *(uint32_t *)(p + 16) = 0u;
    *(uint32_t *)(p + 20) = 1u;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[16] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 16) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    *out_pool_id = *(uint64_t *)(reply + 8);
    return (int)vk_result;
}
