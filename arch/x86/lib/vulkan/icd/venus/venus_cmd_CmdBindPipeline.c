/*
 * Encoder for vkCmdBindPipeline over venus.
 *
 * Request payload:
 *   dev_id        (u64)
 *   cb_id         (u64)
 *   bindPoint     (u32)
 *   pad           (u32)
 *   pipeline_id   (u64)
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_CmdBindPipeline(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t bind_point, uint64_t pipeline_id) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCmdBindPipeline,
            0u /* no reply */,
            8u + 8u + 4u + 4u + 8u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = cb_id;
    *(uint32_t *)(p + 16) = bind_point;
    *(uint32_t *)(p + 20) = 0u;
    *(uint64_t *)(p + 24) = pipeline_id;

    return venus_wire_submit(w);
}
