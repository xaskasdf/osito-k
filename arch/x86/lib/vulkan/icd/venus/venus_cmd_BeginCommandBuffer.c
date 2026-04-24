/*
 * Encoder for vkBeginCommandBuffer over venus.
 *
 * Request payload:
 *   dev_id             (u64)
 *   cb_id              (u64)
 *   flags              (u32)
 *   pInherit_present   (u32)  always 0 (primary)
 *
 * No reply expected in the W3b.4 subset — host treats begin as a
 * side-effect-free state update.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_BeginCommandBuffer(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t flags) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkBeginCommandBuffer,
            0u /* no reply */,
            8u + 8u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = cb_id;
    *(uint32_t *)(p + 16) = flags;
    *(uint32_t *)(p + 20) = 0u;

    return venus_wire_submit(w);
}
