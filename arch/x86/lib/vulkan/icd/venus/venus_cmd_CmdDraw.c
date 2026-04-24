/*
 * Encoder for vkCmdDraw over venus.
 *
 * Request payload:
 *   dev_id         (u64)
 *   cb_id          (u64)
 *   vertexCount    (u32)
 *   instanceCount  (u32)
 *   firstVertex    (u32)
 *   firstInstance  (u32)
 *
 * No reply expected.
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_CmdDraw(
        struct venus_wire *w, uint64_t dev_id, uint64_t cb_id,
        uint32_t vertex_count, uint32_t instance_count,
        uint32_t first_vertex, uint32_t first_instance) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkCmdDraw,
            0u /* no reply */,
            8u + 8u + 4u*4u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = cb_id;
    *(uint32_t *)(p + 16) = vertex_count;
    *(uint32_t *)(p + 20) = instance_count;
    *(uint32_t *)(p + 24) = first_vertex;
    *(uint32_t *)(p + 28) = first_instance;

    return venus_wire_submit(w);
}
