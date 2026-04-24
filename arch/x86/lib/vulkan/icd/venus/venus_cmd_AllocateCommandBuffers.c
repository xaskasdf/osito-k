/*
 * Encoder for vkAllocateCommandBuffers over venus.
 *
 * Request payload:
 *   dev_id              (u64)
 *   pool_id             (u64)
 *   level               (u32)
 *   commandBufferCount  (u32)
 *
 * Reply:
 *   VkResult            (u32)
 *   pad                 (u32)
 *   commandBufferCount * u64  (host cmd buffer ids)
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_CB_ALLOC_MAX 32u

int venus_cmd_encode_AllocateCommandBuffers(
        struct venus_wire *w, uint64_t dev_id, uint64_t pool_id,
        uint32_t level, uint32_t count,
        uint64_t *out_ids) {
    if (!w || !out_ids || count == 0 || count > VENUS_CB_ALLOC_MAX) return -22;

    uint32_t payload = 8u + 8u + 4u + 4u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkAllocateCommandBuffers,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            payload, &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = pool_id;
    *(uint32_t *)(p + 16) = level;
    *(uint32_t *)(p + 20) = count;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint32_t reply_sz = 8u + count * 8u;
    uint8_t reply[8u + VENUS_CB_ALLOC_MAX * 8u] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, reply_sz);
    if (got < (int)reply_sz) return -5;

    uint32_t vk_result = ((uint32_t *)reply)[0];
    for (uint32_t i = 0; i < count; i++) {
        out_ids[i] = *(uint64_t *)(reply + 8u + i * 8u);
    }
    return (int)vk_result;
}
