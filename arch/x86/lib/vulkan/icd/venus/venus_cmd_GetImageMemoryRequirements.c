/*
 * Encoder for vkGetImageMemoryRequirements over venus.
 *
 * Request payload:
 *   dev_id              (u64)
 *   image_id            (u64)
 *   pReqs_present       (u32)  always 1
 *   pad                 (u32)
 *
 * Reply: flattened VkMemoryRequirements
 *   size                (u64)
 *   alignment           (u64)
 *   memoryTypeBits      (u32)
 *   pad                 (u32)     (24 bytes total).
 */
#include "venus_wire.h"
#include "venus_proto_core.h"
#include "venus.h"

int venus_cmd_encode_GetImageMemoryRequirements(
        struct venus_wire *w, uint64_t dev_id, uint64_t image_id,
        VkMemoryRequirements *out) {
    if (!w || !out) return -22;

    uint64_t reply_id = 0;
    uint8_t *p = venus_wire_alloc_cmd(
            w, VN_CMD_vkGetImageMemoryRequirements,
            VENUS_CMD_FLAG_REPLY_EXPECTED,
            8u + 8u + 4u + 4u,
            &reply_id);
    if (!p) return -12;

    *(uint64_t *)(p + 0)  = dev_id;
    *(uint64_t *)(p + 8)  = image_id;
    *(uint32_t *)(p + 16) = 1u;
    *(uint32_t *)(p + 20) = 0u;

    int rc = venus_wire_submit(w);
    if (rc < 0) return rc;

    uint8_t reply[24] = {0};
    int got = venus_wire_wait_reply(w, reply_id, reply, sizeof(reply));
    if (got < 24) return -5;

    out->size           = *(uint64_t *)(reply + 0);
    out->alignment      = *(uint64_t *)(reply + 8);
    out->memoryTypeBits = *(uint32_t *)(reply + 16);
    return 0;
}
