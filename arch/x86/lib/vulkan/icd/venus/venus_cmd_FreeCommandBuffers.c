/*
 * Encoder for vkFreeCommandBuffers over venus.
 *
 * Request payload:
 *   dev_id              (u64)
 *   pool_id             (u64)
 *   commandBufferCount  (u32)
 *   pad                 (u32)
 *   ids[] * u64
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

#define VENUS_CB_FREE_MAX 32u

int venus_cmd_encode_FreeCommandBuffers(
        struct venus_wire *w, uint64_t dev_id, uint64_t pool_id,
        uint32_t count, const uint64_t *ids) {
    if (!w || count == 0 || count > VENUS_CB_FREE_MAX) return -22;

    uint32_t payload = 8u + 8u + 4u + 4u + count * 8u;
    payload = (payload + 7u) & ~7u;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkFreeCommandBuffers,
            0u /* no reply */,
            payload, &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = pool_id;
    *(uint32_t *)(p + 16) = count;
    *(uint32_t *)(p + 20) = 0u;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t v = ids ? ids[i] : 0ull;
        *(uint64_t *)(p + 24u + i * 8u) = v;
    }

    return venus_wire_submit(w);
}
