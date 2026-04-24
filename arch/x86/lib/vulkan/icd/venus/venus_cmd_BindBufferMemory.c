/*
 * Encoder for vkBindBufferMemory over venus.
 *
 * Request payload:
 *   dev_id         (u64)
 *   buf_id         (u64)
 *   mem_id         (u64)
 *   memoryOffset   (u64)
 *
 * Reply:
 *   VkResult       (u32)
 *   pad            (u32)
 *
 * See master plan §W3b.3 T8. */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_BindBufferMemory(
        struct venus_wire *w, uint64_t dev_id, uint64_t buf_id,
        uint64_t mem_id, uint64_t memory_offset) {
    if (!w) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkBindBufferMemory,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 8u + 8u + 8u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = buf_id;
    *(uint64_t *)(p + 16) = mem_id;
    *(uint64_t *)(p + 24) = memory_offset;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[8] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 4) return -5;

    return (int)((uint32_t *)reply)[0];
}
